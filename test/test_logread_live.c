/*
 * ADR-0342 (#612): a filtered log read does not hold the event loop.
 *
 * Measured on 192.168.15.95, 0.2.57-540, 2026-10-18: a regex matching
 * nothing in a 100 MB store held cixd's one loop for 1533 ms, during
 * which nothing else on the box was answered. This test builds a store
 * of that kind -- eight segments, ~80 MB -- before the daemon starts, and
 * then asserts the four things the decision promises:
 *
 *   1. a read with no filter is still answered at once, on the loop;
 *   2. while a no-match regex read is in flight, health answers within
 *      50 ms, and the read itself still answers 200 with `[]`;
 *   3. a client that leaves has its read killed -- the daemon logs the
 *      helper's end, and a place opens for the next read;
 *   4. beyond 2 running and 16 waiting, the next filtered read answers
 *      503 with Retry-After at once.
 *
 * The store is written by hand, in the daemon's own line format, and the
 * daemon resumes appending to the newest segment (state.json's next_seq),
 * so nothing it writes at startup truncates what was seeded.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"
#include "test_drift.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7691
#define PORT_ARG "--port=7691"
#define SEGMENTS 8
#define SEGMENT_BYTES (10 * 1024 * 1024)
#define NOMATCH "/v1/system/logs?regex=zzznomatchzzz&tail=1"

static char g_data_dir[PATH_MAX];
static int g_failures;

static void fail(const char *what, const char *detail)
{
	fprintf(stderr, "FAIL: %s%s%s\n", what, detail[0] != '\0' ? " -- " : "", detail);
	g_failures++;
}

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* Eight segments of ordinary entries, seq 2..9, and the state that makes
 * the daemon append to seq 9 rather than start a new segment 1. */
static int seed_store(void)
{
	char dir[PATH_MAX + 8], path[PATH_MAX + 64];
	FILE *f;
	long long ts = 1792000000;
	int s;

	snprintf(dir, sizeof(dir), "%s/logs", g_data_dir);
	if (test_mkdir_p(dir) != 0)
		return -1;
	for (s = 2; s < 2 + SEGMENTS; s++) {
		long written = 0;
		int i = 0;

		snprintf(path, sizeof(path), "%s/seg-%020d.jsonl", dir, s);
		f = fopen(path, "w");
		if (f == NULL)
			return -1;
		while (written < SEGMENT_BYTES) {
			int n = fprintf(f,
			                "{\"ts\":%lld,\"source\":\"container\",\"level\":\"info\","
			                "\"container\":\"seeded-%d\",\"msg\":\"seeded line %d of segment %d, "
			                "long enough to look like a real container's output line\"}\n",
			                ts++, i % 50, i, s);

			if (n < 0) {
				fclose(f);
				return -1;
			}
			written += n;
			i++;
		}
		fclose(f);
	}
	snprintf(path, sizeof(path), "%s/state.json", dir);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "{\"max_bytes\":%d,\"min_level\":\"debug\",\"next_seq\":%d}", 100 * 1024 * 1024,
	        2 + SEGMENTS);
	fclose(f);
	return 0;
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
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
		_exit(127);
	}
	return pid;
}

static int wait_for_daemon(const struct cix_client *c)
{
	struct cix_response r;
	int i;

	for (i = 0; i < 100; i++) {
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

/* A request sent and not read: the socket, or -1. */
static int send_raw(const char *path)
{
	struct sockaddr_in a;
	char req[512];
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int n;

	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(TEST_PORT);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
	             path);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || write(fd, req, (size_t)n) != n) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Reads the whole response, up to timeout_ms. Returns the status (0 if
 * none arrived); body points into buf. */
static int read_raw(int fd, char *buf, size_t size, int timeout_ms, const char **body)
{
	size_t got = 0;
	long deadline = now_ms() + timeout_ms;
	int status = 0;
	char *sep;

	*body = "";
	while (got + 1 < size) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		long left = deadline - now_ms();
		ssize_t n;

		if (left <= 0 || poll(&p, 1, (int)left) <= 0)
			break;
		n = read(fd, buf + got, size - 1 - got);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	buf[got] = '\0';
	if (sscanf(buf, "HTTP/1.1 %d", &status) != 1)
		status = 0;
	sep = strstr(buf, "\r\n\r\n");
	if (sep != NULL)
		*body = sep + 4;
	return status;
}

/* 1 when the socket has something to read now. */
static int answered_yet(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };

	return poll(&p, 1, 0) > 0;
}

