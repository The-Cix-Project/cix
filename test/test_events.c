/*
 * ADR-0339 (#599): the box tells its clients what changed.
 *
 * Against a real daemon, the first half of that promise -- the part a
 * client can check over plain HTTP:
 *
 *   - the answer to an announced read carries X-Cix-Generation,
 *     "<epoch>.<generation>", and GET /v1/events/status agrees with it;
 *   - reading never announces anything;
 *   - a change that succeeds raises the generation of every read its
 *     contract names in x-cix-invalidates;
 *   - a change that is refused raises nothing, because nothing changed;
 *   - an answer that is not an announced read carries no header.
 *
 * The site config is the instrument: putSystemSite invalidates
 * getSystemSite, both are cheap, and a fresh data directory has one.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"
#include "test_ws.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7851

static char g_data_dir[PATH_MAX];
static int g_failures;

static void fail(const char *what, const char *detail)
{
	fprintf(stderr, "FAIL: %s%s%s\n", what, detail[0] != '\0' ? ": " : "", detail);
	g_failures++;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = "--port=7851";
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

/*
 * GET path; on a 200, copies its X-Cix-Generation into gen (empty when
 * absent) and returns 0. Anything else fails the named check.
 */
static int read_generation(const struct cix_client *c, const char *path, char *gen,
                           size_t gen_size, const char *what)
{
	struct cix_response r;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", path, NULL, &r) != 0 || r.status != 200) {
		char detail[96];

		snprintf(detail, sizeof(detail), "GET %s answered %d", path, r.status);
		fail(what, detail);
		cix_response_free(&r);
		return -1;
	}
	snprintf(gen, gen_size, "%s", r.generation);
	cix_response_free(&r);
	return 0;
}

/* "<epoch>.<n>" split at the last dot. Returns -1 when malformed. */
static int split_generation(const char *gen, char *epoch, size_t epoch_size,
                            unsigned long long *n)
{
	const char *dot = strrchr(gen, '.');
	char *end;

	if (dot == NULL || dot == gen || (size_t)(dot - gen) >= epoch_size)
		return -1;
	memcpy(epoch, gen, (size_t)(dot - gen));
	epoch[dot - gen] = '\0';
	*n = strtoull(dot + 1, &end, 10);
	return end != dot + 1 && *end == '\0' ? 0 : -1;
}

/*
 * Opens GET /v1/events as a WebSocket, through the shared reader
 * (test_ws.c, #524) so the bytes that ride in with the 101 -- here
 * always the first frame -- are the front of the stream rather than
 * lost. Returns the fd, or -1 having said why.
 */
static int open_events(struct ws_reader *r, int timeout_seconds)
{
	struct sockaddr_in addr;
	struct timeval tv;
	char resp[2048];
	const char *end = NULL;
	size_t got = 0;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	static const char req[] =
	        "GET /v1/events HTTP/1.1\r\n"
	        "Host: 127.0.0.1\r\n"
	        "Upgrade: websocket\r\n"
	        "Connection: Upgrade\r\n"
	        "Sec-WebSocket-Key: " TEST_WS_KEY "\r\n"
	        "Sec-WebSocket-Version: 13\r\n"
	        "\r\n";

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(TEST_PORT);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	tv.tv_sec = timeout_seconds;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    write(fd, req, sizeof(req) - 1) != (ssize_t)(sizeof(req) - 1)) {
		close(fd);
		fail("the event stream accepts a connection", "");
		return -1;
	}
	while (got < sizeof(resp) - 1 && end == NULL) {
		ssize_t n = read(fd, resp + got, sizeof(resp) - 1 - got);

		if (n <= 0)
			break;
		got += (size_t)n;
		resp[got] = '\0';
		end = strstr(resp, "\r\n\r\n");
	}
	if (end == NULL || strncmp(resp, "HTTP/1.1 101", 12) != 0 ||
	    strstr(resp, TEST_WS_ACCEPT) == NULL) {
		close(fd);
		fail("GET /v1/events upgrades to a WebSocket", "");
		return -1;
	}
	if (ws_reader_adopt_handshake(r, fd, resp, got, end) != 0) {
		close(fd);
		fail("the handshake's leftover fits the reader", "");
		return -1;
	}
	return fd;
}

