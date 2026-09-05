/*
 * test_aggressive -- a HOSTILE test, not a functional one.
 *
 * Every other test in this suite asserts that something works. This one
 * asserts that the control plane cannot be stopped, which is a
 * different question and needs a different shape: there are no
 * scenarios to pass, only a daemon that either kept answering or did
 * not.
 *
 * WHY IT EXISTS
 *
 * cixd is single-threaded. One epoll_wait() loop serves the API, the
 * container runtime and every long operation, so ANY blocking call
 * anywhere in it is a total outage -- no API, no health endpoint, and
 * no way to read the diagnostic that already knows the answer. That is
 * not hypothetical: on 2026-09-05 the daemon on 192.168.15.95 sat in
 * n_tty_read for over seventeen minutes on an idle box and needed a
 * manual reset (#294), and #283 records an earlier one during a C++
 * package build, which may well be a different site entirely.
 *
 * Each of those was found after the fact, from a wchan, one at a time.
 * Reading 47 blocking waitpid() sites and a blocking run_cmd() one by
 * one will never converge. This harness exists to find them by attack
 * rather than by inspection, and to say plainly whether a build is fit
 * to ship.
 *
 * IT WEDGES ITS OWN DAEMON
 *
 * The daemon under attack is this test's own child on its own port and
 * --data-dir, exactly as the other daemon-linked tests do. That keeps
 * production out of the blast radius, and there is a sharper reason
 * than caution: a build's log is relayed THROUGH cixd, so a harness
 * that successfully wedged the daemon it reported through would report
 * nothing at all. That really happened -- the tcc build in flight
 * during the #294 outage left a zero-byte log.
 *
 * THE PROBER IS INDEPENDENT
 *
 * Forked before any attack starts, it asks GET /v1/health every
 * PROBE_INTERVAL_MS on its own connection and records the longest gap
 * between consecutive ANSWERS, along with whichever attack was running
 * at the time. It shares only an mmap with the parent, so nothing it
 * measures depends on the daemon it is measuring.
 *
 * THE VERDICT
 *
 * A gap over FAIL_GAP_MS is a failure. That threshold is deliberately
 * the same as stallwatch's own STALL_THRESHOLD_SECONDS, so the harness
 * and the daemon's own watchdog cannot disagree about what a stall is.
 * The inner daemon's stallwatch records are then read straight off disk
 * -- never through the API, which is exactly the retrieval path that
 * fails when it matters -- and cross-checked against what the prober
 * saw.
 *
 * A harness that has only ever passed proves nothing. This one is
 * verified by reverting the fix it was written against and confirming
 * it reports the wedge.
 */
#include "httpclient.h"
#include "iohelpers.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7699
#define PORT_ARG "--port=7699"

/* How often the prober asks, and how long it waits for an answer. */
#define PROBE_INTERVAL_MS 100
#define PROBE_TIMEOUT_MS 2000

/*
 * The line between "busy" and "wedged", in milliseconds.
 *
 * Deliberately equal to stallwatch's STALL_THRESHOLD_SECONDS. If the
 * harness were stricter it would fail builds the daemon's own watchdog
 * considers healthy; if it were looser there would be stalls the
 * watchdog reports and the harness calls fine. Neither is a defensible
 * place for a yardstick to sit.
 */
#define FAIL_GAP_MS 5000

#define PROBE_REQUEST "GET /v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"

/*
 * Exit codes, kept distinct on purpose.
 *
 * A harness that cannot set itself up has measured NOTHING, and
 * reporting that as a finding is worse than reporting nothing at all --
 * it invents a wedge that did not happen and sends whoever reads it
 * looking for a bug in the daemon. The first run of this harness did
 * exactly that: a build container with no cgroup filesystem could not
 * create the target container, and the recipe announced that the build
 * had been wedged by its own harness.
 */
#define EXIT_FIT 0       /* attacked, and the control plane held */
#define EXIT_FINDING 1   /* attacked, and it did not */
#define EXIT_NO_RUN 2    /* never got as far as attacking anything */

#define ATTACK_NAME_MAX 64

struct probe_shared {
	volatile int stop;
	volatile long long max_gap_ms;
	volatile long long probes_sent;
	volatile long long probes_answered;
	volatile int attacks_skipped;
	/* Which attack was running when the worst gap ended -- the whole
	 * point of a hostile test is naming what did it, not that it
	 * happened. */
	char worst_attack[ATTACK_NAME_MAX];
	char current_attack[ATTACK_NAME_MAX];
};