int main(void)
{
	static char buf[1 << 20];
	struct cix_client client;
	struct cix_response r;
	const char *body;
	pid_t daemon_pid;
	char detail[256];
	int fd, status;
	long t0, t1;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	if (seed_store() != 0) {
		fprintf(stderr, "FAIL: could not seed the log store\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	daemon_pid = start_daemon();
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (daemon_pid < 0 || wait_for_daemon(&client) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		if (daemon_pid > 0) {
			kill(daemon_pid, SIGKILL);
			waitpid(daemon_pid, NULL, 0);
		}
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. No filter: answered on the loop, from the newest lines. */
	memset(&r, 0, sizeof(r));
	t0 = now_ms();
	if (cix_client_request(&client, "GET", "/v1/system/logs?tail=300", NULL, &r) != 0 ||
	    r.status != 200 || r.json == NULL || r.json->type != JSON_ARRAY ||
	    r.json->u.array.count != 300)
		fail("an unfiltered tail=300 answers 300 entries", "");
	t1 = now_ms();
	cix_response_free(&r);
	printf("unfiltered tail=300 over an %d MB store: %ld ms\n", SEGMENTS * SEGMENT_BYTES >> 20,
	       t1 - t0);

	/* 2. A no-match regex reads every segment. Health, meanwhile. */
	fd = send_raw(NOMATCH);
	if (fd < 0) {
		fail("a filtered read can be sent", "");
	} else {
		long th0, th1, tr;

		usleep(100000);
		memset(&r, 0, sizeof(r));
		th0 = now_ms();
		if (cix_client_request(&client, "GET", "/v1/health", NULL, &r) != 0 || r.status != 200)
			fail("health answers while a filtered read is in flight", "");
		th1 = now_ms();
		cix_response_free(&r);
		if (answered_yet(fd)) {
			fail("the filtered read was still in flight when health was asked",
			     "it had already answered, so this run proves nothing -- the store is too "
			     "small for this machine");
		} else if (th1 - th0 > 50) {
			snprintf(detail, sizeof(detail), "health took %ld ms", th1 - th0);
			fail("health answers within 50 ms during a filtered read", detail);
		}
		tr = now_ms();
		status = read_raw(fd, buf, sizeof(buf), 60000, &body);
		close(fd);
		if (status != 200 || strncmp(body, "[]", 2) != 0) {
			snprintf(detail, sizeof(detail), "status %d, body %.40s", status, body);
			fail("the filtered read answers 200 with no entries", detail);
		}
		printf("health during the read: %ld ms; the read answered %ld ms after it\n", th1 - th0,
		       now_ms() - tr);
	}

	/* 3. A client that leaves: its read is killed, and says so. */
	fd = send_raw(NOMATCH);
	if (fd < 0) {
		fail("a filtered read can be sent", "");
	} else {
		int i, seen = 0;

		usleep(100000);
		close(fd);
		for (i = 0; i < 30 && !seen; i++) {
			usleep(100000);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/system/logs?tail=40", NULL, &r) == 0 &&
			    r.status == 200 && r.body != NULL &&
			    strstr(r.body, "helper for a log read exited -1") != NULL)
				seen = 1;
			cix_response_free(&r);
		}
		if (!seen)
			fail("a read whose client left is killed", "no helper end logged within 3 s");
	}

	/* 4. Two run, sixteen wait, and the nineteenth is told to retry. */
	{
		int fds[19], i, answered = 0, busy = 0;

		for (i = 0; i < 19; i++)
			fds[i] = send_raw(NOMATCH);
		usleep(300000);
		for (i = 0; i < 19; i++) {
			if (fds[i] < 0 || !answered_yet(fds[i]))
				continue;
			answered++;
			status = read_raw(fds[i], buf, sizeof(buf), 1000, &body);
			if (status == 503 && strstr(buf, "Retry-After: 1") != NULL)
				busy++;
		}
		if (answered != 1 || busy != 1) {
			snprintf(detail, sizeof(detail), "%d answered at once, %d of them 503", answered,
			         busy);
			fail("the read past 2 running and 16 waiting is answered 503 at once", detail);
		}
		for (i = 0; i < 19; i++)
			if (fds[i] >= 0)
				close(fds[i]);
	}

	/* After all that: health, at once. */
	usleep(500000);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/health", NULL, &r) != 0 || r.status != 200)
		fail("the daemon answers after the abandoned reads", "");
	cix_response_free(&r);

	if (test_assert_no_drift(&client, NULL) != 0)
		fail("nothing this test changed went unannounced (ADR-0339)", "");
	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);
	if (g_failures != 0) {
		fprintf(stderr, "test_logread_live: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_logread_live: a filtered read left the loop free, and its bounds held\n");
	return 0;
}
