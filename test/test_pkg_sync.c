/*
 * Proves Part 2 of the pkg/ redesign (task #767, ADR-0121) end to end:
 * GET/PUT /v1/pkg/repo-config (partial-update semantics, token never
 * echoed back) and POST/GET /v1/pkg/sync (async fetch + merge/additive
 * recipe import). Kept hermetic like test_pkg.c's own fetch tests: a
 * real `python3 -m http.server` stands in for a gitea instance, serving
 * a hand-built tarball at the exact path build_sync_fetch_request()'s
 * own gitea branch computes (/api/v1/repos/<owner>/<repo>/archive/
 * <ref>.tar.gz) -- the real curl subprocess and real tar-extraction
 * code paths run unmocked, only the forge itself is a stand-in.
 */
#include "httpclient.h"
#include "json.h"
#include "persist.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7647
#define PORT_ARG "--port=7647"
#define HTTP_PORT 7648

static char g_data_dir[PATH_MAX];
static char g_pkg_state_dir[PATH_MAX];

static int g_failures;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s\n", msg); \
			g_failures++; \
		} \
	} while (0)

static int run_cmd(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;
	int rc;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	rc = system(cmd);
	return (rc == 0) ? 0 : -1;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/kanxeod", dargv, environ);
		perror("execve build/kanxeod");
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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

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

/* Serves scratch_dir/api/v1/repos/... as a stand-in gitea REST archive
 * endpoint -- plain static file serving matches exactly since
 * build_sync_fetch_request()'s gitea branch computes a fixed path. */
static pid_t start_http_server(const char *dir)
{
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		char port_str[16];
		char *argv[8];

		snprintf(port_str, sizeof(port_str), "%d", HTTP_PORT);
		argv[0] = "python3";
		argv[1] = "-m";
		argv[2] = "http.server";
		argv[3] = port_str;
		argv[4] = "--directory";
		argv[5] = (char *)dir;
		argv[6] = NULL;
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execvp("python3", argv);
		_exit(127);
	}
	return pid;
}

static int stop_http_server(pid_t pid)
{
	int status;

	kill(pid, SIGTERM);
	return waitpid(pid, &status, 0) == pid ? 0 : -1;
}

/* Builds <scratch>/api/v1/repos/testowner/testrepo/archive/master.tar.gz,
 * a real git-archive-shaped tarball (single top-level "testrepo-master/"
 * prefix) containing one recipe at pkg/recipes/synctest/1.0/build.sh. */
static int stage_fixture_archive(const char *scratch_dir)
{
	char stage_dir[PATH_MAX], recipe_dir[PATH_MAX], recipe_path[PATH_MAX];
	char archive_dir[PATH_MAX], archive_path[PATH_MAX];
	FILE *f;

	snprintf(stage_dir, sizeof(stage_dir), "%s/stage/testrepo-master", scratch_dir);
	snprintf(recipe_dir, sizeof(recipe_dir), "%s/pkg/recipes/synctest/1.0", stage_dir);
	if (run_cmd("mkdir -p '%s'", recipe_dir) != 0)
		return -1;

	snprintf(recipe_path, sizeof(recipe_path), "%s/build.sh", recipe_dir);
	f = fopen(recipe_path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "pkg_name=synctest\n");
	fprintf(f, "pkg_version=1.0\n");
	fprintf(f, "pkg_source=file:///nonexistent/synctest-1.0.tar\n");
	fprintf(f, "pkg_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
	fprintf(f, "pkg_depends=\"\"\n");
	fclose(f);

	snprintf(archive_dir, sizeof(archive_dir), "%s/api/v1/repos/testowner/testrepo/archive",
	         scratch_dir);
	if (run_cmd("mkdir -p '%s'", archive_dir) != 0)
		return -1;
	snprintf(archive_path, sizeof(archive_path), "%s/master.tar.gz", archive_dir);

	return run_cmd("tar -C '%s/stage' -czf '%s' testrepo-master", scratch_dir, archive_path);
}

/* Polls GET /v1/pkg/sync until state leaves "running". */
static int poll_sync(const struct kx_client *c, struct kx_response *out)
{
	int i;

	for (i = 0; i < 100; i++) {
		const char *state;

		memset(out, 0, sizeof(*out));
		if (kx_client_request(c, "GET", "/v1/pkg/sync", NULL, out) != 0 || out->status != 200)
			return -1;
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "running") != 0)
			return 0;
		kx_response_free(out);
		usleep(100000);
	}
	return -1;
}

