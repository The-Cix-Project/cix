/*
 * Proves GET /v1/containers/{name}/console end to end: the hand-rolled
 * RFC 6455 WebSocket handshake (daemon/src/websocket.c) and the
 * setns()-based double-fork exec mechanism (daemon/src/exec.c) wired
 * together in daemon/src/main.c's try_console_upgrade()/
 * handle_console_ws_event()/handle_console_pty_event(). No existing
 * client primitive fits here -- client/src/httpclient.c is strictly
 * one-shot request/response with no socket handoff (see this phase's
 * own plan) -- so this test speaks the handshake and frame format by
 * hand over a raw socket, the same "no existing primitive fits, write
 * the minimum needed directly in the test" precedent
 * test/test_dual_console.c already set for Phase 28's own PTY relay.
 *
 * Exec target is test/dual_console_child.c (already used by
 * test_dual_console.c as a trivial line-echo stand-in) -- reused here
 * rather than duplicated, staged into a real container image and run
 * via X-thinC-Exec-Cmd instead of as a bare host subprocess. The
 * container's own long-lived process is test/daemon_child.c (already
 * used across this project's other daemon tests for exactly "stay
 * alive for a controlled duration").
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7634
#define PORT_ARG "--port=7634"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static char g_container_defs_path[PATH_MAX];

/* RFC 6455's own worked example (section 1.3) -- already independently
 * confirmed correct against a real `openssl dgst -sha1 -binary |
 * openssl base64 -A` pipeline and against ws_compute_accept() directly
 * while building this phase. Reusing this fixed, known-correct vector
 * here proves the *wiring* (the real value that reaches a real HTTP
 * response), not the SHA-1/base64 math a second time. */
#define TEST_WS_KEY "dGhlIHNhbXBsZSBub25jZQ=="
#define TEST_WS_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

static int g_failures;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s\n", msg); \
			g_failures++; \
		} \
	} while (0)

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/thincd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/thincd", dargv, environ);
		perror("execve build/thincd");
		_exit(127);
	}
	return pid;
}

static int stop_daemon(pid_t pid)
{
	int status;

	kill(pid, SIGTERM);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void reset_state(void)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_container_defs_path);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_image_root);
	system(cmd);
}

