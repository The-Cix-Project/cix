/*
 * Issue #237: a client that stops reading must not stall the daemon.
 *
 * cixd used to write every response straight to the socket with the fd
 * put back into blocking mode, inside its single event loop. That made
 * the control plane hostage to its slowest reader: one unauthenticated
 * client asking for a large response and then not reading it held every
 * other request off for as long as it liked. Measured on a real host
 * before the fix, health went from 7 ms to a hard timeout for exactly
 * as long as such a client was attached, and recovered in 14 ms the
 * moment it closed.
 *
 * So this test does precisely that, and asserts the daemon keeps
 * answering. It also asserts the other half of the fix: a peer that
 * never resumes reading is eventually dropped, rather than pinning a
 * connection and its buffered response forever.
 *
 * That second half is asserted through the daemon's own log rather than
 * by watching for EOF on the stalled socket, and the reason is worth
 * stating because the obvious test is wrong. A client that never reads
 * advertises a zero receive window, and TCP cannot deliver a FIN
 * through one -- the kernel sits in zero-window probing instead. So the
 * daemon can close the connection and the peer still observes nothing
 * at all. Confirmed on a real host: the drop was recorded ("after
 * 110048 of 1797803 bytes") while the client saw neither EOF nor error.
 * Asserting on EOF would have failed against correct behaviour.
 *
 * The slow client asks for /app.js because it is a real, large asset
 * (hundreds of KB) already served by this daemon -- comfortably beyond
 * what the socket buffers on both ends can absorb, which is what makes
 * the stall deterministic rather than timing-dependent.
 */
#include "httpclient.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7649
#define PORT_ARG "--port=7649"

/*
 * Must exceed cixd's own CONN_OUT_DEADLINE_SECONDS (30) by enough for
 * the once-a-second sweep to run. Kept as its own named constant so the
 * relationship is visible rather than a bare number in a sleep.
 */
#define DEADLINE_WAIT_SECONDS 35

/* The regression bar. Healthy is single-digit ms; anything approaching
 * a second means the loop is being held by someone else's response. */
#define HEALTH_MAX_MS 2000

