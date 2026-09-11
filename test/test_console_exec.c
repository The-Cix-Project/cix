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
 * via the cmd query parameter instead of as a bare host subprocess. The
 * container's own long-lived process is test/daemon_child.c (already
 * used across this project's other daemon tests for exactly "stay
 * alive for a controlled duration").
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <stdint.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
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

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_container_defs_path);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_image_root);
	system(cmd);
}

/*
 * What the daemon thought it was doing while this test waited (#291).
 *
 * Every failure of this test is "output did not arrive in time", and
 * the test cannot distinguish the three causes that produce it: the
 * child wrote nothing, the frames carried something else, or the
 * daemon's single-threaded event loop was blocked and simply never
 * got round to writing.
 *
 * The daemon already measures the third directly. GET /v1/system/stalls
 * carries a "loop" object with the worst single pass since boot, the
 * count over the slow threshold, and which request that worst pass was
 * serving. Printing it on failure turns "no output" into "no output,
 * and the loop's worst pass was N ms doing X", which separates a
 * scheduling stall from a console bug in one reading.
 *
 * Worth trusting only since #229: that endpoint used to stop reading
 * its own record file at the first NUL byte, so it under-reported for
 * four days on the reference host while looking perfectly healthy.
 */
static void report_loop_health(struct cix_client *c)
{
	struct cix_response lr;

	memset(&lr, 0, sizeof(lr));
	if (cix_client_request(c, "GET", "/v1/system/stalls?limit=3", NULL, &lr) != 0 ||
	    lr.status != 200 || lr.json == NULL) {
		fprintf(stderr, "  loop: GET /v1/system/stalls unavailable (status=%d)\n", lr.status);
		cix_response_free(&lr);
		return;
	}
	{
		const struct json_value *loop = json_object_get(lr.json, "loop");
		const struct json_value *worst = loop != NULL ? json_object_get(loop, "worst_pass_ms") : NULL;
		const struct json_value *slow = loop != NULL ? json_object_get(loop, "slow_passes") : NULL;
		const struct json_value *act =
		    loop != NULL ? json_object_get(loop, "worst_pass_activity") : NULL;

		fprintf(stderr, "  loop: worst_pass_ms=%ld slow_passes=%ld worst_pass_activity=%s\n",
		        worst != NULL && worst->type == JSON_NUMBER ? (long)worst->u.number : -1L,
		        slow != NULL && slow->type == JSON_NUMBER ? (long)slow->u.number : -1L,
		        act != NULL && act->type == JSON_STRING && act->u.string[0] != '\0'
		            ? act->u.string
		            : "(none)");
	}
	cix_response_free(&lr);
}

/*
 * #331: WHICH process is not producing output, and what is it doing?
 *
 * report_loop_health() above answered "is the daemon stuck" and the
 * answer has been a consistent no -- worst_pass_ms 19-24 while a
 * session sat silent for thirty seconds. #372's step diagnostics then
 * ruled out the four ways the intermediate child can fail before the
 * grandchild exists: nothing is ever logged, so the exec succeeds and
 * the daemon believes the session is healthy.
 *
 * That leaves the exec'd process itself, and nothing has ever looked at
 * it. GET /v1/system/processes carries state and wchan for every
 * process the daemon can see, tagged with the container it belongs to
 * -- the same reading stallwatch takes of the daemon, pointed at the
 * thing that is actually silent.
 *
 * A process in R with an empty wchan is running and choosing not to
 * write; one in D names the kernel function it is stuck in; an empty
 * list means it is not there at all and the 101 was a lie. Those are
 * three different bugs and this test could not tell them apart.
 */
/* This file has no json_str_field() -- that helper is local to other
 * test files -- so field reads go through json_object_get() directly,
 * the same way report_loop_health() above does. */
static const char *proc_str(const struct json_value *obj, const char *key)
{
	const struct json_value *v = json_object_get(obj, key);

	return (v != NULL && v->type == JSON_STRING) ? v->u.string : NULL;
}

/*
 * #331: what the DAEMON saw, alongside what the test saw.
 *
 * The daemon's reap of the exec'd process now records how it ended --
 * a signal names its killer, an ordinary exit means it chose to, 127
 * means the shell could not find what it was asked to run. That line
 * goes to the log store, which this test otherwise never reads, so the
 * one fact that distinguishes those was visible only to someone who
 * went looking for it afterwards on a live host.
 */
static void report_console_log(struct cix_client *c)
{
	struct cix_response lr;
	size_t i;

	memset(&lr, 0, sizeof(lr));
	if (cix_client_request(c, "GET", "/v1/system/logs?regex=console%3A&tail=6", NULL, &lr) != 0 ||
	    lr.status != 200 || lr.json == NULL || lr.json->type != JSON_ARRAY) {
		fprintf(stderr, "  dlog: GET /v1/system/logs unavailable (status=%d)\n", lr.status);
		cix_response_free(&lr);
		return;
	}
	for (i = 0; i < lr.json->u.array.count; i++) {
		const char *msg = proc_str(lr.json->u.array.items[i], "msg");

		if (msg != NULL)
			fprintf(stderr, "  dlog: %.180s\n", msg);
	}
	if (lr.json->u.array.count == 0)
		fprintf(stderr, "  dlog: the daemon logged nothing about any console session\n");
	cix_response_free(&lr);
}