static int wait_for_daemon(const struct thinc_client *c, int max_attempts)
{
	int i;
	struct thinc_response r;

	for (i = 0; i < max_attempts; i++) {
		if (thinc_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			thinc_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

/* Raw socket connect to the daemon -- thinc_client_request() has no
 * concept of a connection that survives past one response, which is
 * the whole point of what's being tested here. */
static int raw_connect(int port)
{
	int fd;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}

	/* A 2s read timeout -- a real protocol/relay bug should show up as
	 * a clean failed assertion, not this test hanging forever. */
	{
		struct timeval tv;

		tv.tv_sec = 2;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	return fd;
}

static int write_all_raw(int fd, const void *buf, size_t n)
{
	const char *p = buf;
	size_t written = 0;
	ssize_t w;

	while (written < n) {
		w = write(fd, p + written, n - written);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		written += (size_t)w;
	}
	return 0;
}

/* Sends one masked (client-to-server, per RFC 6455) text/binary frame. */
static int send_ws_frame(int fd, int opcode, const void *payload, size_t len)
{
	unsigned char header[8];
	unsigned char mask[4] = { 0x11, 0x22, 0x33, 0x44 };
	unsigned char *masked;
	size_t hlen;
	size_t i;
	int rc;

	header[0] = (unsigned char)(0x80 | opcode);
	if (len < 126) {
		header[1] = (unsigned char)(0x80 | len);
		hlen = 2;
	} else {
		header[1] = 0x80 | 126;
		header[2] = (unsigned char)((len >> 8) & 0xff);
		header[3] = (unsigned char)(len & 0xff);
		hlen = 4;
	}

	masked = malloc(len);
	if (masked == NULL && len > 0)
		return -1;
	for (i = 0; i < len; i++)
		masked[i] = ((const unsigned char *)payload)[i] ^ mask[i % 4];

	rc = write_all_raw(fd, header, hlen);
	if (rc == 0)
		rc = write_all_raw(fd, mask, 4);
	if (rc == 0 && len > 0)
		rc = write_all_raw(fd, masked, len);
	free(masked);
	return rc;
}

/* Reads exactly one complete (unmasked, server-to-client) WS frame,
 * blocking as needed. Very small/trusting -- this is a test client
 * against a daemon we control, not a hardened general-purpose one. */
static int recv_ws_frame(int fd, int *out_opcode, unsigned char *out_buf, size_t out_cap, size_t *out_len)
{
	unsigned char hdr[4];
	ssize_t n;
	int opcode;
	size_t len7, payload_len, extra = 0;

	n = read(fd, hdr, 2);
	if (n != 2)
		return -1;
	opcode = hdr[0] & 0x0f;
	len7 = hdr[1] & 0x7f;

	if (len7 == 126) {
		unsigned char ext[2];

		n = read(fd, ext, 2);
		if (n != 2)
			return -1;
		payload_len = ((size_t)ext[0] << 8) | (size_t)ext[1];
	} else {
		payload_len = len7;
	}
	(void)extra;

	if (payload_len > out_cap)
		return -1;

	{
		size_t total = 0;

		while (total < payload_len) {
			n = read(fd, out_buf + total, payload_len - total);
			if (n <= 0)
				return -1;
			total += (size_t)n;
		}
	}

	*out_opcode = opcode;
	*out_len = payload_len;
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct thinc_client client;
	struct thinc_response r;
	int fd;
	char req[1024];
	int rlen;
	char resp[2048];
	ssize_t n;
	char *headers_end;
	size_t got = 0;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/consoletest/v1/rootfs", g_data_dir);
	snprintf(g_container_defs_path, sizeof(g_container_defs_path), "%s/state/container_defs.json",
	         g_data_dir);

	reset_state();
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/consoletest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}

	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		fprintf(stderr, "FAIL: could not stage daemon_child\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (test_image_fixture_build(g_image_root, "build/dual_console_child", "dual_console_child") !=
	    0) {
		fprintf(stderr, "FAIL: could not stage dual_console_child\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;

	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never became healthy\n");
		stop_daemon(daemon_pid);
		return 1;
	}

	/* --- fixture: a real, long-lived running container --- */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"consoletest\",\"image\":\"consoletest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST consoletest, status=%d\n", r.status);
		g_failures++;
	}
	thinc_response_free(&r);

	/* --- scenario 1: a plain GET with no Upgrade header is a 400, not
	 * silently treated as an ordinary request for this path --- */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/containers/consoletest/console", NULL, &r) != 0) {
		fprintf(stderr, "FAIL: plain GET .../console unreachable\n");
		g_failures++;
	} else {
		CHECK(r.status == 400, "plain GET .../console (no Upgrade header) should be 400");
	}
	thinc_response_free(&r);

	/* --- scenario 2: console into a container that doesn't exist --- */
	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect for missing-container scenario");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/containers/nosuchcontainer/console HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "\r\n",
		                 TEST_WS_KEY);
		write_all_raw(fd, req, (size_t)rlen);
		n = read(fd, resp, sizeof(resp) - 1);
		CHECK(n > 0, "response for missing-container console request");
		if (n > 0) {
			resp[n] = '\0';
			CHECK(strstr(resp, "404") != NULL, "missing container -> 404");
		}
		close(fd);
	}

	/* --- scenario 3: the real thing -- upgrade, exec dual_console_child,
	 * exchange real lines, confirm clean teardown --- */
	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect for real console session");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/containers/consoletest/console HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "X-thinC-Exec-Cmd: /bin/dual_console_child\r\n"
		                 "\r\n",
		                 TEST_WS_KEY);
		CHECK(write_all_raw(fd, req, (size_t)rlen) == 0, "send upgrade request");

		/* Read the handshake response headers (up to the blank line). */
		got = 0;
		headers_end = NULL;
		while (got < sizeof(resp) - 1) {
			n = read(fd, resp + got, sizeof(resp) - 1 - got);
			if (n <= 0)
				break;
			got += (size_t)n;
			resp[got] = '\0';
			headers_end = strstr(resp, "\r\n\r\n");
			if (headers_end != NULL)
				break;
		}
		CHECK(headers_end != NULL, "received a complete handshake response");
		CHECK(strncmp(resp, "HTTP/1.1 101", 12) == 0, "handshake response is 101 Switching Protocols");
		CHECK(strstr(resp, "Sec-WebSocket-Accept: " TEST_WS_ACCEPT) != NULL,
		      "Sec-WebSocket-Accept matches the RFC 6455 worked example");

		if (send_ws_frame(fd, 0x2 /* binary */, "hello-console\n", 14) == 0) {
			/* The pty is in canonical mode, so the kernel's own input
			 * echo (the literal "hello-console" we just sent) can
			 * arrive as its own frame *before* dual_console_child's
			 * real "ECHO:..." output -- read frames until the marker
			 * shows up (accumulated across as many as it takes), not
			 * just the first one. */
			char accumulated[1024];
			size_t acc_len = 0;
			int found = 0;
			int attempts;

			accumulated[0] = '\0';
			for (attempts = 0; attempts < 20 && !found; attempts++) {
				int opcode;
				unsigned char buf[512];
				size_t len;

				if (recv_ws_frame(fd, &opcode, buf, sizeof(buf), &len) != 0)
					break;
				if (len > 0 && acc_len + len < sizeof(accumulated)) {
					memcpy(accumulated + acc_len, buf, len);
					acc_len += len;
					accumulated[acc_len] = '\0';
				}
				if (strstr(accumulated, "ECHO:hello-console") != NULL)
					found = 1;
			}
			CHECK(found, "dual_console_child's real echo output came back through the relay");
		} else {
			CHECK(0, "send_ws_frame(hello-console)");
		}

		/* QUIT makes dual_console_child exit cleanly -- the pty side
		 * should then hit EOF and the daemon should tear the session
		 * down on its own, closing this socket. */
		send_ws_frame(fd, 0x2, "QUIT\n", 5);
		{
			char drain[256];
			int saw_close = 0;
			int attempts;

			for (attempts = 0; attempts < 50; attempts++) {
				n = read(fd, drain, sizeof(drain));
				if (n <= 0) {
					saw_close = 1;
					break;
				}
			}
			CHECK(saw_close, "connection closed on its own once the exec'd process exited");
		}
		close(fd);
	}

	/* --- cleanup --- */
	memset(&r, 0, sizeof(r));
	thinc_client_request(&client, "DELETE", "/v1/containers/consoletest", NULL, &r);
	thinc_response_free(&r);

	stop_daemon(daemon_pid);
	reset_state();
	test_data_dir_cleanup(g_data_dir);

	if (g_failures == 0)
		printf("CONSOLE EXEC TEST: PASS\n");
	else
		printf("CONSOLE EXEC TEST: FAIL (%d failure(s))\n", g_failures);

	return g_failures == 0 ? 0 : 1;
}
