/*
 * cix-init -- the supervisor (ADR-0246).
 *
 * Runs as pid 1 on an installed host. Its entire job is to reap, to
 * hold running workloads alive across a worker restart, and to restart
 * the worker. It does not speak the REST API, does not run recipes,
 * does not touch rtnetlink, and calls nothing that can block
 * indefinitely.
 *
 * It exists because until now cixd was the init, the supervisor, the
 * event loop, the REST API, the container runtime and the executor of
 * unbounded blocking work -- five jobs with five failure profiles
 * sharing one fate. On 2026-09-05 one of them blocked (reading a pty
 * master, confirmed: state S, wchan n_tty_read) and took the other four
 * with it, including the health endpoint whose only job was to report
 * it. Nothing could restart the daemon because it WAS pid 1, and a dead
 * pid 1 is a kernel panic rather than a recovery.
 *
 * Two rules govern every line here, and both are the point of the file:
 *
 *   1. Never return from main(). Returning as pid 1 is
 *      "Attempted to kill init!", a kernel panic (#131). That
 *      constraint moved here from cixd; it did not disappear.
 *
 *   2. Never block indefinitely. Every wait has a timeout, every socket
 *      is non-blocking, and the reap channel is drained rather than
 *      written to synchronously. A supervisor that can hang is the
 *      thing it was built to fix -- and the specific trap is real: a
 *      wedged worker stops reading its reap channel, so a blocking
 *      write into that socket would wedge the supervisor precisely when
 *      the worker most needs restarting.
 */

#include "supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKER_PATH "/bin/cixd"
#define PENDING_MAX 256
#define RESTART_BACKOFF_SECONDS 2
#define LOOP_TIMEOUT_MS 250
#define REASON_MAX 256

static volatile sig_atomic_t g_sigchld;
static volatile sig_atomic_t g_restart_requested;

static pid_t g_worker = -1;
static int g_reap_fd = -1;
static unsigned long g_restarts;
static time_t g_worker_since;
static char g_last_reason[REASON_MAX] = "none";

/*
 * Records reaped while no worker was running, or while the worker's
 * socket was full, waiting to be delivered. A ring: if a worker stays
 * dead long enough to overflow this, the oldest exits are dropped and
 * that is the correct trade -- the alternative is unbounded memory in
 * the one process that must never fail.
 */
static struct supervisor_reap_record g_pending[PENDING_MAX];
static int g_pending_head, g_pending_count, g_pending_dropped;

static void on_sigchld(int sig)
{
	(void)sig;
	g_sigchld = 1;
}

static void on_restart_signal(int sig)
{
	(void)sig;
	g_restart_requested = 1;
}

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);

	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void pending_push(const struct supervisor_reap_record *r)
{
	int slot;

	if (g_pending_count == PENDING_MAX) {
		g_pending_head = (g_pending_head + 1) % PENDING_MAX;
		g_pending_count--;
		g_pending_dropped++;
	}
	slot = (g_pending_head + g_pending_count) % PENDING_MAX;
	g_pending[slot] = *r;
	g_pending_count++;
}

/*
 * Delivers as much of the backlog as the worker will take right now.
 * A full socket is not an error and not a reason to wait: the records
 * stay queued and the next pass tries again.
 */
static void pending_flush(void)
{
	while (g_pending_count > 0 && g_reap_fd >= 0) {
		const struct supervisor_reap_record *r = &g_pending[g_pending_head];
		ssize_t n = send(g_reap_fd, r, sizeof(*r), MSG_DONTWAIT | MSG_NOSIGNAL);

		if (n == (ssize_t)sizeof(*r)) {
			g_pending_head = (g_pending_head + 1) % PENDING_MAX;
			g_pending_count--;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			return; /* worker is not reading right now -- try again later */
		return;         /* channel is broken; the restart path will rebuild it */
	}
}

static pid_t spawn_worker(char *const argv[])
{
	int sv[2];
	pid_t pid;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
		fprintf(stderr, "cix-init: socketpair: %s\n", strerror(errno));
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "cix-init: fork: %s\n", strerror(errno));
		close(sv[0]);
		close(sv[1]);
		return -1;
	}
	if (pid == 0) {
		char numbuf[32];

		close(sv[0]);
		/*
		 * The worker inherits this end by number, so it must survive
		 * execve -- explicitly clear FD_CLOEXEC rather than assuming
		 * a default.
		 */
		(void)fcntl(sv[1], F_SETFD, 0);
		snprintf(numbuf, sizeof(numbuf), "%d", sv[1]);
		setenv(SUPERVISOR_REAP_FD_ENV, numbuf, 1);
		execv(WORKER_PATH, argv);
		fprintf(stderr, "cix-init: execv %s: %s\n", WORKER_PATH, strerror(errno));
		_exit(127);
	}

	close(sv[1]);
	if (g_reap_fd >= 0)
		close(g_reap_fd);
	g_reap_fd = sv[0];
	set_nonblock(g_reap_fd);
	g_worker_since = time(NULL);
	return pid;
}