static void report_container_processes(struct cix_client *c, const char *container)
{
	struct cix_response pr;
	size_t i;
	int shown = 0;

	memset(&pr, 0, sizeof(pr));
	if (cix_client_request(c, "GET", "/v1/system/processes", NULL, &pr) != 0 ||
	    pr.status != 200 || pr.json == NULL || pr.json->type != JSON_ARRAY) {
		fprintf(stderr, "  procs: GET /v1/system/processes unavailable (status=%d)\n",
		        pr.status);
		cix_response_free(&pr);
		return;
	}
	for (i = 0; i < pr.json->u.array.count; i++) {
		const struct json_value *p = pr.json->u.array.items[i];
		const char *cn = proc_str(p, "container");
		const char *comm = proc_str(p, "comm");
		const char *state = proc_str(p, "state");
		const char *wchan = proc_str(p, "wchan");
		const char *cmdl = proc_str(p, "command_line");
		const struct json_value *pid = json_object_get(p, "pid");

		/*
		 * #331: the container filter used to be applied HERE, and it
		 * made this report unable to answer the one question it was
		 * added for.
		 *
		 * `container` is populated by walking a process's real host
		 * ppid chain against each running container's root pid. An
		 * exec'd console session is a child of CIXD, not of the
		 * container's init -- it joined the namespaces with setns()
		 * rather than being cloned into them -- so its `container` is
		 * empty and the filter dropped it. Whether it is alive or
		 * dead. A report that shows "no process in this container"
		 * either way cannot distinguish them, and a comment on #331
		 * asserted the process was gone on exactly that evidence.
		 *
		 * So print everything and label it instead. In a build
		 * container the list is short, and the unattributed entries
		 * are precisely where the session would be.
		 */
		fprintf(stderr, "  procs: [%s] pid=%ld comm=%s state=%s wchan=%s cmd=%.60s\n",
		        (cn != NULL && cn[0] != '\0') ? cn : "host/unattributed",
		        pid != NULL && pid->type == JSON_NUMBER ? (long)pid->u.number : -1L,
		        comm != NULL ? comm : "?", state != NULL ? state : "?",
		        (wchan != NULL && wchan[0] != '\0') ? wchan : "(none)",
		        cmdl != NULL ? cmdl : "?");
		if (cn != NULL && strcmp(cn, container) == 0)
			shown++;
	}
	if (shown == 0)
		fprintf(stderr,
		        "  procs: nothing is attributed to container %s -- note an exec'd session "
		        "never is, so read the host/unattributed lines above\n",
		        container);
	cix_response_free(&pr);
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

/* Raw socket connect to the daemon -- cix_client_request() has no
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

	/*
	 * A read timeout, so a real protocol/relay bug shows up as a clean
	 * failed assertion rather than this test hanging forever.
	 *
	 * Ten seconds, not the two it was. What one of these reads waits
	 * for is the whole chain: the HTTP upgrade, a fork, entering the
	 * container's namespaces, an execve, the child's first write, and
	 * the relay back. Two seconds is generous when the box is idle and
	 * is not a limit anything is measured against -- but this suite
	 * runs inside a build container on a two-CPU host, alongside the
	 * rest of itself, and there it is simply a race. Measured: the
	 * v2.53.28 build failed on the defaults scenario with zero bytes
	 * received, while the immediately preceding scenario -- the same
	 * request, on its own connection -- passed.
	 *
	 * A longer ceiling costs nothing when the test passes and changes
	 * no assertion. It only means a genuine failure is reported later.
	 *
	 * Raised 10 -> 60 after two more release-blocking failures (#331):
	 * v2.57.64 and v2.57.66 both died on this scenario at ~10226 ms,
	 * which is the ceiling itself. That is the tell. The test never
	 * learned how much longer the child actually needed, so "slow" and
	 * "never produced a byte at all" were indistinguishable -- the same
	 * failure mode the READ_TIMEOUT/READ_PEER_CLOSED split was added to
	 * fix, one level up. Both runs also printed clean loop figures
	 * (worst_pass_ms=22, slow_passes=0), so the daemon was turning
	 * normally throughout and this is CPU starvation of the container's
	 * child while the same box compiles the control plane.
	 *
	 * Then 60 broke it differently, and that is what found the real
	 * constraint: the fixture container was created with
	 * `daemon_child 30`, so it exits cleanly after THIRTY seconds. A
	 * 60-second read ceiling outlives the container it is reading from.
	 * v2.57.68 waited 29906 ms and ended PEER CLOSED, then reported ten
	 * more failures from scenarios whose container had simply gone.
	 * The fixture's own lifetime was the ceiling on this whole test all
	 * along, and nothing said so -- which is also why moving 2 -> 10
	 * never settled it.
	 *
	 * So: the container now lives 300 s (see its create body), and this
	 * sits at 30 -- three times the figure the real failures clustered
	 * at, and a tenth of the fixture's life, so a slow child passes, a
	 * broken one still fails, and neither can be confused with the
	 * fixture expiring underneath the test.
	 */
	{
		struct timeval tv;

		tv.tv_sec = 30;
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
/*
 * Why a read stopped, kept apart rather than collapsed into -1.
 *
 * #331/#291. This test has failed intermittently four times and every
 * report said "read failed or peer closed", because read_full() folded
 * a TIMEOUT and a CLOSED PEER into the same -1. Those are opposite
 * faults with opposite fixes: a timeout means the container's child had
 * not produced its first byte inside the budget on a loaded two-CPU
 * box, and a close means the session really did collapse, which is a
 * daemon bug. Four investigations had no way to tell which, and the
 * previous attempt at this raised the budget from 2 s to 10 s -- a
 * change that cannot distinguish them either.
 */
enum read_stop {
	READ_OK = 0,
	READ_TIMEOUT,     /* SO_RCVTIMEO expired: nothing arrived in time */
	READ_PEER_CLOSED, /* orderly close: the session ended */
	READ_ERROR        /* anything else, errno preserved */
};

static enum read_stop g_last_stop = READ_OK;

static const char *read_stop_name(enum read_stop s)
{
	switch (s) {
	case READ_OK:
		return "ok";
	case READ_TIMEOUT:
		return "TIMED OUT waiting for the first byte";
	case READ_PEER_CLOSED:
		return "PEER CLOSED the session";
	default:
		return "read error";
	}
}

/*
 * Bytes that arrived in the same read() as the 101, held until the
 * frame reader asks for them (#331).
 *
 * Every upgrade site below reads into a response buffer until it sees
 * "\r\n\r\n" and then stops looking. One read() routinely returns the
 * handshake response AND the first websocket frame, because the daemon
 * writes the 101 and then relays whatever the exec'd process has
 * already produced -- and console_term_child reports before it does
 * anything else. Everything past the header terminator used to be
 * dropped with the response buffer, and recv_ws_frame() then read from
 * a socket those bytes had already left.
 *
 * It failed as a HANG, not as corruption: the test waited 30 seconds
 * for a frame that had already been delivered and thrown away, while
 * the exec'd process sat in pause() having written it. Five of the last
 * ten cix releases failed this way, in clusters, which is what made it
 * read as flakiness in the daemon rather than a bug in the reader.
 *
 * Deliberately reset by ws_take_leftover() at every upgrade rather than
 * only filled: a scenario that left bytes here would otherwise feed
 * them to the NEXT scenario's frame parser, which is a worse and much
 * stranger failure than the one being fixed.
 */
static unsigned char g_ws_pushback[4096];
static size_t g_ws_pushback_len;

/*
 * Records whatever `resp` holds past the end of the headers. `got` is
 * the byte count, not strlen: these are frame bytes and may contain
 * NUL.
 */
static void ws_take_leftover(const char *resp, size_t got, const char *headers_end)
{
	size_t off;

	g_ws_pushback_len = 0;
	if (headers_end == NULL)
		return;
	off = (size_t)(headers_end - resp) + 4;
	if (got <= off)
		return;
	if (got - off > sizeof(g_ws_pushback))
		return; /* cannot happen with the response buffers in use here */
	memcpy(g_ws_pushback, resp + off, got - off);
	g_ws_pushback_len = got - off;
}

static int read_full(int fd, void *buf, size_t want)
{
	unsigned char *p = buf;
	size_t got = 0;

	/* Anything the handshake over-read is consumed first, in order,
	 * before the socket is touched. */
	if (g_ws_pushback_len > 0) {
		size_t take = g_ws_pushback_len < want ? g_ws_pushback_len : want;

		memcpy(p, g_ws_pushback, take);
		memmove(g_ws_pushback, g_ws_pushback + take, g_ws_pushback_len - take);
		g_ws_pushback_len -= take;
		got = take;
	}

	while (got < want) {
		ssize_t n = read(fd, p + got, want - got);

		if (n == 0) {
			g_last_stop = READ_PEER_CLOSED;
			return -1;
		}
		if (n < 0) {
			g_last_stop = (errno == EAGAIN || errno == EWOULDBLOCK) ? READ_TIMEOUT : READ_ERROR;
			return -1;
		}
		got += (size_t)n;
	}
	return 0;
}

static int recv_ws_frame(int fd, int *out_opcode, unsigned char *out_buf, size_t out_cap, size_t *out_len)
{
	unsigned char hdr[4];
	int opcode;
	size_t len7, payload_len, extra = 0;

	if (read_full(fd, hdr, 2) != 0)
		return -1;
	opcode = hdr[0] & 0x0f;
	len7 = hdr[1] & 0x7f;

	if (len7 == 126) {
		unsigned char ext[2];

		if (read_full(fd, ext, 2) != 0)
			return -1;
		payload_len = ((size_t)ext[0] << 8) | (size_t)ext[1];
	} else {
		payload_len = len7;
	}
	(void)extra;

	if (payload_len > out_cap)
		return -1;

	/* The same loop the header now uses -- one implementation, not a
	 * second copy of it three lines further down. */
	if (payload_len > 0 && read_full(fd, out_buf, payload_len) != 0)
		return -1;

	*out_opcode = opcode;
	*out_len = payload_len;
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
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
	if (test_image_fixture_build(g_image_root, "build/console_term_child", "console_term_child") !=
	    0) {
		fprintf(stderr, "FAIL: could not stage console_term_child\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (test_image_fixture_build(g_image_root, "build/console_input_child", "console_input_child") !=
	    0) {
		fprintf(stderr, "FAIL: could not stage console_input_child\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;

	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never became healthy\n");
		stop_daemon(daemon_pid);
		return 1;
	}

	/* --- fixture: a real, long-lived running container --- */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       /* userns stated rather than defaulted (#293): the
	                        * console must enter the container's user namespace,
	                        * and a test that inherits userns_default asserts
	                        * nothing on a host where that default is off. */
	                       "{\"name\":\"consoletest\",\"image\":\"consoletest\","
	                       "\"userns\":true,"
	                       /* ADR-0261: there is no free-text cmd= any more, so every
	                        * program this file attaches to is declared here. Four is
	                        * REGISTRY_MAX_CONSOLES, which is why the missing-binary
	                        * case below gets a container of its own. */
	                       "\"consoles\":["
	                       "{\"name\":\"first\",\"cmd\":[\"/bin/dual_console_child\"]},"
	                       "{\"name\":\"second\",\"cmd\":[\"/bin/dual_console_child\"]},"
	                       "{\"name\":\"term\",\"cmd\":[\"/bin/console_term_child\"]},"
	                       "{\"name\":\"input\",\"cmd\":[\"/bin/console_input_child\"]}],"
	                       /* #331: 30 seconds was the REAL ceiling on this whole test, and
	                        * nothing said so. daemon_child's argv[1] is how long it
	                        * lives, every scenario below runs against this container,
	                        * and on a box compiling the control plane the scenarios do
	                        * not all fit inside half a minute. When they did not, the
	                        * container exited cleanly at 30s and the remaining reads
	                        * failed as PEER CLOSED -- reported as eleven separate
	                        * assertion failures including "Sec-WebSocket-Accept matches
	                        * the RFC 6455 worked example", which cannot fail for any
	                        * reason of its own and was simply downstream of a container
	                        * that had gone.
	                        *
	                        * 300s is not a timeout anything is measured against: the
	                        * test deletes the container when it finishes, so this only
	                        * bounds how long a HUNG run waits before the fixture gives
	                        * up on it. */
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"300\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST consoletest, status=%d\n", r.status);
		g_failures++;
	}
	cix_response_free(&r);

	/* --- scenario 1: a plain GET with no Upgrade header is a 400, not
	 * silently treated as an ordinary request for this path --- */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/consoletest/console", NULL, &r) != 0) {
		fprintf(stderr, "FAIL: plain GET .../console unreachable\n");
		g_failures++;
	} else {
		CHECK(r.status == 400, "plain GET .../console (no Upgrade header) should be 400");
	}
	cix_response_free(&r);

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

	/* --- scenario 2b: the container exists, the exec target does not
	 * (issue #108) ---
	 *
	 * This is the case that hung. exec_into_container() handed back a
	 * valid grandchild pid before that grandchild had tried to
	 * execve() anything, so a command missing from the container still
	 * looked like success: the daemon sent its 101, the client
	 * attached to a pty whose process was already dead, and then
	 * waited forever with no banner, no error and no exit. Really hit
	 * on a live container whose image has no shell at all.
	 *
	 * The assertion that matters is NOT the pty relay -- it is that
	 * the failure is reported before the upgrade, while an HTTP status
	 * can still carry it. A 101 here is the bug, whatever happens
	 * afterwards. */
	/* Its own container: consoletest already declares four consoles,
	 * which is REGISTRY_MAX_CONSOLES, and since ADR-0261 a program can
	 * only be reached by being declared. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"misscon\",\"image\":\"consoletest\",\"userns\":true,"
	                       "\"consoles\":[{\"name\":\"gone\","
	                       "\"cmd\":[\"/bin/definitely-not-in-this-image\"]}],"
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"300\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		CHECK(0, "POST misscon (a container declaring a console whose binary is absent)");
	}
	cix_response_free(&r);

	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect for missing-exec-target scenario");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/containers/misscon/console?console=gone HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "\r\n",
		                 TEST_WS_KEY);
		write_all_raw(fd, req, (size_t)rlen);
		n = read(fd, resp, sizeof(resp) - 1);
		CHECK(n > 0, "response for missing-exec-target console request");
		if (n > 0) {
			resp[n] = '\0';
			CHECK(strncmp(resp, "HTTP/1.1 101", 12) != 0,
			      "a missing exec target must NOT be upgraded to a websocket (#108)");
			CHECK(strstr(resp, "500") != NULL, "missing exec target -> 500");
			/* Naming the reason is the difference between a bug report
			 * and a shrug: "failed to start console session" alone is
			 * what made this undiagnosable in the first place. */
			CHECK(strstr(resp, "No such file or directory") != NULL,
			      "the 500 names the real errno, not just a generic failure");
		}
		close(fd);
	}

	/* --- issue #248: a container's consoles are DECLARED, and a
	 * container that declares none has none.
	 *
	 * These run before scenario 3 because they are ordinary REST
	 * responses on a container that is already up -- no upgrade, no
	 * PTY, nothing to tear down. --- */

	/* The declaration is echoed back, in order: a client cannot offer
	 * what it cannot see, and order is what makes "first declared" a
	 * usable default rather than an arbitrary one. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/consoletest", NULL, &r) == 0 &&
	    r.status == 200) {
		const struct json_value *cs = json_object_get(r.json, "consoles");

		CHECK(cs != NULL && cs->type == JSON_ARRAY && cs->u.array.count == 4,
		      "GET .../{name} echoes every declared console");
		if (cs != NULL && cs->type == JSON_ARRAY && cs->u.array.count == 4) {
			const char *n0 = json_as_string(json_object_get(cs->u.array.items[0], "name"));

			CHECK(n0 != NULL && strcmp(n0, "first") == 0,
			      "declaration order is preserved -- 'first' is first");
		}
	} else {
		CHECK(0, "GET /v1/containers/consoletest for console read-back");
	}
	cix_response_free(&r);

	/* A console this container does not declare is a 404 that LISTS
	 * what it does declare, rather than leaving the caller to guess at
	 * a set this very response already knows. */
	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect for unknown-console scenario");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/containers/consoletest/console?console=nope HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		                 "Sec-WebSocket-Version: 13\r\n\r\n");
		if (write(fd, req, (size_t)rlen) == rlen) {
			ssize_t n = read(fd, resp, sizeof(resp) - 1);

			if (n > 0)
				resp[n] = '\0';

			CHECK(n > 0 && strstr(resp, "404") != NULL,
			      "an undeclared console name is a 404");
			CHECK(n > 0 && strstr(resp, "first") != NULL &&
			              strstr(resp, "second") != NULL,
			      "the 404 lists the consoles this container DOES declare");
		}
		close(fd);
	}

	/* A container declaring nothing has no console: 409, naming the
	 * escape hatch rather than a bare refusal. This is the whole point
	 * of the change -- before it, every container got a hardcoded
	 * /usr/bin/bash and a container without one got a session that
	 * opened and instantly died. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"nocon\",\"image\":\"consoletest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"300\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		CHECK(0, "POST nocon (a container declaring no console)");
	}
	cix_response_free(&r);

	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect for no-console scenario");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/containers/nocon/console HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		                 "Sec-WebSocket-Version: 13\r\n\r\n");
		if (write(fd, req, (size_t)rlen) == rlen) {
			ssize_t n = read(fd, resp, sizeof(resp) - 1);

			if (n > 0)
				resp[n] = '\0';

			CHECK(n > 0 && strstr(resp, "409") != NULL,
			      "a container declaring no console refuses with 409");
			CHECK(n > 0 && strstr(resp, "definition") != NULL,
			      "the 409 says how to fix it -- declare one -- since ADR-0261 left no "
			      "override to point at");
		}
		close(fd);
	}

	/* A malformed parameter is refused before the upgrade, while an
	 * HTTP status can still carry the reason. Silently substituting a
	 * default here would leave the caller believing a geometry it does
	 * not have. */
	{
		static const struct {
			const char *query;
			const char *what;
		} bad[] = {
			{ "?cols=0", "cols=0 (a 0-wide terminal is never legitimate)" },
			{ "?cols=99999", "cols beyond the accepted range" },
			{ "?cols=wide", "a non-numeric cols" },
			{ "?rows=0", "rows=0" },
			{ "?rows=-5", "a negative rows" },
			{ "?term=xterm;rm%20-rf", "a term carrying characters no terminfo name uses" },
		};
		size_t bi;

		for (bi = 0; bi < sizeof(bad) / sizeof(bad[0]); bi++) {
			fd = raw_connect(TEST_PORT);
			if (fd < 0) {
				CHECK(0, "raw_connect for malformed-parameter scenario");
				continue;
			}
			rlen = snprintf(req, sizeof(req),
			                 "GET /v1/containers/consoletest/console%s HTTP/1.1\r\n"
			                 "Host: 127.0.0.1\r\n"
			                 "Upgrade: websocket\r\n"
			                 "Connection: Upgrade\r\n"
			                 "Sec-WebSocket-Key: %s\r\n"
			                 "Sec-WebSocket-Version: 13\r\n"
			                 "\r\n",
			                 bad[bi].query, TEST_WS_KEY);
			write_all_raw(fd, req, (size_t)rlen);
			n = read(fd, resp, sizeof(resp) - 1);
			if (n > 0) {
				resp[n] = '\0';
				CHECK(strncmp(resp, "HTTP/1.1 101", 12) != 0 && strstr(resp, "400") != NULL,
				      bad[bi].what);
			} else {
				CHECK(0, "response for malformed-parameter console request");
			}
			close(fd);
		}
	}

	/* The whole point, end to end: what the program actually sees. */
	{
		static const struct {
			const char *query;
			const char *expect;
			const char *what;
		} geom[] = {
			{ "?term=xterm-256color&cols=203&rows=51",
			  "TERMINFO TERM=xterm-256color COLS=203 ROWS=51",
			  "the exec'd process sees the requested $TERM and window size" },
			/*
			 * #278: an exec'd process must be an ordinary OOM
			 * candidate. cixd sets oom_score_adj -1000 on itself so
			 * the only management path on the box is never the OOM
			 * killer's choice, and that value is inherited across
			 * fork() AND execve() -- so with cixd as pid 1, every
			 * process on the host used to be exempt. A memory cgroup
			 * whose every task is exempt does not fail at its ceiling,
			 * it livelocks: on 192.168.15.95 that was 1.18 million
			 * failed allocations still climbing, ~1000 kernel log
			 * lines a second, and a box that needed a manual reset.
			 */
			{ "", "OOMADJ=0",
			  "the exec'd process is a killable OOM candidate, not exempt (#278)" },
			/*
			 * #290: the pty must be one the process can NAME, not
			 * merely one it can read and write.
			 *
			 * It used to be allocated from the daemon's own devpts,
			 * so the slave fd worked perfectly while ttyname() failed
			 * ENODEV -- /proc/self/fd/0 said /dev/pts/0 and the
			 * container's own /dev/pts held no such entry. A shell
			 * never notices, which is why this went unseen; login(1)
			 * reports the failure to syslog rather than to the
			 * terminal and exits five seconds later in silence.
			 *
			 * Asserting the /dev/pts/ prefix rather than an exact
			 * number: which slave a fresh devpts instance hands out
			 * is not this test's business, only that it named one.
			 */
			{ "", "TTY=/dev/pts/",
			  "the pty comes from the container's own devpts, so ttyname() resolves (#290)" },
			/*
			 * #293: the session must be INSIDE the container's user
			 * namespace, not merely inside its mount namespace.
			 *
			 * join_namespaces() entered mnt, uts, net and pid and not
			 * user, so a console ran with host credentials against the
			 * container's filesystem: every on-disk id read unmapped,
			 * a login could not reach its own home directory, and the
			 * isolation userns exists to provide was absent on this
			 * path entirely. Every assertion in this table passed
			 * throughout, which is why it went unseen -- a command
			 * runs perfectly well with the wrong credentials.
			 *
			 * Asserting the mapping is non-identity rather than its
			 * exact base: which subordinate range this container was
			 * assigned is not this test's business, only that the
			 * session is subject to one.
			 */
			{ "", "USERNS=mapped",
			  "the console session joins the container's user namespace (#293)" },
			/*
			 * #293, second half: joining the namespace is not
			 * enough. setns(CLONE_NEWUSER) reinterprets the uid
			 * already held rather than carrying one across, and the
			 * daemon's host 0 is not in the container's map -- so
			 * the first version of this fix produced a session that
			 * was correctly inside the namespace and had no identity
			 * in it, which bash greets with "I have no name!" and
			 * which owns nothing. Asserting the namespace without
			 * asserting the uid inside it would have passed that.
			 */
			{ "", "EUID=0",
			  "the console session is the container's own root, not the overflow uid (#293)" },
			/* Omitting everything is an ordinary request, not an
			 * error -- a piped client has no terminal to describe.
			 * The documented defaults are what it must then get,
			 * and 80x24 rather than the 0x0 a bare posix_openpt()
			 * leaves behind is the entire fix. */
			{ "", "TERMINFO TERM=xterm-256color COLS=80 ROWS=24",
			  "no parameters gives the documented defaults, not a 0x0 terminal" },
			/* One parameter given, the others defaulted -- they are
			 * independent, not an all-or-nothing group. */
			{ "?cols=132", "TERMINFO TERM=xterm-256color COLS=132 ROWS=24",
			  "an omitted parameter defaults independently of a given one" },
		};
		size_t gi;

		for (gi = 0; gi < sizeof(geom) / sizeof(geom[0]); gi++) {
			fd = raw_connect(TEST_PORT);
			if (fd < 0) {
				CHECK(0, "raw_connect for terminal-geometry scenario");
				continue;
			}
			rlen = snprintf(req, sizeof(req),
			                 "GET /v1/containers/consoletest/console%s%sconsole=term HTTP/1.1\r\n"
			                 "Host: 127.0.0.1\r\n"
			                 "Upgrade: websocket\r\n"
			                 "Connection: Upgrade\r\n"
			                 "Sec-WebSocket-Key: %s\r\n"
			                 "Sec-WebSocket-Version: 13\r\n"
			                 "\r\n",
			                 geom[gi].query, geom[gi].query[0] == '\0' ? "?" : "&",
			                 TEST_WS_KEY);
			write_all_raw(fd, req, (size_t)rlen);

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
			ws_take_leftover(resp, got, headers_end);
			if (headers_end == NULL || strncmp(resp, "HTTP/1.1 101", 12) != 0) {
				CHECK(0, "terminal-geometry request upgraded to a websocket");
				close(fd);
				continue;
			}

			{
				char acc[1024];
				size_t acc_len = 0;
				int found = 0;
				int attempts;
				int frames = 0;
				int recv_failed = 0;
				struct timespec t_wait0;

				acc[0] = '\0';
				g_last_stop = READ_OK;
				clock_gettime(CLOCK_MONOTONIC, &t_wait0);
				for (attempts = 0; attempts < 20 && !found; attempts++) {
					int opcode;
					unsigned char buf[512];
					size_t len;

					if (recv_ws_frame(fd, &opcode, buf, sizeof(buf), &len) != 0) {
						recv_failed = 1;
						break;
					}
					frames++;
					if (len > 0 && acc_len + len < sizeof(acc)) {
						memcpy(acc + acc_len, buf, len);
						acc_len += len;
						acc[acc_len] = '\0';
					}
					if (strstr(acc, geom[gi].expect) != NULL)
						found = 1;
				}
				if (!found) {
					/*
					 * Say what actually arrived, and HOW the wait
					 * ended (#291).
					 *
					 * "got (0 bytes): (nothing)" was the whole of
					 * this report, and it is the same output for
					 * three different faults: the socket read timed
					 * out, the peer closed, or frames arrived
					 * carrying something else. Three intermittent
					 * failures of this test were investigated with
					 * no way to tell those apart, so the next one
					 * says which.
					 */
					struct timespec t_wait1;
					long waited_ms;

					clock_gettime(CLOCK_MONOTONIC, &t_wait1);
					waited_ms = (t_wait1.tv_sec - t_wait0.tv_sec) * 1000L +
					            (t_wait1.tv_nsec - t_wait0.tv_nsec) / 1000000L;
					fprintf(stderr,
					        "  wanted: %s\n  got (%zu bytes in %d frame(s) after %ldms, "
					        "ended: %s): %s\n",
					        geom[gi].expect, acc_len, frames, waited_ms,
					        recv_failed ? read_stop_name(g_last_stop)
					                    : "20 frames without a match",
					        acc_len > 0 ? acc : "(nothing)");
					report_loop_health(&client);
				report_container_processes(&client, "consoletest");
				report_console_log(&client);
				}
				CHECK(found, geom[gi].what);
			}
			close(fd);
		}
	}

	/*
	 * #296: the INPUT direction, end to end -- which nothing in this
	 * file tested until now, and that gap is exactly how #294 shipped.
	 *
	 * Every scenario above reads what the exec'd process SAYS. A
	 * console that draws its prompt and then ignores every keystroke
	 * passes all of them, and that is precisely what reached a real
	 * host: struct console_exec_session was malloc()ed and never
	 * zeroed, so the pty output buffer's length came up as garbage,
	 * the direct write was skipped, and terminal input was copied to a
	 * junk offset. The suite was green throughout.
	 *
	 * A single marker coming back proves the whole chain in the
	 * direction that was unasserted: bytes leave here as a masked
	 * BINARY frame, cross the daemon's websocket relay, are written to
	 * the pty master (the write path #294 rebuilt as non-blocking with
	 * an EPOLLOUT drain), reach the slave, are read by a real exec'd
	 * process, and return the other way.
	 *
	 * BINARY, not text: the opcode is the discriminator between a
	 * keystroke and a control message (ADR-0242), so sending this as
	 * text would test the resize path instead.
	 */
	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect for console-input scenario");
	if (fd >= 0) {
		static const char probe[] = "console-input-probe\n";

		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/containers/consoletest/console?console=input HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "\r\n",
		                 TEST_WS_KEY);
		write_all_raw(fd, req, (size_t)rlen);

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
		ws_take_leftover(resp, got, headers_end);
		if (headers_end == NULL || strncmp(resp, "HTTP/1.1 101", 12) != 0) {
			CHECK(0, "console-input request upgraded to a websocket");
		} else {
			char acc[2048];
			size_t acc_len = 0;
			int ready = 0, echoed = 0, sent = 0;
			int attempts, frames = 0, recv_failed = 0;

			acc[0] = '\0';
			/*
			 * Wait for INPUT-READY before writing, so a missing
			 * echo cannot be blamed on having typed at a process
			 * that had not reached its read() yet. Then send once
			 * and keep reading: the pty echoes the input back as
			 * well, so the marker is looked for in everything
			 * received rather than in one particular frame.
			 */
			for (attempts = 0; attempts < 40 && !echoed; attempts++) {
				int opcode;
				unsigned char buf[512];
				size_t len;

				if (ready && !sent) {
					CHECK(send_ws_frame(fd, 0x2 /* binary -- a keystroke */,
					                     probe, sizeof(probe) - 1) == 0,
					      "sending console input as a binary frame");
					sent = 1;
				}
				if (recv_ws_frame(fd, &opcode, buf, sizeof(buf), &len) != 0) {
					recv_failed = 1;
					break;
				}
				frames++;
				if (len > 0 && acc_len + len < sizeof(acc)) {
					memcpy(acc + acc_len, buf, len);
					acc_len += len;
					acc[acc_len] = '\0';
				}
				if (strstr(acc, "INPUT-READY") != NULL)
					ready = 1;
				if (strstr(acc, "INPUT-ECHO:console-input-probe") != NULL)
					echoed = 1;
			}
			if (!echoed) {
				fprintf(stderr,
				        "  wanted: INPUT-ECHO:console-input-probe\n"
				        "  got (%zu bytes in %d frame(s), ready=%d sent=%d, ended: %s): %s\n",
				        acc_len, frames, ready, sent,
				        recv_failed ? read_stop_name(g_last_stop)
				                    : "40 frames without a match",
				        acc_len > 0 ? acc : "(nothing)");
				report_loop_health(&client);
				report_container_processes(&client, "consoletest");
				report_console_log(&client);
			}
			CHECK(ready, "the exec'd process reached its read() and said so");
			CHECK(echoed,
			      "a keystroke written to the console reaches the exec'd process (#296)");
		}
		close(fd);
	}

	/* A resize on a LIVE session. The daemon sends no signal itself --
	 * applying the size to the pty is what makes the kernel raise
	 * SIGWINCH on the foreground process group, so a second report
	 * arriving at all is the proof that the whole chain worked.
	 *
	 * Sent as a TEXT frame: keystrokes are BINARY, and the opcode is
	 * the discriminator between the two (ADR-0242). */
	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect for live-resize scenario");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/containers/consoletest/console?cols=80&rows=24&console=term HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "\r\n",
		                 TEST_WS_KEY);
		write_all_raw(fd, req, (size_t)rlen);

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
		ws_take_leftover(resp, got, headers_end);
		CHECK(headers_end != NULL && strncmp(resp, "HTTP/1.1 101", 12) == 0,
		      "live-resize session upgraded");

		if (headers_end != NULL) {
			char acc[1024];
			size_t acc_len = 0;
			int attempts;
			int saw_first = 0, saw_second = 0;
			int first_frames = 0, first_recv_failed = 0;

			acc[0] = '\0';
			/* Wait for the startup report first, so the resize
			 * cannot land before the child has installed its own
			 * SIGWINCH handler -- otherwise a pass would depend on
			 * scheduling rather than on the mechanism. */
			for (attempts = 0; attempts < 20 && !saw_first; attempts++) {
				int opcode;
				unsigned char buf[512];
				size_t len;

				if (recv_ws_frame(fd, &opcode, buf, sizeof(buf), &len) != 0) {
					first_recv_failed = 1;
					break;
				}
				first_frames++;
				if (len > 0 && acc_len + len < sizeof(acc)) {
					memcpy(acc + acc_len, buf, len);
					acc_len += len;
					acc[acc_len] = '\0';
				}
				if (strstr(acc, "TERMINFO TERM=xterm-256color COLS=80 ROWS=24") != NULL)
					saw_first = 1;
			}
			/* #291: same reason as the geometry loop above -- a bare
			 * failure here cannot be told apart from a timeout. */
			if (!saw_first) {
				fprintf(stderr, "  got (%zu bytes in %d frame(s), ended: %s): %s\n", acc_len,
				        first_frames,
				        first_recv_failed ? read_stop_name(g_last_stop)
				                          : "20 frames without a match",
				        acc_len > 0 ? acc : "(nothing)");
				report_loop_health(&client);
				report_container_processes(&client, "consoletest");
				report_console_log(&client);
			}
			CHECK(saw_first, "the initial report arrived before any resize");

			if (saw_first) {
				static const char resize[] = "{\"type\":\"resize\",\"cols\":120,\"rows\":40}";

				CHECK(send_ws_frame(fd, 0x1 /* text -- a control message, not input */,
				                     resize, sizeof(resize) - 1) == 0,
				      "send the resize control message");

				for (attempts = 0; attempts < 20 && !saw_second; attempts++) {
					int opcode;
					unsigned char buf[512];
					size_t len;

					if (recv_ws_frame(fd, &opcode, buf, sizeof(buf), &len) != 0)
						break;
					if (len > 0 && acc_len + len < sizeof(acc)) {
						memcpy(acc + acc_len, buf, len);
						acc_len += len;
						acc[acc_len] = '\0';
					}
					if (strstr(acc, "TERMINFO TERM=xterm-256color COLS=120 ROWS=40") != NULL)
						saw_second = 1;
				}
				CHECK(saw_second,
				      "a resize control message resized the live pty and raised SIGWINCH");

				/* A malformed control message must NOT kill the
				 * session: it is advisory about presentation, and a
				 * live shell is worth far more than strictness here.
				 * The session staying usable is the assertion. */
				send_ws_frame(fd, 0x1, "not json at all", 15);
				send_ws_frame(fd, 0x1, "{\"type\":\"unknown-to-this-daemon\"}", 33);
				send_ws_frame(fd, 0x1, "{\"type\":\"resize\",\"cols\":0,\"rows\":0}", 36);
				{
					static const char good[] = "{\"type\":\"resize\",\"cols\":90,\"rows\":30}";
					int saw_third = 0;

					send_ws_frame(fd, 0x1, good, sizeof(good) - 1);
					for (attempts = 0; attempts < 20 && !saw_third; attempts++) {
						int opcode;
						unsigned char buf[512];
						size_t len;

						if (recv_ws_frame(fd, &opcode, buf, sizeof(buf), &len) != 0)
							break;
						if (len > 0 && acc_len + len < sizeof(acc)) {
							memcpy(acc + acc_len, buf, len);
							acc_len += len;
							acc[acc_len] = '\0';
						}
						if (strstr(acc, "TERMINFO TERM=xterm-256color COLS=90 ROWS=30") != NULL)
							saw_third = 1;
					}
					CHECK(saw_third,
					      "the session survived malformed control messages and still resizes");
				}
			}
		}
		close(fd);
	}

	/* --- scenario 3: the real thing -- upgrade, exec dual_console_child,
	 * exchange real lines, confirm clean teardown --- */
	fd = raw_connect(TEST_PORT);
	CHECK(fd >= 0, "raw_connect for real console session");
	if (fd >= 0) {
		rlen = snprintf(req, sizeof(req),
		                 "GET /v1/containers/consoletest/console?console=first HTTP/1.1\r\n"
		                 "Host: 127.0.0.1\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
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
		ws_take_leftover(resp, got, headers_end);
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
	cix_client_request(&client, "DELETE", "/v1/containers/consoletest", NULL, &r);
	cix_response_free(&r);

	stop_daemon(daemon_pid);
	reset_state();
	test_data_dir_cleanup(g_data_dir);

	if (g_failures == 0)
		printf("CONSOLE EXEC TEST: PASS\n");
	else
		printf("CONSOLE EXEC TEST: FAIL (%d failure(s))\n", g_failures);

	return g_failures == 0 ? 0 : 1;
}