/*
 * The next text frame, parsed. Returns the JSON (caller frees) or NULL
 * having said why; a pong is returned as NULL with *pong set.
 */
static struct json_value *next_event(struct ws_reader *r, const char *what, int *pong)
{
	static unsigned char buf[70000];
	size_t len = 0;
	int opcode = 0;
	struct json_value *v;

	if (pong != NULL)
		*pong = 0;
	if (ws_recv_frame(r, &opcode, buf, sizeof(buf) - 1, &len) != 0) {
		fail(what, ws_stop_str(r));
		return NULL;
	}
	if (opcode == 0xA && pong != NULL) {
		*pong = 1;
		return NULL;
	}
	buf[len] = '\0';
	v = opcode == 0x1 ? json_parse((const char *)buf, len) : NULL;
	if (v == NULL)
		fail(what, "the frame is not a JSON text frame");
	return v;
}

/* A frame's generation for one read, or -1 when the frame does not name it. */
static long long frame_gen(const struct json_value *v, const char *op)
{
	const struct json_value *gen = json_object_get(v, "gen");
	const struct json_value *g = gen != NULL ? json_object_get(gen, op) : NULL;

	return g != NULL ? (long long)json_as_number(g) : -1;
}

/* How many reads a frame names. */
static size_t frame_count(const struct json_value *v)
{
	const struct json_value *gen = json_object_get(v, "gen");

	return gen != NULL && gen->type == JSON_OBJECT ? gen->u.object.count : 0;
}

/* The next frame carrying `key`, skipping announcement frames between;
 * NULL, having said why, when 20 frames pass without one. */
static struct json_value *next_reply(struct ws_reader *r, const char *key, const char *what)
{
	int i;

	for (i = 0; i < 20; i++) {
		struct json_value *v = next_event(r, what, NULL);

		if (v == NULL)
			return NULL;
		if (json_object_get(v, key) != NULL)
			return v;
		json_free(v);
	}
	fail(what, "20 frames went by without one");
	return NULL;
}

/* Sends one subscribe/unsubscribe text frame. */
static int send_text(int fd, const char *text)
{
	return ws_send_frame(fd, 0x1, text, strlen(text));
}

/* GET /v1/events/status's counter `key`, or -1. */
static long long status_counter(const struct cix_client *c, const char *key)
{
	struct cix_response r;
	long long n = -1;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", "/v1/events/status", NULL, &r) == 0 && r.json != NULL &&
	    json_object_get(r.json, key) != NULL)
		n = (long long)json_as_number(json_object_get(r.json, key));
	cix_response_free(&r);
	return n;
}

/* A refusal for `id` whose reason contains `want`. */
static void expect_refused(struct ws_reader *r, int fd, const char *msg, const char *id,
                           const char *want, const char *what)
{
	struct json_value *v;
	const char *got, *reason;

	if (send_text(fd, msg) != 0) {
		fail(what, "could not send the subscribe");
		return;
	}
	v = next_reply(r, "refused", what);
	if (v == NULL)
		return;
	got = json_as_string(json_object_get(v, "refused"));
	reason = json_as_string(json_object_get(v, "reason"));
	if (got == NULL || strcmp(got, id) != 0 || reason == NULL || strstr(reason, want) == NULL)
		fail(what, reason != NULL ? reason : "no reason");
	json_free(v);
}

/* Elapsed milliseconds, so a bound is not on whole seconds (#309). */
static long elapsed_ms(const struct timespec *t0, const struct timespec *t1)
{
	return (t1->tv_sec - t0->tv_sec) * 1000L + (t1->tv_nsec - t0->tv_nsec) / 1000000L;
}