static struct probe_shared *g_shared;
static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static int g_failures;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_attack(const char *name)
{
	snprintf(g_shared->current_attack, ATTACK_NAME_MAX, "%s", name);
	printf("  attack: %s\n", name);
	fflush(stdout);
}

/*
 * A plain TCP connection to the daemon under attack, with a bounded
 * connect. Deliberately not the shared client helper: several attacks
 * below need a socket that is left half-used or abandoned, which is the
 * opposite of what a well-behaved client offers.
 */
static int raw_connect_timeout(int port, int timeout_ms)
{
	struct sockaddr_in addr;
	struct timeval tv;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * One health request on its own connection. Returns 0 when the daemon
 * answered anything at all -- the body does not matter here, only that
 * the loop turned.
 */
static int probe_once(void)
{
	char buf[512];
	ssize_t n;
	int fd = raw_connect_timeout(TEST_PORT, PROBE_TIMEOUT_MS);

	if (fd < 0)
		return -1;
	/* strlen(), never a hand-counted constant. The first version of
	 * this said 62 for a 63-byte request, so every probe sent a header
	 * without its terminator, the daemon correctly waited for the rest
	 * forever, and the harness reported a twelve-second wedge that had
	 * not happened. */
	if (cix_write_all(fd, PROBE_REQUEST, strlen(PROBE_REQUEST)) != 0) {
		close(fd);
		return -1;
	}
	n = read(fd, buf, sizeof(buf));
	close(fd);
	return (n > 0) ? 0 : -1;
}

/*
 * The prober. Runs as its own process for the whole attack sequence and
 * shares nothing with the daemon it watches.
 *
 * It measures the gap between consecutive ANSWERS rather than counting
 * failures: a daemon that refuses ten probes in a row and then answers
 * was unavailable for that whole span, and a count of ten would
 * understate it. Never returns -- _exit() only, so a forked child can
 * never fall out of here and run the parent's code.
 */
static void prober_main(void)
{
	long long last_ok = now_ms();

	while (!g_shared->stop) {
		long long before = now_ms();

		g_shared->probes_sent++;
		if (probe_once() == 0) {
			long long gap = now_ms() - last_ok;

			g_shared->probes_answered++;
			if (gap > g_shared->max_gap_ms) {
				g_shared->max_gap_ms = gap;
				snprintf(g_shared->worst_attack, ATTACK_NAME_MAX, "%s",
				         g_shared->current_attack);
			}
			last_ok = now_ms();
		}
		{
			long long spent = now_ms() - before;
			long long sleep_ms = PROBE_INTERVAL_MS - spent;

			if (sleep_ms > 0)
				usleep((useconds_t)sleep_ms * 1000);
		}
	}
	/* One last reading, so a wedge that never recovers is still
	 * reported as its true length rather than as whatever the last
	 * successful probe happened to see. */
	{
		long long gap = now_ms() - last_ok;

		if (gap > g_shared->max_gap_ms) {
			g_shared->max_gap_ms = gap;
			snprintf(g_shared->worst_attack, ATTACK_NAME_MAX, "%s (never recovered)",
			         g_shared->current_attack);
		}
	}
	_exit(0);
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
		_exit(127);
	}
	return pid;
}

/*
 * Needing SIGKILL is itself a failure and is reported as one. A daemon
 * that will not answer SIGTERM is a daemon whose loop is not turning,
 * which is the exact condition this harness exists to detect -- letting
 * teardown paper over it would hide the finding.
 */
static int stop_daemon(pid_t pid)
{
	int status;
	int i;

	kill(pid, SIGTERM);
	for (i = 0; i < 100; i++) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			return 0;
		usleep(100000);
	}
	fprintf(stderr, "FAIL: daemon did not exit on SIGTERM within 10s -- its loop was not turning\n");
	g_failures++;
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return -1;
}

static int wait_for_daemon(struct cix_client *client, int tries)
{
	struct cix_response r;
	int i;

	for (i = 0; i < tries; i++) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(client, "GET", "/v1/health", NULL, &r) == 0 && r.status == 200)
			return 0;
		usleep(200000);
	}
	return -1;
}

