/*
 * Proves GET /v1/pkg/build/log end to end (task #676, ADR-0101): a
 * client attached WHILE a real build is still running receives its
 * output live, incrementally, not just after the fact from a
 * completed/failed job's log entry. Speaks the WS handshake/frame
 * format by hand over a raw socket, same "no existing client
 * primitive fits, write the minimum needed directly in the test"
 * precedent test/test_console_exec.c already set for the console
 * WebSocket. Reuses test_pkg.c's own hermetic loopback-http tarball +
 * toolchain-bootstrap pattern for a real, non-mocked build container.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"
#include "test_floor.h"

#include <stdint.h>
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
#include <time.h>
#include <unistd.h>

extern char **environ;

/* Monotonic milliseconds, for saying how long a read waited rather
 * than only that it gave up (#519). */
static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

	/*
	 * A non-ASCII file name, in a pax archive, which is how upstream
	 * source tarballs carry one: pax records the path as UTF-8. cixd
	 * extracts sources in-process with libarchive (#411), and in the C
	 * locale libarchive cannot represent that name, so the whole
	 * extraction failed -- go1.24.9's source was refused with
	 * "Pathname can't be converted from UTF-8 to current locale"
	 * (192.168.15.95, 2026-09-23). pkg_build() below checks the file
	 * arrived with its bytes intact.
	 */
	snprintf(placeholder, sizeof(placeholder), "%s/na\xc3\xafve-\xc3\xbc.txt", src_dir);
	f = fopen(placeholder, "w");
	if (f == NULL)
		return -1;
	fputs("a file whose name is not ASCII\n", f);
	fclose(f);

	snprintf(out_tarball_path, tarball_path_size, "%s/slowbuild-1.0.tarball", scratch_dir);
	if (run_cmd("LC_ALL=C.UTF-8 tar --format=pax -cf '%s' -C '%s' slowbuild-1.0", out_tarball_path,
	            scratch_dir) != 0)
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
	fprintf(f, "pkg_source=%s\n", test_http_src(tarball_path));
	fprintf(f, "pkg_sha256=%s\n", sha256);
	fprintf(f, "pkg_depends=\"\"\n");
	fprintf(f, "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n");
	fprintf(f, "pkg_build() {\n"
	           "\ttest -f \"$(printf 'na\\303\\257ve-\\303\\274.txt')\"\n"
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

/*
 * #519: what the job was doing, printed at the two moments that
 * distinguish this bug's candidate causes.
 *
 * The live tail intermittently gets its 101 and then neither a frame
 * nor a close frame, and reports "0 frames, 0 bytes" -- which is
 * consistent with three different things and tells them apart from
 * none of them: the build finished before the attach and nothing was
 * left to send; the build was still running and had produced no output
 * within the socket's 5 s SO_RCVTIMEO; or the conn really was on the
 * attach list and teardown missed it.
 *
 * v2.57.277 ruled out the first -- the attach now answers 404 when the
 * chain's output fd is already closed -- and the failure survived, so
 * a guess was wrong rather than a fix being wrong. The remaining two
 * are told apart by the job's own state at the moment the read gave
 * up: still "building" means the timeout is the bug, anything else
 * means the close frame is.
 */
static void report_job_state(const struct cix_client *c, const char *name, const char *when)
{
	struct cix_response r;
	const char *state = NULL, *err = NULL;
	char path[256];

	memset(&r, 0, sizeof(r));
	snprintf(path, sizeof(path), "/v1/pkg/%s", name);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0 || r.json == NULL) {
		fprintf(stderr, "      job state %s: GET /v1/pkg/%s unavailable (status=%d)\n", when,
		        name, r.status);
		cix_response_free(&r);
		return;
	}
	state = json_as_string(json_object_get(r.json, "state"));
	err = json_as_string(json_object_get(r.json, "error"));
	fprintf(stderr, "      job state %s: %s%s%s\n", when, state != NULL ? state : "(none)",
	        err != NULL && err[0] != '\0' ? ", error: " : "", err != NULL ? err : "");
	cix_response_free(&r);
}
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