/* PUT /v1/system/site with body; returns the status. */
static int put_site(const struct cix_client *c, const char *body, char *gen, size_t gen_size)
{
	struct cix_response r;
	int status;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "PUT", "/v1/system/site", body, &r) != 0)
		return -1;
	status = r.status;
	snprintf(gen, gen_size, "%s", r.generation);
	cix_response_free(&r);
	return status;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	char gen0[48], gen1[48], gen2[48], gen3[48], change_gen[48];
	char epoch0[24] = "", epoch1[24] = "";
	unsigned long long n0 = 0, n2 = 0, n3 = 0;
	int status;

	printf("test_events\n");
	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. An announced read says which generation it is. */
	if (read_generation(&client, "/v1/system/site", gen0, sizeof(gen0),
	                    "an announced read answers") == 0 &&
	    split_generation(gen0, epoch0, sizeof(epoch0), &n0) != 0)
		fail("an announced read carries X-Cix-Generation as <epoch>.<n>", gen0);

	/* 2. The status read agrees with the header, and is not itself
	 *    announced (it is derived: it changes on every announcement). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/events/status", NULL, &r) != 0 ||
	    r.status != 200 || r.json == NULL) {
		fail("GET /v1/events/status answers 200 with JSON", "");
	} else {
		const struct json_value *ep = json_object_get(r.json, "epoch");
		const struct json_value *gens = json_object_get(r.json, "generations");
		const struct json_value *site =
		        gens != NULL ? json_object_get(gens, "getSystemSite") : NULL;

		if (ep == NULL || ep->type != JSON_STRING || strcmp(ep->u.string, epoch0) != 0)
			fail("events/status reports the epoch the header carries", epoch0);
		if (site == NULL || (unsigned long long)json_as_number(site) != n0)
			fail("events/status reports getSystemSite at the header's generation", gen0);
		if (gens != NULL && json_object_get(gens, "getSystemStats") != NULL)
			fail("a stream read has no generation", "getSystemStats is listed");
		if (r.generation[0] != '\0')
			fail("a derived read carries no X-Cix-Generation", r.generation);
	}
	cix_response_free(&r);

	/* 3. Reading announces nothing. */
	if (read_generation(&client, "/v1/system/site", gen1, sizeof(gen1),
	                    "a second read answers") == 0 &&
	    strcmp(gen1, gen0) != 0)
		fail("reading does not raise a generation", gen1);

	/* 4. A change that succeeds raises what it invalidates, once. */
	status = put_site(&client,
	                  "{\"instance_name\":\"evtest\",\"site_name\":\"lab\","
	                  "\"domain_suffix\":\"internal\"}",
	                  change_gen, sizeof(change_gen));
	if (status != 200) {
		char detail[32];

		snprintf(detail, sizeof(detail), "status %d", status);
		fail("a valid site change is accepted", detail);
	}
	if (change_gen[0] != '\0')
		fail("a change's own answer carries no X-Cix-Generation", change_gen);
	if (read_generation(&client, "/v1/system/site", gen2, sizeof(gen2),
	                    "the read after a change answers") == 0) {
		if (split_generation(gen2, epoch1, sizeof(epoch1), &n2) != 0 ||
		    strcmp(epoch1, epoch0) != 0)
			fail("the epoch is the same within one daemon's life", gen2);
		else if (n2 != n0 + 1)
			fail("a change raises what it invalidates, exactly once", gen2);
	}

	/* 5. A refused change raises nothing: nothing changed. */
	status = put_site(&client, "{", change_gen, sizeof(change_gen));
	if (status < 400 || status >= 500) {
		char detail[32];

		snprintf(detail, sizeof(detail), "status %d", status);
		fail("an invalid site change is refused", detail);
	}
	if (read_generation(&client, "/v1/system/site", gen3, sizeof(gen3),
	                    "the read after a refusal answers") == 0 &&
	    (split_generation(gen3, epoch1, sizeof(epoch1), &n3) != 0 || n3 != n2))
		fail("a refused change raises nothing", gen3);

	/* 6. The stream: the first frame is the whole vector, under the
	 *    epoch the header carries. */
	{
		struct ws_reader r, idle;
		struct json_value *v;
		const struct json_value *ep;
		int fd, idle_fd, pong = 0, i, frames;
		long long g;
		char body[160];
		struct timespec t0, t1;

		fd = open_events(&r, 5);
		v = fd >= 0 ? next_event(&r, "the stream's first frame arrives", NULL) : NULL;
		if (v != NULL) {
			ep = json_object_get(v, "epoch");
			if (ep == NULL || ep->type != JSON_STRING || strcmp(ep->u.string, epoch0) != 0)
				fail("the stream carries the epoch the header carries", epoch0);
			if (frame_gen(v, "getSystemSite") != (long long)n3)
				fail("the first frame names getSystemSite at its current generation", gen3);
			if (frame_count(v) < 100)
				fail("the first frame is the whole vector", "it names fewer than 100 reads");
			if (frame_gen(v, "getSystemStats") != -1)
				fail("a stream read is never announced", "getSystemStats is in the frame");
			json_free(v);
		}

		/* 7. A change is pushed: the next frame names the read it
		 *    invalidated, at its new generation, and nothing unrelated. */
		if (fd >= 0) {
			status = put_site(&client,
			                  "{\"instance_name\":\"evtest2\",\"site_name\":\"lab\","
			                  "\"domain_suffix\":\"internal\"}",
			                  change_gen, sizeof(change_gen));
			if (status != 200)
				fail("a second site change is accepted", "");
			/* A heartbeat (every 10 s, naming every read) may land
			 * first; it is skipped by its size, and the change frame
			 * is the first one that is not whole. */
			for (frames = 0; frames < 3; frames++) {
				v = next_event(&r, "a change is pushed as a frame", NULL);
				if (v == NULL || frame_count(v) < 100)
					break;
				json_free(v);
				v = NULL;
			}
			if (v != NULL) {
				if (frame_gen(v, "getSystemSite") != (long long)n3 + 1)
					fail("the pushed frame names getSystemSite at its new generation", "");
				if (frame_gen(v, "listNetworks") != -1)
					fail("a pushed frame names only what changed", "listNetworks is in it");
				json_free(v);
			} else {
				fail("a change frame arrives", "");
			}

			/* 8. A ping is answered -- possibly after a heartbeat. */
			if (ws_send_frame(fd, 0x9, "hi", 2) != 0) {
				fail("a ping can be sent", "");
			} else {
				for (frames = 0; frames < 3 && !pong; frames++) {
					v = next_event(&r, "a ping is answered", &pong);
					if (v == NULL && !pong)
						break;
					json_free(v);
				}
				if (!pong)
					fail("a ping is answered with a pong", "");
			}
		}

		/*
		 * 9. A stream that never reads cannot stall the daemon, and the
		 *    reading one still reaches the last generation. Each change
		 *    here is its own loop pass, so the reader gets about one
		 *    frame per change: coalescing happens only when a socket
		 *    will not take a whole frame, which loopback with this
		 *    little traffic never refuses, so this does not exercise it.
		 */
		idle_fd = open_events(&idle, 5);
		for (i = 0; i < 100 && fd >= 0; i++) {
			snprintf(body, sizeof(body),
			         "{\"instance_name\":\"evflood%d\",\"site_name\":\"lab\","
			         "\"domain_suffix\":\"internal\"}",
			         i);
			if (put_site(&client, body, change_gen, sizeof(change_gen)) != 200) {
				fail("the flood's changes are accepted", "");
				break;
			}
		}
		clock_gettime(CLOCK_MONOTONIC, &t0);
		{
			struct cix_response hr;

			memset(&hr, 0, sizeof(hr));
			if (cix_client_request(&client, "GET", "/v1/health", NULL, &hr) != 0 ||
			    hr.status != 200)
				fail("the daemon answers while a stream is not reading", "");
			cix_response_free(&hr);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		if (elapsed_ms(&t0, &t1) > 2000L)
			fail("a stream that does not read does not slow the daemon", "health took > 2000 ms");
		if (fd >= 0) {
			g = -1;
			for (frames = 0; frames < 1000 && g != (long long)n3 + 1 + 100; frames++) {
				v = next_event(&r, "the reading stream converges after the flood", NULL);
				if (v == NULL)
					break;
				if (frame_gen(v, "getSystemSite") >= 0)
					g = frame_gen(v, "getSystemSite");
				json_free(v);
			}
			if (g != (long long)n3 + 1 + 100)
				fail("the reading stream reaches the last generation", "");
		}

		/* 10. A heartbeat: with nothing changing, the whole vector
		 *     arrives again within the 10 s period. */
		if (fd >= 0) {
			struct timeval tv = { 15, 0 };

			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			for (frames = 0; frames < 3; frames++) {
				v = next_event(&r, "a heartbeat arrives within 15 s", NULL);
				if (v == NULL || frame_count(v) >= 100)
					break;
				json_free(v);
				v = NULL;
			}
			if (v == NULL)
				fail("a heartbeat carries the whole vector", "");
			json_free(v);
			close(fd);
		}
		if (idle_fd >= 0)
			close(idle_fd);
	}

	/*
	 * 10b. Subscriptions (ADR-0339 phase 2). A stream read with a period
	 *      is pushed rather than polled: sampled at once, then sent only
	 *      when it changes, and one run serves every subscriber to a
	 *      path. Authorization per subscription needs a gated session,
	 *      which this daemon does not run; it is verified on the box.
	 */
	{
		struct ws_reader a, b;
		int fa = open_events(&a, 10);
		int fb = open_events(&b, 10);
		struct json_value *v;
		long long runs0, runs1, frames0, frames1;
		char msg[160];
		int i;

		if (fa >= 0 && fb >= 0) {
			/* At once: the reply, then the first sample. */
			if (send_text(fa, "{\"subscribe\":\"quiet\","
			                  "\"path\":\"/v1/system/logs?source=nosuchsource&tail=5\"}") != 0)
				fail("a subscribe can be sent", "");
			v = next_reply(&a, "subscribed", "a subscribe is answered");
			json_free(v);
			v = next_reply(&a, "data", "the first sample arrives at once");
			if (v != NULL) {
				const struct json_value *body = json_object_get(v, "body");
				const char *id = json_as_string(json_object_get(v, "data"));

				if (id == NULL || strcmp(id, "quiet") != 0 || body == NULL ||
				    body->type != JSON_ARRAY)
					fail("a sample names its subscription and carries the read's own answer",
					     id != NULL ? id : "?");
				json_free(v);
			}

			/* Unchanged, nothing more is sent; a read that does not
			 * answer 200 sends nothing at all. */
			if (send_text(fa, "{\"subscribe\":\"gone\","
			                  "\"path\":\"/v1/containers/nosuchcontainer/stats\"}") != 0)
				fail("a second subscribe can be sent", "");
			v = next_reply(&a, "subscribed", "a subscribe to a missing container is made");
			json_free(v);
			frames0 = status_counter(&client, "subscription_frames");
			sleep(5);
			frames1 = status_counter(&client, "subscription_frames");
			if (frames0 < 0 || frames1 != frames0) {
				snprintf(msg, sizeof(msg), "%lld then %lld", frames0, frames1);
				fail("an unchanged answer, or a non-200 one, sends no sample", msg);
			}

			/* Two streams on one path: one run per period, not two. */
			if (send_text(fa, "{\"unsubscribe\":\"quiet\"}") != 0 ||
			    send_text(fa, "{\"unsubscribe\":\"gone\"}") != 0)
				fail("an unsubscribe can be sent", "");
			v = next_reply(&a, "unsubscribed", "an unsubscribe is answered");
			json_free(v);
			v = next_reply(&a, "unsubscribed", "the second unsubscribe is answered");
			json_free(v);
			if (send_text(fa, "{\"subscribe\":\"st\",\"path\":\"/v1/system/stats\"}") != 0 ||
			    send_text(fb, "{\"subscribe\":\"st\",\"path\":\"/v1/system/stats\"}") != 0)
				fail("two streams subscribe to one path", "");
			v = next_reply(&a, "data", "stream A's stats sample arrives");
			json_free(v);
			v = next_reply(&b, "data", "stream B's stats sample arrives");
			json_free(v);
			if (status_counter(&client, "subscriptions") != 2)
				fail("events/status counts the two subscriptions", "");
			runs0 = status_counter(&client, "subscription_runs");
			sleep(5);
			runs1 = status_counter(&client, "subscription_runs");
			/* A 2 s period over 5 s is two or three ticks: shared, that
			 * is two or three runs; run per subscriber, four to six. */
			if (runs0 < 0 || runs1 - runs0 < 1 || runs1 - runs0 > 3) {
				snprintf(msg, sizeof(msg), "%lld runs in 5 s for two subscribers to one path",
				         runs1 - runs0);
				fail("one run serves every subscriber to a path", msg);
			}

			/* What is refused, and why. */
			expect_refused(&a, fa, "{\"subscribe\":\"site\",\"path\":\"/v1/system/site\"}", "site",
			               "not subscribable", "a state read is not subscribable");
			expect_refused(&a, fa,
			               "{\"subscribe\":\"q\",\"path\":\"/v1/system/logs?nosuchparam=1\"}", "q",
			               "unknown query parameter", "a query the read does not take is refused");
			expect_refused(&a, fa, "{\"subscribe\":\"p\",\"path\":\"/etc/passwd\"}", "p",
			               "/v1/", "a path outside the API is refused");
			expect_refused(&a, fa, "{\"subscribe\":\"no spaces\",\"path\":\"/v1/system/stats\"}",
			               "no spaces", "expected", "an id outside its alphabet is refused");
			for (i = 1; i < 8; i++) {
				snprintf(msg, sizeof(msg),
				         "{\"subscribe\":\"cap%d\",\"path\":\"/v1/system/stats\"}", i);
				if (send_text(fa, msg) != 0)
					fail("a subscribe can be sent", "");
				v = next_reply(&a, "subscribed", "subscriptions up to the cap are made");
				json_free(v);
			}
			expect_refused(&a, fa, "{\"subscribe\":\"cap8\",\"path\":\"/v1/system/stats\"}", "cap8",
			               "already holds 8", "a ninth subscription is refused");
		}
		if (fa >= 0)
			close(fa);
		if (fb >= 0)
			close(fb);
	}

	/*
	 * 11. The daemon's own check (ADR-0339 point 7): after everything
	 *     this test changed, nothing changed that was not announced.
	 *     The baseline was taken at daemon start, so this compares the
	 *     whole run.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/events/verify", NULL, &r) != 0 ||
	    r.status != 200 || r.json == NULL) {
		fail("POST /v1/events/verify answers 200 with JSON", "");
	} else {
		const struct json_value *drift = json_object_get(r.json, "drift");
		const struct json_value *checked = json_object_get(r.json, "checked");

		if (drift == NULL || drift->type != JSON_ARRAY)
			fail("verify lists its drift", "");
		else if (drift->u.array.count != 0)
			fail("nothing this test did went unannounced",
			     drift->u.array.items[0]->type == JSON_STRING ? drift->u.array.items[0]->u.string
			                                                   : "?");
		if (checked == NULL || json_as_number(checked) < 50)
			fail("verify compared the state reads", "fewer than 50");
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/events/status", NULL, &r) != 0 || r.json == NULL ||
	    json_object_get(r.json, "drift") == NULL ||
	    json_as_number(json_object_get(r.json, "drift")) != 0) {
		struct cix_response lr;

		fail("events/status reports zero drift -- the rolling check found some during the run",
		     "");
		/* The daemon names each drifted read in its log store, which
		 * this test's data directory takes with it on exit -- so read
		 * it now, or a failure here says only that something drifted
		 * (logstore_write() never reaches stderr). */
		memset(&lr, 0, sizeof(lr));
		if (cix_client_request(&client, "GET", "/v1/system/logs?source=events&tail=20", NULL,
		                       &lr) == 0 &&
		    lr.body != NULL)
			fprintf(stderr, "      events log: %s\n", lr.body);
		cix_response_free(&lr);
	}
	cix_response_free(&r);

	if (stop_daemon(daemon_pid) != 0)
		fail("the daemon exits cleanly", "");
	test_data_dir_cleanup(g_data_dir);
	if (g_failures != 0) {
		printf("FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