/* A console websocket, upgraded and left open for the caller to abuse. */
static int open_console(const char *container, const char *cmd)
{
	char req[512];
	char resp[2048];
	int rlen;
	ssize_t n;
	int fd = raw_connect_timeout(TEST_PORT, 5000);

	if (fd < 0)
		return -1;
	rlen = snprintf(req, sizeof(req),
	                "GET /v1/containers/%s/console?cmd=%s HTTP/1.1\r\n"
	                "Host: 127.0.0.1\r\n"
	                "Upgrade: websocket\r\n"
	                "Connection: Upgrade\r\n"
	                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
	                "Sec-WebSocket-Version: 13\r\n\r\n",
	                container, cmd);
	if (cix_write_all(fd, req, (size_t)rlen) != 0) {
		close(fd);
		return -1;
	}
	n = read(fd, resp, sizeof(resp) - 1);
	if (n <= 0) {
		close(fd);
		return -1;
	}
	resp[n] = '\0';
	if (strstr(resp, " 101 ") == NULL) {
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * write() everything, draining whatever the peer sends back while
 * waiting for room. Used by the flood, where the socket is deliberately
 * non-blocking and the peer is deliberately chatty.
 */
static int write_all_draining(int fd, const unsigned char *buf, size_t n)
{
	size_t done = 0;
	int spins = 0;

	while (done < n) {
		ssize_t w = write(fd, buf + done, n - done);

		if (w > 0) {
			done += (size_t)w;
			spins = 0;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			unsigned char drain[8192];
			ssize_t d;

			do {
				d = read(fd, drain, sizeof(drain));
			} while (d > 0);
			if (++spins > 2000)
				return -1; /* peer is not moving at all */
			usleep(1000);
			continue;
		}
		return -1;
	}
	return 0;
}

/* A masked binary websocket frame -- what a real client sends. */
static int ws_send_binary(int fd, const unsigned char *payload, size_t len)
{
	unsigned char hdr[14];
	unsigned char mask[4] = { 0x11, 0x22, 0x33, 0x44 };
	unsigned char *masked;
	size_t hlen = 0;
	size_t i;
	int rc;

	hdr[hlen++] = 0x82; /* FIN | binary */
	if (len < 126) {
		hdr[hlen++] = (unsigned char)(0x80 | len);
	} else if (len < 65536) {
		hdr[hlen++] = 0x80 | 126;
		hdr[hlen++] = (unsigned char)(len >> 8);
		hdr[hlen++] = (unsigned char)(len & 0xff);
	} else {
		return -1;
	}
	memcpy(hdr + hlen, mask, 4);
	hlen += 4;

	masked = malloc(len);
	if (masked == NULL)
		return -1;
	for (i = 0; i < len; i++)
		masked[i] = payload[i] ^ mask[i & 3];
	/*
	 * The socket is non-blocking during the flood, so a short write is
	 * ordinary rather than an error. Retried with a drain in between --
	 * the reason it is full is that the daemon is relaying echo back,
	 * and reading is what makes room. Bounded, so a genuinely dead peer
	 * still ends the attack instead of spinning.
	 */
	rc = write_all_draining(fd, hdr, hlen);
	if (rc == 0)
		rc = write_all_draining(fd, masked, len);
	free(masked);
	return rc;
}

/*
 * ATTACK 1 -- terminal input nobody is reading.
 *
 * console_term_child never reads stdin; it reports once and pause()s.
 * So everything written into its pty accumulates in the slave's input
 * buffer, and once that is full the master will not accept another
 * byte. Before #294 the daemon wrote into that master with a blocking
 * cix_write_all() and stopped there, taking the whole control plane
 * with it -- the write-side twin of the read that actually wedged
 * 192.168.15.95.
 *
 * This is the deterministic half of #294. A spurious EPOLLIN cannot be
 * forced from outside; a full slave buffer can, every time.
 *
 * The correct behaviour is not "the daemon accepts it all": it is that
 * the daemon stays responsive either way. Tearing the session down at
 * the buffer bound is a fine outcome. Blocking is not.
 */
static void attack_console_input_flood(void)
{
	unsigned char chunk[4096];
	int fd;
	int i;

	set_attack("console input flood (slave never reads)");
	/*
	 * LINES, not one long run of bytes, and the difference is the whole
	 * attack.
	 *
	 * A pty slave in canonical mode holds an unterminated line in a
	 * 4 KiB buffer and DISCARDS everything past it -- so half a
	 * megabyte of 'A' with no newline never reaches the read queue at
	 * all, write_room never falls to zero, and a blocking master write
	 * never blocks. That is why the first version of this attack could
	 * not wedge a daemon with the #294 fix reverted, and it is a
	 * property of the tty layer rather than anything about cixd.
	 *
	 * Completed lines are what accumulate in the read queue a process
	 * that never calls read() will never drain. Once that fills, the
	 * master stops accepting, and a blocking write stops the event
	 * loop -- which is precisely the failure being hunted.
	 */
	memset(chunk, 'A', sizeof(chunk));
	for (i = 63; i < (int)sizeof(chunk); i += 64)
		chunk[i] = '\n';

	fd = open_console("aggressive", "/bin/console_term_child");
	if (fd < 0) {
		/* Reported, never silent: an attack that did not reach its
		 * target is not the same as one the daemon survived, and a
		 * harness that cannot tell them apart certifies nothing. */
		fprintf(stderr, "    NOT DELIVERED: could not open the console -- this attack ran against"
		                " nothing\n");
		g_shared->attacks_skipped++;
		return;
	}
	/*
	 * Drain what comes back while flooding, because a real terminal
	 * client does.
	 *
	 * Every byte written into a pty in echo mode produces a byte out of
	 * it, and the daemon relays that to this socket. Accepted sockets
	 * are non-blocking (accept4 SOCK_NONBLOCK), so a client that never
	 * reads makes ws_write_frame() fail on a full socket buffer and the
	 * daemon tears the session down -- ending the attack for a reason
	 * that has nothing to do with what it is aiming at. The first
	 * version of this attack did exactly that, which is the likeliest
	 * reason it could not wedge a daemon whose master write blocks.
	 *
	 * So the socket is non-blocking here and drained every iteration.
	 * The input queue is then the only thing that can fill, which is
	 * the point.
	 */
	{
		int fl = fcntl(fd, F_GETFL, 0);

		if (fl >= 0)
			fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	}
	/* Far past any pty buffer and past the daemon's own 64 KiB bound,
	 * so both the kernel's limit and ours are crossed. */
	for (i = 0; i < 128; i++) {
		unsigned char drain[8192];
		ssize_t d;

		if (ws_send_binary(fd, chunk, sizeof(chunk)) != 0)
			break; /* daemon closed on us, which is a legitimate answer */
		do {
			d = read(fd, drain, sizeof(drain));
		} while (d > 0);
	}
	/*
	 * How much actually went in, reported rather than assumed.
	 *
	 * A run of this attack against a daemon with the #294 fix reverted
	 * neither wedged nor complained, and there was no way to tell from
	 * the log whether the bytes reached the pty at all -- so the one
	 * thing the harness most needed to know, it had not recorded. Both
	 * readings matter: a low count means the daemon stopped accepting,
	 * which is itself an answer, and a full count with no effect means
	 * the attack is not landing where it was aimed.
	 */
	fprintf(stderr, "    delivered %d of 128 chunks (%d bytes)\n", i, i * (int)sizeof(chunk));
	usleep(500000);
	close(fd);
}

/*
 * ATTACK 2 -- sessions abandoned by RST, never closed politely.
 *
 * SO_LINGER with a zero timeout makes close() send RST instead of FIN,
 * so the daemon sees the connection vanish mid-session with no
 * websocket close frame and no orderly shutdown. Aimed at the teardown
 * path, which reaps the exec'd child with a blocking
 * waitpid(sess->exec_pid, NULL, 0): a child that is slow to die stops
 * the loop for exactly as long as it takes.
 */
static void attack_abandoned_sessions(void)
{
	struct linger lg;
	int i;

	set_attack("sessions abandoned by RST");
	lg.l_onoff = 1;
	lg.l_linger = 0;

	for (i = 0; i < 12; i++) {
		int fd = open_console("aggressive", "/bin/console_term_child");

		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
		close(fd);
	}
	usleep(500000);
}

/*
 * ATTACK 3 -- slowloris.
 *
 * Many connections that send a partial request header and then nothing.
 * test_slow_client already covers one such client functionally; the
 * question here is different and only a hostile test asks it: whether
 * enough of them together starve the accept loop.
 */
static void attack_slowloris(void)
{
	static const char SLOWLORIS_PARTIAL[] = "GET /v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n";
	int fds[64];
	int n = 0;
	int i;

	set_attack("slowloris (64 half-sent requests)");

	for (i = 0; i < (int)(sizeof(fds) / sizeof(fds[0])); i++) {
		fds[n] = raw_connect_timeout(TEST_PORT, 2000);
		if (fds[n] < 0)
			continue;
		cix_write_all(fds[n], SLOWLORIS_PARTIAL, strlen(SLOWLORIS_PARTIAL));
		n++;
	}
	sleep(3);
	for (i = 0; i < n; i++)
		close(fds[i]);
}

/*
 * ATTACK 4 -- connect and vanish, as fast as possible.
 *
 * No request at all: connect, RST, repeat. Aimed at the accept path and
 * at every per-connection allocation behind it.
 */
static void attack_connect_storm(void)
{
	struct linger lg;
	int i;

	set_attack("connect/RST storm");
	lg.l_onoff = 1;
	lg.l_linger = 0;
	for (i = 0; i < 400; i++) {
		int fd = raw_connect_timeout(TEST_PORT, 1000);

		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
		close(fd);
	}
}

/*
 * ATTACK 5 -- a request body that stops arriving.
 *
 * Content-Length promises more than will ever be sent. Aimed at any
 * read of a body that assumes the rest is coming.
 */
static void attack_truncated_body(void)
{
	static const char TRUNCATED_POST[] =
	    "POST /v1/containers HTTP/1.1\r\nHost: 127.0.0.1\r\n"
	    "Content-Type: application/json\r\nContent-Length: 100000\r\n\r\n{\"name\":\"x\"";
	int fds[16];
	int n = 0;
	int i;

	set_attack("truncated request bodies");
	for (i = 0; i < (int)(sizeof(fds) / sizeof(fds[0])); i++) {
		fds[n] = raw_connect_timeout(TEST_PORT, 2000);
		if (fds[n] < 0)
			continue;
		cix_write_all(fds[n], TRUNCATED_POST, strlen(TRUNCATED_POST));
		n++;
	}
	sleep(3);
	for (i = 0; i < n; i++)
		close(fds[i]);
}

/*
 * The inner daemon's own stallwatch records, read off disk rather than
 * through GET /v1/system/stalls.
 *
 * That is the whole point: the API is exactly the retrieval path that
 * fails when it matters. During the #294 outage stallwatch had the
 * diagnosis five seconds in and wrote twelve records saying so, and not
 * one was reachable, because reading them required the daemon that was
 * blocked.
 */
static void report_inner_stallwatch(void)
{
	char path[PATH_MAX];
	char line[1024];
	FILE *f;
	int count = 0;

	snprintf(path, sizeof(path), "%s/state/control_plane_stalls.jsonl", g_data_dir);
	f = fopen(path, "r");
	if (f == NULL) {
		printf("  stallwatch records: none written (%s)\n", path);
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		count++;
		if (count <= 5)
			printf("    %s", line);
	}
	fclose(f);
	printf("  stallwatch records: %d\n", count);
	if (count > 0) {
		printf("  ^^ the daemon's own watchdog saw this too\n");
		g_failures++;
	}
}

int main(void)
{
	pid_t daemon_pid, prober_pid;
	struct cix_client client;
	struct cix_response r;
	long long max_gap;
	int status;

	g_shared = mmap(NULL, sizeof(*g_shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
	                -1, 0);
	if (g_shared == MAP_FAILED) {
		perror("mmap");
		return EXIT_NO_RUN;
	}
	memset(g_shared, 0, sizeof(*g_shared));
	snprintf(g_shared->current_attack, ATTACK_NAME_MAX, "%s", "startup");

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return EXIT_NO_RUN;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/aggressive/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/aggressive", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return EXIT_NO_RUN;
		}
	}
	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0 ||
	    test_image_fixture_build(g_image_root, "build/console_term_child", "console_term_child") !=
	        0) {
		fprintf(stderr, "NO-RUN: could not stage the attack fixture's binaries\n");
		test_data_dir_cleanup(g_data_dir);
		return EXIT_NO_RUN;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return EXIT_NO_RUN;
	}
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "NO-RUN: the daemon under test never became healthy\n");
		stop_daemon(daemon_pid);
		test_data_dir_cleanup(g_data_dir);
		return EXIT_NO_RUN;
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"aggressive\",\"image\":\"aggressive\",\"userns\":true,"
	                       "\"cmd\":[\"/bin/daemon_child\",\"600\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		/*
		 * Not a finding. A build container composed by `pkg install`
		 * has no cgroup filesystem, so nothing here can create a
		 * container -- the harness needs the hostbuild environment,
		 * the same one the daemon-linked tests in cix-tests run in.
		 */
		fprintf(stderr, "NO-RUN: could not create the target container, status=%d\n", r.status);
		fprintf(stderr, "        this harness needs an environment that can create containers;\n");
		fprintf(stderr, "        nothing was attacked, so nothing is claimed about this build.\n");
		stop_daemon(daemon_pid);
		test_data_dir_cleanup(g_data_dir);
		return EXIT_NO_RUN;
	}

	/*
	 * The prober starts BEFORE the first attack, so the baseline it
	 * measures is a daemon nobody is attacking yet -- a harness whose
	 * first reading is already under load cannot tell a stall from its
	 * own startup.
	 */
	prober_pid = fork();
	if (prober_pid < 0) {
		perror("fork (prober)");
		stop_daemon(daemon_pid);
		test_data_dir_cleanup(g_data_dir);
		return EXIT_NO_RUN;
	}
	if (prober_pid == 0)
		prober_main();

	sleep(1); /* a baseline of an idle daemon, for contrast */
	set_attack("idle baseline");
	sleep(2);

	/*
	 * The baseline is also a check on the prober itself.
	 *
	 * Nothing is attacking the daemon yet, so if the prober has not
	 * had a single answer by now the fault is the prober's and every
	 * number it goes on to produce is meaningless. Without this the
	 * harness reports a wedge that did not happen, which is exactly
	 * what a hand-counted request length caused the first time: 62
	 * bytes of a 63-byte request, no header terminator, six probes,
	 * zero answers, and a confident twelve-second finding.
	 *
	 * A harness must fail loudly when it cannot measure. Reporting on
	 * a daemon it never reached is the one outcome worse than not
	 * running at all.
	 */
	if (g_shared->probes_answered == 0) {
		fprintf(stderr, "NO-RUN: the prober got no answer from an IDLE daemon (%lld sent).\n",
		        g_shared->probes_sent);
		fprintf(stderr, "        Nothing was attacked yet, so this is the harness failing to\n");
		fprintf(stderr, "        measure, not the control plane failing to answer.\n");
		g_shared->stop = 1;
		waitpid(prober_pid, &status, 0);
		stop_daemon(daemon_pid);
		test_data_dir_cleanup(g_data_dir);
		return EXIT_NO_RUN;
	}

	attack_console_input_flood();
	attack_abandoned_sessions();
	attack_slowloris();
	attack_connect_storm();
	attack_truncated_body();

	set_attack("cooldown");
	sleep(2);

	g_shared->stop = 1;
	waitpid(prober_pid, &status, 0);

	max_gap = g_shared->max_gap_ms;
	printf("\n");
	printf("  probes sent/answered : %lld/%lld\n", g_shared->probes_sent,
	       g_shared->probes_answered);
	if (g_shared->attacks_skipped > 0) {
		printf("  attacks NOT DELIVERED: %d -- this run attacked less than it claims to\n",
		       g_shared->attacks_skipped);
		g_failures++;
	}
	printf("  worst gap            : %lld ms\n", max_gap);
	printf("  during               : %s\n",
	       g_shared->worst_attack[0] != '\0' ? g_shared->worst_attack : "(none)");
	report_inner_stallwatch();

	if (max_gap > FAIL_GAP_MS) {
		fprintf(stderr, "FAIL: the control plane stopped answering for %lld ms during: %s\n",
		        max_gap, g_shared->worst_attack);
		g_failures++;
	}

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);

	if (g_failures > 0) {
		printf("AGGRESSIVE TEST: FAIL (%d finding(s)) -- this build is NOT fit to ship\n",
		       g_failures);
		return EXIT_FINDING;
	}
	printf("AGGRESSIVE TEST: PASS -- the control plane answered throughout, worst gap %lld ms\n",
	       max_gap);
	return EXIT_FIT;
}
