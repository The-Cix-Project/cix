/*
 * Proves GET /v1/pkg/build/log end to end (task #676, ADR-0101): a
 * client attached WHILE a real build is still running receives its
 * output live, incrementally, not just after the fact from a
 * completed/failed job's log entry. Speaks the WS handshake/frame
 * format by hand over a raw socket, same "no existing client
 * primitive fits, write the minimum needed directly in the test"
 * precedent test/test_console_exec.c already set for the console
 * WebSocket. Reuses test_pkg.c's own hermetic file:// tarball +
 * toolchain-bootstrap pattern for a real, non-mocked build container.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7639
#define PORT_ARG "--port=7639"

static char g_data_dir[PATH_MAX];
static char g_pkg_state_dir[PATH_MAX];
static char g_images_base_dir[PATH_MAX];
static char g_pkgbuild_rootfs[PATH_MAX];

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

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_pkg_state_dir);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_images_base_dir);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_pkgbuild_rootfs);
	system(cmd);
}

static int run_cmd(const char *fmt, ...)
{
	char cmd[1024];
	va_list ap;
	int rc;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	rc = system(cmd);
	return (rc == 0) ? 0 : -1;
}

static int compute_file_sha256(const char *path, char *out_sha256, size_t sha256_size)
{
	char shacmd[700];
	FILE *sp;
	char buf[128] = { 0 };

	snprintf(shacmd, sizeof(shacmd), "sha256sum '%s'", path);
	sp = popen(shacmd, "r");
	if (sp == NULL)
		return -1;
	if (fgets(buf, sizeof(buf), sp) == NULL) {
		pclose(sp);
		return -1;
	}
	pclose(sp);
	if (strlen(buf) < 64 || 64 >= sha256_size)
		return -1;
	memcpy(out_sha256, buf, 64);
	out_sha256[64] = '\0';
	return 0;
}

/*
 * A minimal, wrapping-directory tarball with one placeholder file --
 * this recipe's own pkg_build()/pkg_install() below are pure shell
 * (deliberately no compile step, so the only real timing this test
 * needs -- several observable seconds of build duration -- comes from
 * explicit sleeps, not from how fast gcc happens to run), but
 * pkg_source/pkg_sha256 are still required recipe fields.
 */
static int stage_tarball(const char *scratch_dir, char *out_tarball_path, size_t tarball_path_size,
                          char *out_sha256, size_t sha256_size)
{
	char src_dir[512], placeholder[600];
	FILE *f;

	snprintf(src_dir, sizeof(src_dir), "%s/slowbuild-1.0", scratch_dir);
	if (run_cmd("mkdir -p '%s'", src_dir) != 0)
		return -1;

	snprintf(placeholder, sizeof(placeholder), "%s/placeholder.txt", src_dir);
	f = fopen(placeholder, "w");
	if (f == NULL)
		return -1;
	fputs("unused -- pkg_build() below never reads this\n", f);
	fclose(f);

	snprintf(out_tarball_path, tarball_path_size, "%s/slowbuild-1.0.tarball", scratch_dir);
	if (run_cmd("tar -cf '%s' -C '%s' slowbuild-1.0", out_tarball_path, scratch_dir) != 0)
		return -1;

	return compute_file_sha256(out_tarball_path, out_sha256, sha256_size);
}

/*
 * pkg_build() emits four distinct, individually-`sleep`-separated
 * markers -- a live-tail client attached partway through must observe
 * at least one marker arrive strictly before the build's own overall
 * completion, proving genuine incremental delivery rather than a
 * single post-completion dump.
 */