/*
 * A websocket stream, with whatever the handshake read already took
 * off the socket sitting in front of it.
 *
 * One read() of the upgrade response is entitled to return the 101
 * headers AND whatever the daemon wrote next in the same segment --
 * and this endpoint writes the replay snapshot immediately after the
 * 101 (try_pkg_build_log_upgrade(), daemon/src/main.c). Discarding
 * those trailing bytes does not cost one frame, it MISALIGNS the
 * stream: the next two bytes read are payload, parsed as a frame
 * header, and the bogus length that follows swallows every later
 * frame -- the close frame included -- until the read times out.
 *
 * It presents as "0 frames, 0 bytes" from a daemon that did
 * everything right, and only when the snapshot is non-empty and TCP
 * happens to coalesce the two writes, which is why it is
 * intermittent. Measured on 192.168.15.95, 2026-09-25
 * (probe-cix-testreport@35-1): five runs, two failures, both with 0
 * frames and no close frame, one with the job still "building" at
 * the 5 s give-up and one with it already "installed" -- a spread
 * that fits a misaligned reader and nothing else (#519).
 */
struct ws_reader {
	int fd;
	/* Sized to the handshake buffer it drains, so the whole of a
	 * coalesced read always fits. */
	unsigned char pending[2048];
	size_t pending_len;
	size_t pending_pos;
	/* What the last failed read saw, so a caller can say which of
	 * "the peer closed" (0) and "nothing arrived in time" (-1 with
	 * EAGAIN) it hit -- they are different bugs and read_full used to
	 * report both as -1. */
	ssize_t last_n;
	int last_errno;
};

static void ws_reader_init(struct ws_reader *r, int fd, const void *leftover, size_t leftover_len)
{
	memset(r, 0, sizeof(*r));
	r->fd = fd;
	r->last_n = 1;
	if (leftover_len > sizeof(r->pending)) {
		fprintf(stderr, "    ws: %zu leftover handshake bytes exceed this reader's %zu\n",
		        leftover_len, sizeof(r->pending));
		leftover_len = sizeof(r->pending);
	}
	memcpy(r->pending, leftover, leftover_len);
	r->pending_len = leftover_len;
}

/*
 * A websocket frame header is two bytes, and read() is entitled to
 * hand back one of them.
 *
 * That is not pedantry here: it is the whole of a flake that has
 * failed the build gate repeatedly on three different assertions of
 * this file -- "received 0 bytes", the SIGWINCH resize, and the
 * initial report -- because losing any single frame presents as
 * whatever that frame was carrying never arriving. A short read is
 * likeliest exactly when the box is busy, which is why it shows up in
 * a build container and never in a quiet hand-run.
 *
 * The payload loop below has always looped. The two header reads did
 * not, and treated a one-byte read as a dead connection, discarding a
 * frame that was perfectly good and only late.
 */
static int ws_read_full(struct ws_reader *r, void *buf, size_t want)
{
	unsigned char *p = buf;
	size_t got = 0;

	while (got < want) {
		ssize_t n;

		if (r->pending_pos < r->pending_len) {
			size_t avail = r->pending_len - r->pending_pos;
			size_t take = (avail > want - got) ? want - got : avail;

			memcpy(p + got, r->pending + r->pending_pos, take);
			r->pending_pos += take;
			got += take;
			continue;
		}
		n = read(r->fd, p + got, want - got);
		if (n <= 0) {
			r->last_n = n;
			r->last_errno = errno;
			return -1;
		}
		got += (size_t)n;
	}
	return 0;
}

