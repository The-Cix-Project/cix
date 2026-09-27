/*
 * Phase 5 end-to-end test: proves cixd's static-file serving
 * (daemon/src/staticfile.c) works over real HTTP -- correct status/
 * content-type per asset, 404 for missing files, path traversal
 * rejected, and that adding this didn't regress /v1/... routing.
 * Whether the dashboard actually renders and behaves correctly in a
 * real browser is a separate, manual check (docs/adr/0010) -- not
 * something this test (or any tool in this project) can verify.
 */
#include "httpclient.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7623
#define PORT_ARG "--port=7623"

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

static void check(const struct cix_client *c, const char *path, int want_status,
                   const char *want_content_type, const char *want_body_substr,
                   const char *forbid_body_substr, int *ok)
{
	struct cix_response r;

	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "FAIL: GET %s: transport error\n", path);
		*ok = 0;
		return;
	}

	if (r.status != want_status) {
		fprintf(stderr, "FAIL: GET %s: status %d, expected %d\n", path, r.status, want_status);
		*ok = 0;
	}
	if (want_content_type != NULL && strcmp(r.content_type, want_content_type) != 0) {
		fprintf(stderr, "FAIL: GET %s: content-type '%s', expected '%s'\n", path,
		        r.content_type, want_content_type);
		*ok = 0;
	}
	if (want_body_substr != NULL && (r.body == NULL || strstr(r.body, want_body_substr) == NULL)) {
		fprintf(stderr, "FAIL: GET %s: body missing expected substring '%s'\n", path,
		        want_body_substr);
		*ok = 0;
	}
	if (forbid_body_substr != NULL && r.body != NULL &&
	    strstr(r.body, forbid_body_substr) != NULL) {
		fprintf(stderr, "FAIL: GET %s: body unexpectedly contains '%s'\n", path,
		        forbid_body_substr);
		*ok = 0;
	}

	cix_response_free(&r);
}

/*
 * #498: a HEAD is the GET response's headers with no content.
 *
 * `curl -sI https://<host>/app.js` answered 401 where the same request
 * as a GET answered 200, because the write gate asked "is this a GET".
 * Opening that gate alone moved it to 404 -- the static-asset
 * fallthrough asked the identical question -- so this asserts the whole
 * round trip and not just the status.
 *
 * The three properties, and why each is here rather than implied by
 * the others:
 *
 *  - same status and content-type as the GET, so a client learns the
 *    same things about the resource;
 *  - a Content-Length equal to what the GET actually returned. This is
 *    the one the daemon computes differently (from fstat(), never from
 *    bytes read, because reading a file to discard it defeats the
 *    method) and therefore the one that can silently diverge. Asserting
 *    only "no body" would pass a HEAD that declared 0 bytes for a 2 MB
 *    script, which is a worse answer than the 404 this replaced;
 *  - no body at all, which is the actual saving.
 */
static void check_head(const struct cix_client *c, const char *path, const char *want_content_type,
                        int *ok)
{
	struct cix_response g, h;

	if (cix_client_request(c, "GET", path, NULL, &g) != 0) {
		fprintf(stderr, "FAIL: GET %s: transport error\n", path);
		*ok = 0;
		return;
	}
	if (cix_client_request(c, "HEAD", path, NULL, &h) != 0) {
		fprintf(stderr, "FAIL: HEAD %s: transport error\n", path);
		cix_response_free(&g);
		*ok = 0;
		return;
	}

	if (h.status != g.status) {
		fprintf(stderr, "FAIL: HEAD %s: status %d, but GET says %d\n", path, h.status, g.status);
		*ok = 0;
	}
	if (want_content_type != NULL && strcmp(h.content_type, want_content_type) != 0) {
		fprintf(stderr, "FAIL: HEAD %s: content-type '%s', expected '%s'\n", path,
		        h.content_type, want_content_type);
		*ok = 0;
	}
	if (h.content_length != (long)g.body_len) {
		fprintf(stderr,
		        "FAIL: HEAD %s: declared Content-Length %ld, but a GET returns %zu bytes\n", path,
		        h.content_length, g.body_len);
		*ok = 0;
	}
	if (h.body_len != 0) {
		fprintf(stderr, "FAIL: HEAD %s: sent %zu bytes of body; a HEAD sends none\n", path,
		        h.body_len);
		*ok = 0;
	}

	cix_response_free(&g);
	cix_response_free(&h);
}