int main(void)
{
	pid_t daemon_pid, http_pid;
	struct kx_client client;
	struct kx_response r;
	char scratch_dir[] = "/tmp/kanxeo_test_pkgsync_XXXXXX";
	char put_body[512];
	char repo_url[128];
	char recipe_check_path[PATH_MAX];
	char *content;
	size_t content_len;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_pkg_state_dir, sizeof(g_pkg_state_dir), "%s/rebuildable/pkg", g_data_dir);

	if (mkdtemp(scratch_dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (stage_fixture_archive(scratch_dir) != 0) {
		fprintf(stderr, "FAIL: could not stage fixture archive\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	http_pid = start_http_server(scratch_dir);
	if (http_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	usleep(300000); /* give http.server a moment to bind before curl hits it */

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		stop_http_server(http_pid);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never became healthy\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		stop_http_server(http_pid);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* --- scenario 1: fresh daemon, no repo configured --- */
	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "GET", "/v1/pkg/repo-config", NULL, &r) == 0 &&
	              r.status == 200,
	      "GET /v1/pkg/repo-config (fresh)");
	if (r.json != NULL) {
		const char *url = json_str_field(r.json, "repo_url");

		CHECK(url != NULL && url[0] == '\0', "fresh repo-config has empty repo_url");
	}
	kx_response_free(&r);

	/* --- scenario 2: POST /v1/pkg/sync with nothing configured -> 400 --- */
	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "POST", "/v1/pkg/sync", NULL, &r) == 0 && r.status == 400,
	      "POST /v1/pkg/sync with no repo configured must 400");
	kx_response_free(&r);

	/* --- scenario 3: PUT /v1/pkg/repo-config, partial update semantics --- */
	snprintf(repo_url, sizeof(repo_url), "http://127.0.0.1:%d/testowner/testrepo", HTTP_PORT);
	snprintf(put_body, sizeof(put_body),
	         "{\"repo_url\":\"%s\",\"repo_kind\":\"gitea\",\"ref\":\"master\","
	         "\"auth_token\":\"scratch-token\",\"sync_interval_seconds\":0}",
	         repo_url);
	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "PUT", "/v1/pkg/repo-config", put_body, &r) == 0 &&
	              r.status == 200,
	      "PUT /v1/pkg/repo-config (initial)");
	if (r.json != NULL) {
		const char *got_url = json_str_field(r.json, "repo_url");
		const char *got_kind = json_str_field(r.json, "repo_kind");
		const struct json_value *token_set = json_object_get(r.json, "auth_token_set");

		CHECK(got_url != NULL && strcmp(got_url, repo_url) == 0, "repo_url echoed back");
		CHECK(got_kind != NULL && strcmp(got_kind, "gitea") == 0, "repo_kind echoed back");
		CHECK(token_set != NULL && token_set->type == JSON_BOOL && token_set->u.boolean,
		      "auth_token_set is true after setting a token");
		CHECK(json_object_get(r.json, "auth_token") == NULL,
		      "the raw auth_token value is never echoed back");
	}
	kx_response_free(&r);

	/* Partial update: only bump sync_interval_seconds, url/kind/ref/token
	 * must be left exactly as they were. */
	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "PUT", "/v1/pkg/repo-config",
	                         "{\"sync_interval_seconds\":3600}", &r) == 0 &&
	              r.status == 200,
	      "PUT /v1/pkg/repo-config (partial: interval only)");
	if (r.json != NULL) {
		const char *got_url = json_str_field(r.json, "repo_url");
		long interval = (long)json_as_number(json_object_get(r.json, "sync_interval_seconds"));
		const struct json_value *token_set = json_object_get(r.json, "auth_token_set");

		CHECK(got_url != NULL && strcmp(got_url, repo_url) == 0,
		      "repo_url unchanged by a partial update omitting it");
		CHECK(interval == 3600, "sync_interval_seconds applied");
		CHECK(token_set != NULL && token_set->type == JSON_BOOL && token_set->u.boolean,
		      "auth_token_set still true (token untouched by partial update)");
	}
	kx_response_free(&r);

	/* --clear-token equivalent: an explicit empty string clears it. */
	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "PUT", "/v1/pkg/repo-config", "{\"auth_token\":\"\"}", &r) ==
	              0 &&
	              r.status == 200,
	      "PUT /v1/pkg/repo-config (clear token)");
	if (r.json != NULL) {
		const struct json_value *token_set = json_object_get(r.json, "auth_token_set");

		CHECK(token_set != NULL && token_set->type == JSON_BOOL && !token_set->u.boolean,
		      "auth_token_set false after explicit clear");
	}
	kx_response_free(&r);

	/* Set the interval back to 0 (disabled) so no periodic timer races
	 * the explicit POST /v1/pkg/sync calls below. */
	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "PUT", "/v1/pkg/repo-config",
	                         "{\"sync_interval_seconds\":0}", &r) == 0 && r.status == 200,
	      "PUT /v1/pkg/repo-config (disable periodic sync)");
	kx_response_free(&r);

	/* --- scenario 4: real fetch + merge against the stand-in http server --- */
	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "POST", "/v1/pkg/sync", NULL, &r) == 0 && r.status == 202,
	      "POST /v1/pkg/sync accepted (202)");
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(poll_sync(&client, &r) == 0, "sync leaves running state before timeout");
	if (r.json != NULL) {
		const char *state = json_str_field(r.json, "state");
		long added = (long)json_as_number(json_object_get(r.json, "added"));
		long skipped = (long)json_as_number(json_object_get(r.json, "skipped"));

		CHECK(state != NULL && strcmp(state, "success") == 0, "first sync succeeds");
		CHECK(added == 1, "first sync adds exactly the one fixture recipe");
		CHECK(skipped == 0, "first sync skips nothing (nothing pre-existing)");
	}
	kx_response_free(&r);

	snprintf(recipe_check_path, sizeof(recipe_check_path), "%s/recipes/synctest/1.0/build.sh",
	         g_pkg_state_dir);
	CHECK(persist_read_file(recipe_check_path, &content, &content_len) == 0 && content != NULL,
	      "synced recipe actually landed on disk");
	if (content != NULL) {
		CHECK(strstr(content, "pkg_name=synctest") != NULL,
		      "synced recipe content matches the fixture");
		free(content);
	}

	/* --- scenario 5: re-sync is additive/merge, not destructive -- the
	 * same recipe is skipped as a duplicate, not re-added or rejected. */
	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "POST", "/v1/pkg/sync", NULL, &r) == 0 && r.status == 202,
	      "second POST /v1/pkg/sync accepted (202)");
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(poll_sync(&client, &r) == 0, "second sync leaves running state before timeout");
	if (r.json != NULL) {
		const char *state = json_str_field(r.json, "state");
		long added = (long)json_as_number(json_object_get(r.json, "added"));
		long skipped = (long)json_as_number(json_object_get(r.json, "skipped"));

		CHECK(state != NULL && strcmp(state, "success") == 0, "second sync succeeds");
		CHECK(added == 0, "second sync adds nothing new");
		CHECK(skipped == 1, "second sync skips the already-present recipe (merge semantics)");
	}
	kx_response_free(&r);

	/* --- scenario 5b: parse_repo_url() ignores anything past the repo
	 * segment (ADR-0133) -- an operator pasting a real forge browse URL
	 * (e.g. gitea's own "<repo>/src/branch/<ref>/<path>", exactly what
	 * a browser address bar shows while looking at the repo) must still
	 * resolve to the same owner/repo, not silently mis-parse into a
	 * garbage owner and a bogus fetch URL (confirmed live: this exact
	 * shape 404'd with no clue the URL itself was the problem, before
	 * this fix). Re-pointed at the identical stand-in repo via a URL
	 * with a browse-style suffix appended -- sync must still find and
	 * correctly skip the same already-present fixture recipe. */
	memset(&r, 0, sizeof(r));
	snprintf(put_body, sizeof(put_body),
	         "{\"repo_url\":\"http://127.0.0.1:%d/testowner/testrepo/src/branch/master/pkg/recipes\"}",
	         HTTP_PORT);
	CHECK(kx_client_request(&client, "PUT", "/v1/pkg/repo-config", put_body, &r) == 0 &&
	              r.status == 200,
	      "PUT /v1/pkg/repo-config (browse-URL-style suffix)");
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "POST", "/v1/pkg/sync", NULL, &r) == 0 && r.status == 202,
	      "POST /v1/pkg/sync accepted (browse-URL-style suffix)");
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(poll_sync(&client, &r) == 0, "browse-URL-suffix sync leaves running state before timeout");
	if (r.json != NULL) {
		const char *state = json_str_field(r.json, "state");
		long added = (long)json_as_number(json_object_get(r.json, "added"));
		long skipped = (long)json_as_number(json_object_get(r.json, "skipped"));

		CHECK(state != NULL && strcmp(state, "success") == 0,
		      "sync with a browse-URL-style suffix still succeeds (owner/repo correctly parsed)");
		CHECK(added == 0 && skipped == 1,
		      "browse-URL-suffix sync resolves to the SAME repo as the bare owner/repo URL did");
	}
	kx_response_free(&r);

	/* --- scenario 6: a real fetch failure (unknown repo path -> curl
	 * exit) reports state=failed with a real error, not a silent hang. */
	memset(&r, 0, sizeof(r));
	snprintf(put_body, sizeof(put_body), "{\"repo_url\":\"http://127.0.0.1:%d/testowner/nosuch\"}",
	         HTTP_PORT);
	CHECK(kx_client_request(&client, "PUT", "/v1/pkg/repo-config", put_body, &r) == 0 &&
	              r.status == 200,
	      "PUT /v1/pkg/repo-config (repoint at a nonexistent repo)");
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(kx_client_request(&client, "POST", "/v1/pkg/sync", NULL, &r) == 0 && r.status == 202,
	      "POST /v1/pkg/sync accepted even though the target 404s (async, fails later)");
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(poll_sync(&client, &r) == 0, "failing sync leaves running state before timeout");
	if (r.json != NULL) {
		const char *state = json_str_field(r.json, "state");
		const char *error = json_str_field(r.json, "error");

		CHECK(state != NULL && strcmp(state, "failed") == 0, "sync against a 404 reports failed");
		CHECK(error != NULL && error[0] != '\0', "a failed sync carries a real error message");
	}
	kx_response_free(&r);

	stop_daemon(daemon_pid);
	stop_http_server(http_pid);
	test_data_dir_cleanup(g_data_dir);
	run_cmd("rm -rf '%s'", scratch_dir);

	if (g_failures > 0) {
		fprintf(stderr, "test_pkg_sync: %d failure(s)\n", g_failures);
		return 1;
	}
	fprintf(stderr, "test_pkg_sync: OK\n");
	return 0;
}