static int recv_ws_frame(struct ws_reader *r, int *out_opcode, unsigned char *out_buf, size_t out_cap,
                          size_t *out_len)
{
	unsigned char hdr[4];
	int opcode;
	size_t len7, payload_len;

	if (ws_read_full(r, hdr, 2) != 0)
		return -1;
	opcode = hdr[0] & 0x0f;
	len7 = hdr[1] & 0x7f;

	if (len7 == 126) {
		unsigned char ext[2];

		if (ws_read_full(r, ext, 2) != 0)
			return -1;
		payload_len = ((size_t)ext[0] << 8) | (size_t)ext[1];
	} else if (len7 == 127) {
		/* The 64-bit form. ws_write_frame() never sends it (its
		 * payload is capped at 64 KiB), so meeting one is a protocol
		 * fault to report, not a length to read as 127 bytes. */
		fprintf(stderr, "    ws: unexpected 64-bit length frame\n");
		return -1;
	} else {
		payload_len = len7;
	}
	if (payload_len > out_cap) {
		fprintf(stderr, "    ws: %zu-byte frame exceeds this reader's %zu-byte buffer\n",
		        payload_len, out_cap);
		return -1;
	}
	/* The same loop the header now uses -- one implementation, not a
	 * second copy of it three lines further down. */
	if (payload_len > 0 && ws_read_full(r, out_buf, payload_len) != 0)
		return -1;
	*out_opcode = opcode;
	*out_len = payload_len;
	return 0;
}

#define TEST_WS_KEY "dGhlIHNhbXBsZSBub25jZQ=="
#define TEST_WS_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

/*
 * One handshake attempt against an arbitrary query string, returning
 * the HTTP status the daemon answered with and (for a non-101) the
 * response text, so a test can assert on the message and not only on
 * the code (#476: the two 404s this endpoint can return mean
 * different things and used to carry the same words).
 *
 * Closes the connection before returning -- nothing here relays
 * frames, and leaving a 101 attached would count against the
 * endpoint's own four-viewer cap for the rest of the run.
 *
 * Returns the status code, or -1 if nothing parseable came back.
 */