int main(void)
{
	pid_t daemon_pid;
	char *dargv[4];
	char data_dir[PATH_MAX];
	char data_dir_arg[PATH_MAX + 11];
	struct cix_client client;
	int ok = 1;

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

	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(data_dir);
		return 1;
	}

	/* 1-2. index and its title marker */
	check(&client, "/", 200, "text/html", "Cix", NULL, &ok);
	/* 3. app.js */
	check(&client, "/app.js", 200, "application/javascript", "docs/api/openapi.yaml", NULL, &ok);
	/* 4. style.css */
	check(&client, "/style.css", 200, "text/css", NULL, NULL, &ok);
	/* 5. vt.js (ADR-0243) -- the terminal is its own asset, and an asset
	 * that is not staged 404s in the browser with nothing on the server
	 * side to notice. index.html loads it before app.js, which uses it,
	 * so a missing vt.js is a dashboard that throws on every console
	 * open rather than one that degrades. */
	check(&client, "/vt.js", 200, "application/javascript", "createVT", NULL, &ok);
	/* 5. missing asset */
	check(&client, "/nonexistent.txt", 404, NULL, NULL, NULL, &ok);
	/* 6. path traversal rejected before the filesystem is ever touched */
	check(&client, "/../CLAUDE.md", 400, NULL, NULL, "Immutable Maxims", &ok);
	/* 7. API routing unaffected by the new static-serving fallback */
	check(&client, "/v1/health", 200, "application/json", "\"status\":\"ok\"", NULL, &ok);

	/*
	 * 8-10. HEAD on a static asset (#498). Three assets rather than
	 * one: index.html is the "/" rewrite, app.js is the file the
	 * report was actually about and the only large one, and a missing
	 * path proves the 404 is still a 404 for HEAD rather than
	 * accidentally becoming a 200 with a length.
	 */
	check_head(&client, "/", "text/html", &ok);
	check_head(&client, "/app.js", "application/javascript", &ok);
	check_head(&client, "/nonexistent.txt", NULL, &ok);
	/*
	 * 11. HEAD stays outside the API contract, deliberately. This is
	 * an assertion about scope: /v1/... is routed by the methods
	 * docs/api/openapi.yaml declares, none of which is HEAD, and if
	 * that ever changes it should change because someone decided to
	 * rather than because a gate was widened.
	 */
	{
		struct cix_response r;

		if (cix_client_request(&client, "HEAD", "/v1/health", NULL, &r) != 0) {
			fprintf(stderr, "FAIL: HEAD /v1/health: transport error\n");
			ok = 0;
		} else {
			if (r.status != 404) {
				fprintf(stderr,
				        "FAIL: HEAD /v1/health: status %d, expected 404 -- the API router is "
				        "keyed on declared methods and declares no HEAD\n",
				        r.status);
				ok = 0;
			}
			/*
			 * And it is a body-less 404. This is the half the
			 * report called "the other half of this being
			 * untested": the refusal paths a HEAD reaches are
			 * ordinary respond_error() sites, and a HEAD that
			 * answers with a JSON body is a protocol violation
			 * however correct its status is.
			 */
			if (r.body_len != 0) {
				fprintf(stderr, "FAIL: HEAD /v1/health returned %zu bytes of body: %s\n",
				        r.body_len, r.body);
				ok = 0;
			}
			if (r.content_length <= 0) {
				fprintf(stderr,
				        "FAIL: HEAD /v1/health declared Content-Length %ld -- a HEAD's headers "
				        "are the ones a GET would carry, so it must describe the error body it "
				        "is not sending\n",
				        r.content_length);
				ok = 0;
			}
			cix_response_free(&r);
		}
	}

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
	printf(ok ? "WEB RESULT: PASS\n" : "WEB RESULT: FAIL\n");
	return ok ? 0 : 1;
}