static int connect_to_daemon(int rcvbuf)
{
	struct sockaddr_in addr;
	struct timeval tv;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;

	if (rcvbuf > 0) {
		/* Deliberately tiny so the daemon's response cannot fit and
		 * the write genuinely has to block or defer. */
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	}

	/* Every read and write in this test is bounded: a wedged daemon
	 * must fail the test, never hang it. */
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(TEST_PORT);
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int send_request(int fd, const char *path)
{
	char req[256];
	int len = snprintf(req, sizeof(req),
	                   "GET %s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", path);

	if (len < 0 || (size_t)len >= sizeof(req))
		return -1;
	return write(fd, req, (size_t)len) == (ssize_t)len ? 0 : -1;
}

static long now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/*
 * One complete GET /v1/health, start to finish, on its own connection.
 * Returns elapsed milliseconds, or -1 if it failed or timed out --
 * which is exactly what the bug looked like from a client.
 */
static long probe_health(void)
{
	char buf[1024];
	long start = now_ms();
	ssize_t n;
	int got_ok = 0;
	int fd = connect_to_daemon(0);

	if (fd < 0)
		return -1;
	if (send_request(fd, "/v1/health") != 0) {
		close(fd);
		return -1;
	}

	for (;;) {
		n = read(fd, buf, sizeof(buf) - 1);
		if (n <= 0)
			break;
		buf[n] = '\0';
		if (strstr(buf, "200") != NULL)
			got_ok = 1;
	}
	close(fd);
	return got_ok ? now_ms() - start : -1;
}

static int sample_health(const char *phase, int count, int *ok)
{
	int i;
	int worst = 0;

	for (i = 0; i < count; i++) {
		long ms = probe_health();

		if (ms < 0) {
			fprintf(stderr, "FAIL: %s: health never answered (sample %d)\n", phase, i + 1);
			*ok = 0;
			return -1;
		}
		if (ms > worst)
			worst = (int)ms;
		if (ms > HEALTH_MAX_MS) {
			fprintf(stderr, "FAIL: %s: health took %ldms (limit %dms) -- the event "
			                "loop is being held by another connection\n",
			        phase, ms, HEALTH_MAX_MS);
			*ok = 0;
		}
		sleep(1);
	}
	printf("  %s: %d samples, worst %dms\n", phase, count, worst);
	return worst;
}

int main(void)
{
	pid_t daemon_pid;
	char *dargv[4];
	char data_dir[PATH_MAX];
	char data_dir_arg[PATH_MAX + 11];
	struct cix_client client;
	int slow_fd;
	int ok = 1;
	int i;

	if (test_data_dir_create(data_dir, sizeof(data_dir)) != 0)
		return 1;
	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", data_dir);

	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	daemon_pid = fork();
	if (daemon_pid < 0) {
		perror("fork");
		test_data_dir_cleanup(data_dir);
		return 1;
	}
	if (daemon_pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
		_exit(127);
	}

	cix_client_init(&client, "127.0.0.1", TEST_PORT);

	for (i = 0; i < 50; i++) {
		if (probe_health() >= 0)
			break;
		usleep(100000);
	}
	if (i == 50) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(data_dir);
		return 1;
	}

	/* 1. Baseline, so a later slowdown can be attributed to the slow
	 *    client rather than to the machine being busy. */
	sample_health("baseline", 2, &ok);

	/* 2. The reproduction: request a large asset, never read a byte. */
	slow_fd = connect_to_daemon(512);
	if (slow_fd < 0 || send_request(slow_fd, "/app.js") != 0) {
		fprintf(stderr, "FAIL: could not open the slow client\n");
		if (slow_fd >= 0)
			close(slow_fd);
		ok = 0;
	} else {
		/* 3. The assertion this test exists for. */
		sample_health("slow client attached", 4, &ok);
		close(slow_fd);
	}

	/* 4. And nothing was broken on the way through. */
	sample_health("slow client gone", 2, &ok);

	/*
	 * 5. The other half: a peer that never resumes must be dropped, or
	 *    its connection and buffered response are pinned forever. The
	 *    daemon closing it shows up here as a clean EOF within the
	 *    read timeout -- a blocking read would time out and fail.
	 */
	slow_fd = connect_to_daemon(512);
	if (slow_fd < 0 || send_request(slow_fd, "/app.js") != 0) {
		fprintf(stderr, "FAIL: could not open the second slow client\n");
		if (slow_fd >= 0)
			close(slow_fd);
		ok = 0;
	} else {
		struct cix_response r;

		printf("  waiting %ds for the write deadline\n", DEADLINE_WAIT_SECONDS);
		sleep(DEADLINE_WAIT_SECONDS);

		if (cix_client_request(&client, "GET", "/v1/system/logs?limit=2000", NULL, &r) != 0) {
			fprintf(stderr, "FAIL: could not read the log store\n");
			ok = 0;
		} else {
			if (r.body == NULL || strstr(r.body, "stopped reading its response") == NULL) {
				fprintf(stderr, "FAIL: a client that never read was not dropped "
				                "after %ds -- the write deadline did not fire\n",
				        DEADLINE_WAIT_SECONDS);
				ok = 0;
			} else {
				printf("  stalled client dropped by the write deadline\n");
			}
			cix_response_free(&r);
		}
		close(slow_fd);
	}

	/* 6. Still serving afterwards. */
	sample_health("after deadline", 2, &ok);

	kill(daemon_pid, SIGTERM);
	{
		int status;
		pid_t w = waitpid(daemon_pid, &status, 0);

		if (w != daemon_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
			ok = 0;
		}
	}

	test_data_dir_cleanup(data_dir);
	printf(ok ? "SLOW CLIENT RESULT: PASS\n" : "SLOW CLIENT RESULT: FAIL\n");
	return ok ? 0 : 1;
}
