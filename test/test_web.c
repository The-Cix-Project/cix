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