static void reap_all(void)
{
	for (;;) {
		int status;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		struct supervisor_reap_record rec;

		if (pid <= 0)
			return;

		memset(&rec, 0, sizeof(rec));
		rec.pid = (int32_t)pid;
		if (WIFEXITED(status)) {
			rec.si_code = CLD_EXITED;
			rec.si_status = (int32_t)WEXITSTATUS(status);
		} else if (WIFSIGNALED(status)) {
			rec.si_code = WCOREDUMP(status) ? CLD_DUMPED : CLD_KILLED;
			rec.si_status = (int32_t)WTERMSIG(status);
		} else {
			continue; /* stopped/continued -- not an exit */
		}

		if (pid == g_worker) {
			g_worker = -1;
			if (rec.si_code == CLD_EXITED)
				snprintf(g_last_reason, sizeof(g_last_reason),
				         "worker exited with status %d", (int)rec.si_status);
			else
				snprintf(g_last_reason, sizeof(g_last_reason),
				         "worker killed by signal %d", (int)rec.si_status);
			fprintf(stderr, "cix-init: %s\n", g_last_reason);
		}
		/*
		 * Queued unfiltered, the dead worker's own record included.
		 * Which pids matter is a question that needs the registry,
		 * and the registry is deliberately not here.
		 */
		pending_push(&rec);
	}
}

static int status_listener_open(void)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int on = 1;

	if (fd < 0)
		return -1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(SUPERVISOR_STATUS_PORT);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
		close(fd);
		return -1;
	}
	set_nonblock(fd);
	return fd;
}

/*
 * Answers exactly one question and then closes: is the worker alive,
 * since when, how many times has it been restarted, and why last time.
 * The request is drained and ignored -- there is only one answer, and
 * parsing a request would be the first step toward a control surface
 * this must never grow.
 */
static void status_serve(int lfd)
{
	for (;;) {
		char reqbuf[1024];
		char body[512];
		char resp[1024];
		int cfd = accept(lfd, NULL, NULL);
		int blen, rlen;

		if (cfd < 0)
			return;
		set_nonblock(cfd);
		(void)recv(cfd, reqbuf, sizeof(reqbuf), MSG_DONTWAIT);

		blen = snprintf(body, sizeof(body),
		                "{\"worker_alive\":%s,\"worker_pid\":%ld,"
		                "\"worker_since\":%ld,\"restarts\":%lu,"
		                "\"pending_reap_records\":%d,\"dropped_reap_records\":%d,"
		                "\"last_reason\":\"%s\"}\n",
		                g_worker > 0 ? "true" : "false", (long)g_worker,
		                (long)g_worker_since, g_restarts,
		                g_pending_count, g_pending_dropped, g_last_reason);
		rlen = snprintf(resp, sizeof(resp),
		                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
		                "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
		                blen, body);
		(void)send(cfd, resp, (size_t)rlen, MSG_DONTWAIT | MSG_NOSIGNAL);
		close(cfd);
	}
}

int main(int argc, char **argv)
{
	char *worker_argv[64];
	struct sigaction sa;
	int status_fd;
	time_t last_spawn_attempt = 0;
	int i;

	/*
	 * Everything after "--" is the worker's own command line; the
	 * kernel cmdline reads init=/bin/cix-init -- --init-mode --slot=a
	 * so this is where those arguments come from.
	 */
	(void)argc;
	worker_argv[0] = (char *)"cixd";
	for (i = 1; i < 63 && argv[i] != NULL; i++)
		worker_argv[i] = argv[i];
	worker_argv[i] = NULL;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	/*
	 * SIGUSR1 is stallwatch asking for a restart (ADR-0246 item 2).
	 * pid 1 only receives signals it has actually installed a handler
	 * for -- the kernel discards the rest -- so this handler is what
	 * makes the watchdog able to act at all.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_restart_signal;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);

	status_fd = status_listener_open();
	if (status_fd < 0)
		fprintf(stderr, "cix-init: status port %d unavailable: %s\n",
		        SUPERVISOR_STATUS_PORT, strerror(errno));

	for (;;) {
		struct pollfd pfd[2];
		int nfds = 0;
		int status_idx = -1;

		if (g_sigchld) {
			g_sigchld = 0;
			reap_all();
		}

		if (g_restart_requested) {
			g_restart_requested = 0;
			if (g_worker > 0) {
				snprintf(g_last_reason, sizeof(g_last_reason),
				         "restart requested by watchdog");
				fprintf(stderr, "cix-init: %s -- killing worker %ld\n",
				        g_last_reason, (long)g_worker);
				/*
				 * SIGKILL rather than SIGTERM on purpose: the
				 * request only ever comes from a watchdog that has
				 * already decided the worker is not responding, and
				 * asking a wedged process to shut down politely is
				 * how a restart becomes another hang.
				 */
				kill(g_worker, SIGKILL);
			}
		}

		if (g_worker <= 0) {
			time_t now = time(NULL);

			if (now - last_spawn_attempt >= RESTART_BACKOFF_SECONDS) {
				last_spawn_attempt = now;
				g_worker = spawn_worker(worker_argv);
				if (g_worker > 0) {
					g_restarts++;
					fprintf(stderr, "cix-init: worker started, pid %ld (start %lu)\n",
					        (long)g_worker, g_restarts);
				}
			}
		}

		pending_flush();

		if (status_fd >= 0) {
			pfd[nfds].fd = status_fd;
			pfd[nfds].events = POLLIN;
			pfd[nfds].revents = 0;
			status_idx = nfds;
			nfds++;
		}
		/*
		 * The reap channel is watched for readability only to notice
		 * that the worker closed it. Nothing is ever read from it --
		 * it carries traffic in one direction.
		 */
		if (g_reap_fd >= 0) {
			pfd[nfds].fd = g_reap_fd;
			pfd[nfds].events = POLLIN;
			pfd[nfds].revents = 0;
			nfds++;
		}

		if (poll(pfd, (nfds_t)nfds, LOOP_TIMEOUT_MS) > 0) {
			if (status_idx >= 0 && (pfd[status_idx].revents & POLLIN) != 0)
				status_serve(status_fd);
		}
	}

	/* Not reached, and must not be: returning here is a kernel panic. */
}