static int attach_once(const char *query, char *out_resp, size_t out_cap)
{
	int fd, status = -1;
	char req[512];
	char resp[2048];
	size_t got = 0;
	int rlen;

	if (out_cap > 0)
		out_resp[0] = '\0';
	fd = raw_connect(TEST_PORT);
	if (fd < 0)
		return -1;
	rlen = snprintf(req, sizeof(req),
	                 "GET /v1/pkg/build/log%s HTTP/1.1\r\n"
	                 "Host: 127.0.0.1\r\n"
	                 "Upgrade: websocket\r\n"
	                 "Connection: Upgrade\r\n"
	                 "Sec-WebSocket-Key: %s\r\n"
	                 "Sec-WebSocket-Version: 13\r\n"
	                 "\r\n",
	                 query, TEST_WS_KEY);
	if (rlen > 0 && (size_t)rlen < sizeof(req) &&
	    write_all_raw(fd, req, (size_t)rlen) == 0) {
		while (got < sizeof(resp) - 1) {
			ssize_t n = read(fd, resp + got, sizeof(resp) - 1 - got);

			if (n <= 0)
				break;
			got += (size_t)n;
			resp[got] = '\0';
			if (strstr(resp, "\r\n\r\n") != NULL)
				break;
		}
		if (got > 12 && strncmp(resp, "HTTP/1.1 ", 9) == 0)
			status = atoi(resp + 9);
		if (out_cap > 0)
			snprintf(out_resp, out_cap, "%s", resp);
	}
	close(fd);
	return status;
}

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
	/*
	 * ADR-0209: the build floor -- real recipe-built artifacts seeded
	 * into this daemon's own cache so installing them is a cache hit
	 * needing no build environment. Seeded AFTER reset_state(), which
	 * wipes the pkg directory: doing it before meant the recipes were
	 * deleted moments later and every floor install failed with "no
	 * such recipe", which then surfaced three screens away as a build
	 * complaining that tcc was not installed.
	 */
	if (test_image_fixture_seed_floor_packages(g_data_dir, "build-inputs/floor-artifacts") != 0) {
		fprintf(stderr, "could not seed the build floor -- fetch the real package artifacts "
		                "into build-inputs/floor-artifacts first (ADR-0209)\n");
		return 1;
	}

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
	CHECK(test_floor_install_all(&client) == 0, "install the build floor");

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

	if (wait_for_pkg_state(&client, "slowbuild", "building", 100) != 0) {
		/* Say WHY, not just that it did not happen: a package that
		 * fails fast never passes through "building" at all, and the
		 * reason is sitting in its own error field. Without this the
		 * failure is indistinguishable from a timeout. */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "GET", "/v1/pkg/slowbuild", NULL, &r);
		{
			char now_sha[128] = { 0 };

			(void)compute_file_sha256(tarball_path, now_sha, sizeof(now_sha));
			fprintf(stderr,
			        "FAIL: slowbuild reached state=building before this test's own timeout\n"
			        "      slowbuild is: %.400s\n"
			        "      recipe sha : %s\n"
			        "      tarball now: %s (%s)\n",
			        r.body != NULL ? r.body : "(no response)", sha256, now_sha,
			        strcmp(sha256, now_sha) == 0 ? "unchanged" : "CHANGED since the recipe was written");
		}
		cix_response_free(&r);
		g_failures++;
	}

	/*
	 * --- scenario 3a: a ?name= with no ?image= resolves, and a ?name=
	 * that matches nothing says what IS building (#476). ---
	 *
	 * The bug #476 records is that an absent ?image= was normalised to
	 * PKG_DEFAULT_IMAGE, so a job filed under any other image -- a host
	 * build's PKG_HOSTBUILD_IMAGE above all -- could not be reached by
	 * name. THIS TEST CANNOT REPRODUCE THAT HALF: slowbuild installs
	 * into "base", so `?name=slowbuild` matched before the fix too.
	 * Standing that up here would mean bootstrapping a second image
	 * with its own toolchain, which is disproportionate, and the real
	 * reproduction is a host build -- verified on 192.168.15.95 rather
	 * than here, and recorded in #476. What these two DO gate is the
	 * name-only lookup path staying alive, and the message: the old
	 * text asserted "no build in progress" from one failed lookup,
	 * which is why an operator with a running build was told there was
	 * none.
	 */
	{
		char aresp[2048];
		int st = attach_once("?name=slowbuild", aresp, sizeof(aresp));

		CHECK(st == 101, "?name= with no ?image= attaches to the running build");
		if (st != 101)
			fprintf(stderr, "      got: %.300s\n", aresp);

		st = attach_once("?name=nosuchpackage", aresp, sizeof(aresp));
		CHECK(st == 404, "?name= matching nothing -> 404");
		CHECK(strstr(aresp, "slowbuild") != NULL,
		      "that 404 names the build that IS running (#476)");
		if (strstr(aresp, "slowbuild") == NULL)
			fprintf(stderr, "      got: %.300s\n", aresp);
	}

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
			/*
			 * Sized to the largest frame ws_write_frame() can send
			 * (WS_MAX_FRAME_PAYLOAD, 64 KiB), not to a guess at how
			 * much output one relay carries. This was 2048 while the
			 * relay reads up to 4096 bytes at a time
			 * (handle_pkg_build_output_event()), so one busy chunk
			 * ended the loop with neither a marker nor the close
			 * frame seen (probe-cix-testreport@4-1, 2026-09-23).
			 *
			 * "marker-" is looked for in each frame plus the tail of
			 * the one before, so a marker split across two relays
			 * still counts and nothing needs to be accumulated.
			 */
			static unsigned char buf[65536 + 8];
			char tail[8] = "";
			size_t frames = 0, bytes = 0;
			const char *ended = "frame limit reached";
			int saw_marker_before_close = 0;
			int saw_close = 0;
			struct ws_reader reader;
			size_t consumed = headers_end != NULL ? (size_t)(headers_end + 4 - resp) : got;
			long long t0;

			/*
			 * Everything this read took past the handshake is the
			 * front of the frame stream, not spare bytes -- see
			 * struct ws_reader. Nothing to hand over is the ordinary
			 * case; having some is the case that used to misalign
			 * the reader for the rest of the build (#519).
			 */
			ws_reader_init(&reader, fd, resp + consumed, got > consumed ? got - consumed : 0);
			if (got > consumed)
				fprintf(stderr, "      handshake read carried %zu byte(s) of frame data\n",
				        got - consumed);

			/* #519: the paired reading -- what the job was doing when
			 * the tail attached, against what it was doing when the
			 * read gave up. A run that shows "building" at both ends
			 * produced no output for five seconds while alive. */
			report_job_state(&client, "slowbuild", "at attach");
			t0 = now_ms();

			while (frames < 4096) {
				int opcode;
				size_t len, tl = strlen(tail);

				if (recv_ws_frame(&reader, &opcode, buf + tl, sizeof(buf) - 8, &len) != 0) {
					/* Which failure: a peer that closed without a
					 * close frame, or five seconds of silence. They
					 * point at different code and used to share one
					 * message. */
					if (reader.last_n == 0)
						ended = "peer closed the socket";
					else if (reader.last_errno == EAGAIN || reader.last_errno == EWOULDBLOCK)
						ended = "no data within the 5 s receive timeout";
					else
						ended = "read error";
					break;
				}
				frames++;
				if (opcode == 0x8) {
					saw_close = 1;
					ended = "close frame";
					break;
				}
				if ((opcode != 0x1 && opcode != 0x2) || len == 0)
					continue;
				bytes += len;
				memcpy(buf, tail, tl);
				buf[tl + len] = '\0';
				if (memmem(buf, tl + len, "marker-", 7) != NULL)
					saw_marker_before_close = 1;
				/* Keep the last six bytes: one short of "marker-". */
				if (tl + len > 6) {
					memcpy(tail, buf + tl + len - 6, 6);
					tail[6] = '\0';
				} else {
					memcpy(tail, buf, tl + len);
					tail[tl + len] = '\0';
				}
			}
			/*
			 * Printed whether or not the assertions passed: a pass
			 * that is one burst frame at exit and a pass that is four
			 * live frames are the difference between this endpoint
			 * streaming and this test being weaker than its name, and
			 * only this line tells them apart.
			 */
			fprintf(stderr, "      live tail ended on: %s, after %zu frames, %zu bytes, %lld ms\n",
			        ended, frames, bytes, now_ms() - t0);
			CHECK(saw_marker_before_close,
			      "at least one build output marker arrived live, before the close frame");
			CHECK(saw_close, "daemon sent a real WS close frame once the build finished");
			if (!saw_marker_before_close || !saw_close)
				report_job_state(&client, "slowbuild", "when the read gave up");
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

	/*
	 * And by NAME after the build has finished, asserting the words and
	 * not only the code (#476). The endpoint has two 404s now and they
	 * mean different things: "no build in progress", and "that build
	 * has no live output right now ... retry shortly". A finished build
	 * must get the first. The second would be a claim of transience
	 * about a job that is over, which is what happens if a chain slot
	 * its job failed to release is still matched by name -- so this is
	 * the gate on chain_reap_stale() being reached from the name
	 * lookup, not decoration on the status code.
	 */
	{
		char aresp[2048];
		int st = attach_once("?name=slowbuild", aresp, sizeof(aresp));

		CHECK(st == 404, "?name= after the build finished -> 404");
		CHECK(strstr(aresp, "no build in progress") != NULL,
		      "that 404 says no build in progress, not \"retry shortly\" (#476)");
		if (strstr(aresp, "no build in progress") == NULL)
			fprintf(stderr, "      got: %.300s\n", aresp);
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
