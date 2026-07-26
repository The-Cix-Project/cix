/*
 * Phase 5 end-to-end test: proves kanxeod's static-file serving
 * (daemon/src/staticfile.c) works over real HTTP -- correct status/
 * content-type per asset, 404 for missing files, path traversal
 * rejected, and that adding this didn't regress /v1/... routing.
 * Whether the dashboard actually renders and behaves correctly in a
 * real browser is a separate, manual check (docs/adr/0010) -- not
 * something this test (or any tool in this project) can verify.
 */
#include "httpclient.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7623
#define PORT_ARG "--port=7623"

static int wait_for_daemon(const struct kx_client *c, int max_attempts)
{
	int i;
	struct kx_response r;

	for (i = 0; i < max_attempts; i++) {
		if (kx_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			kx_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static void check(const struct kx_client *c, const char *path, int want_status,
                   const char *want_content_type, const char *want_body_substr,
                   const char *forbid_body_substr, int *ok)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
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

	kx_response_free(&r);
}

int main(void)
{
	pid_t daemon_pid;
	char *dargv[3];
	struct kx_client client;
	int ok = 1;

	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
	dargv[2] = NULL;

	daemon_pid = fork();
	if (daemon_pid < 0) {
		perror("fork");
		return 1;
	}
	if (daemon_pid == 0) {
		execve("build/kanxeod", dargv, environ);
		perror("execve build/kanxeod");
		_exit(127);
	}

	kx_client_init(&client, "127.0.0.1", TEST_PORT);

	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	/* 1-2. index and its title marker */
	check(&client, "/", 200, "text/html", "Kanxeo", NULL, &ok);
	/* 3. app.js */
	check(&client, "/app.js", 200, "application/javascript", "docs/api/openapi.yaml", NULL, &ok);
	/* 4. style.css */
	check(&client, "/style.css", 200, "text/css", NULL, NULL, &ok);
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

	printf(ok ? "WEB RESULT: PASS\n" : "WEB RESULT: FAIL\n");
	return ok ? 0 : 1;
}