static int write_slowbuild_recipe(const char *tarball_path, const char *sha256)
{
	char path[300];
	FILE *f;

	if (run_cmd("mkdir -p '%s/recipes/slowbuild/1.0'", g_pkg_state_dir) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/recipes/slowbuild/1.0/build.sh", g_pkg_state_dir);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "pkg_name=slowbuild\n");
	fprintf(f, "pkg_version=1.0\n");
	fprintf(f, "pkg_source=file://%s\n", tarball_path);
	fprintf(f, "pkg_sha256=%s\n", sha256);
	fprintf(f, "pkg_depends=\"\"\n\n");
	fprintf(f, "pkg_build() {\n"
	           "\techo marker-1\n"
	           "\tsleep 1\n"
	           "\techo marker-2\n"
	           "\tsleep 1\n"
	           "\techo marker-3\n"
	           "\tsleep 1\n"
	           "\techo marker-4\n"
	           "}\n\n");
	fprintf(f, "pkg_install() {\n"
	           "\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n"
	           "\techo done > \"$PKG_DESTDIR/usr/bin/slowbuild-marker\"\n"
	           "}\n");
	fclose(f);
	return 0;
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;
	struct cix_response r;

	for (i = 0; i < max_attempts; i++) {
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

/* Polls GET /v1/pkg/{name} until its state matches want, or times out. */
static int wait_for_pkg_state(const struct cix_client *c, const char *name, const char *want,
                               int max_attempts)
{
	int i;
	char path[256];

	snprintf(path, sizeof(path), "/v1/pkg/%s", name);
	for (i = 0; i < max_attempts; i++) {
		struct cix_response r;
		const char *state;
		int matched;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", path, NULL, &r) != 0 || r.status != 200) {
			cix_response_free(&r);
			return -1;
		}
		state = json_str_field(r.json, "state");
		matched = state != NULL && strcmp(state, want) == 0;
		cix_response_free(&r);
		if (matched)
			return 0;
		usleep(100000);
	}
	return -1;
}

static int raw_connect(int port)
{
	int fd;
	struct sockaddr_in addr;
	struct timeval tv;

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
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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

/* Reads exactly one complete (unmasked, server-to-client) WS frame. */
static int recv_ws_frame(int fd, int *out_opcode, unsigned char *out_buf, size_t out_cap, size_t *out_len)
{
	unsigned char hdr[4];
	ssize_t n;
	int opcode;
	size_t len7, payload_len;

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

#define TEST_WS_KEY "dGhlIHNhbXBsZSBub25jZQ=="
#define TEST_WS_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	char scratch_dir[] = "/tmp/cix_test_pkgbuildlog_XXXXXX";
	char tarball_path[512], sha256[128];
	int fd;
	char req[512];
	int rlen;
	char resp[2048];
	ssize_t n;
	size_t got;
	char *headers_end;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_pkg_state_dir, sizeof(g_pkg_state_dir), "%s/rebuildable/pkg", g_data_dir);
	snprintf(g_images_base_dir, sizeof(g_images_base_dir), "%s/rebuildable/images/base", g_data_dir);
	snprintf(g_pkgbuild_rootfs, sizeof(g_pkgbuild_rootfs), "%s/rebuildable/images/pkgbuild", g_data_dir);

	reset_state();
	run_cmd("mkdir -p '%s/recipes'", g_pkg_state_dir);

	if (mkdtemp(scratch_dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (stage_tarball(scratch_dir, tarball_path, sizeof(tarball_path), sha256, sizeof(sha256)) != 0 ||
	    write_slowbuild_recipe(tarball_path, sha256) != 0) {
		fprintf(stderr, "FAIL: could not stage recipe fixture\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;

	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never became healthy\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/bootstrap", NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: POST /v1/pkg/bootstrap, status=%d\n", r.status);
		g_failures++;
	}
	cix_response_free(&r);

	/* --- scenario 1: no build in progress yet -> 404 --- */
	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect (no build in progress)");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/pkg/build/log HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "\r\n",
		                 TEST_WS_KEY);
		write_all_raw(fd, req, (size_t)rlen);
		n = read(fd, resp, sizeof(resp) - 1);
		CHECK(n > 0, "response for no-build-in-progress attach");
		if (n > 0) {
			resp[n] = '\0';
			CHECK(strstr(resp, "404") != NULL, "no build in progress -> 404");
		}
		close(fd);
	}

	/* --- scenario 2: no Upgrade header -> 400, doesn't fall through as
	 * an ordinary GET --- */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pkg/build/log", NULL, &r) != 0) {
		fprintf(stderr, "FAIL: plain GET .../build/log unreachable\n");
		g_failures++;
	} else {
		CHECK(r.status == 400, "plain GET .../build/log (no Upgrade header) should be 400");
	}
	cix_response_free(&r);

	/* --- scenario 3: the real thing -- kick off a real build, attach
	 * mid-build, observe live incremental output, then a clean close
	 * once the build finishes. --- */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"slowbuild\"}", &r) != 0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: POST install slowbuild, status=%d\n", r.status);
		g_failures++;
	}
	cix_response_free(&r);

	CHECK(wait_for_pkg_state(&client, "slowbuild", "building", 100) == 0,
	      "slowbuild reached state=building before this test's own timeout");

	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect for live build-log attach");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/pkg/build/log HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "\r\n",
		                 TEST_WS_KEY);
		CHECK(write_all_raw(fd, req, (size_t)rlen) == 0, "send build-log upgrade request");

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

		{
			char accumulated[4096];
			size_t acc_len = 0;
			int saw_marker_before_close = 0;
			int saw_close = 0;
			int attempts;

			accumulated[0] = '\0';
			for (attempts = 0; attempts < 40 && !saw_close; attempts++) {
				int opcode;
				unsigned char buf[2048];
				size_t len;

				if (recv_ws_frame(fd, &opcode, buf, sizeof(buf), &len) != 0)
					break;
				if (opcode == 0x8) {
					saw_close = 1;
					break;
				}
				if ((opcode == 0x1 || opcode == 0x2) && len > 0 &&
				    acc_len + len < sizeof(accumulated)) {
					memcpy(accumulated + acc_len, buf, len);
					acc_len += len;
					accumulated[acc_len] = '\0';
					if (strstr(accumulated, "marker-") != NULL)
						saw_marker_before_close = 1;
				}
			}
			CHECK(saw_marker_before_close,
			      "at least one build output marker arrived live, before the close frame");
			CHECK(saw_close, "daemon sent a real WS close frame once the build finished");
		}
		close(fd);
	}

	CHECK(wait_for_pkg_state(&client, "slowbuild", "installed", 100) == 0,
	      "slowbuild eventually reached state=installed");

	/* --- scenario 4: after the build has finished, a fresh attach is
	 * 404 again -- the attach list/output pipe were genuinely torn
	 * down, not left dangling. --- */
	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect (after build finished)");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/pkg/build/log HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "\r\n",
		                 TEST_WS_KEY);
		write_all_raw(fd, req, (size_t)rlen);
		n = read(fd, resp, sizeof(resp) - 1);
		CHECK(n > 0, "response for post-build attach");
		if (n > 0) {
			resp[n] = '\0';
			CHECK(strstr(resp, "404") != NULL, "attach after build finished -> 404 again");
		}
		close(fd);
	}

	stop_daemon(daemon_pid);
	reset_state();
	test_data_dir_cleanup(g_data_dir);

	if (g_failures == 0)
		printf("PKG BUILD LOG TEST: PASS\n");
	else
		printf("PKG BUILD LOG TEST: FAIL (%d failure(s))\n", g_failures);

	return g_failures == 0 ? 0 : 1;
}
