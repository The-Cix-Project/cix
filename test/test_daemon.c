/*
 * Phase 3 end-to-end test: proves the REST daemon actually implements
 * docs/api/openapi.yaml over real HTTP, not just that its internal
 * functions work. Stages one minimal test image, forks and execve's
 * the built daemon on a test port, then drives it via the shared
 * httpclient.c (the same client library kanxeoctl uses -- see
 * ADR-0005/Phase 4 in docs/roadmap/ROADMAP.md), so there is one implementation
 * of "how to talk to the API," not a test-only copy of it.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7621

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

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

static long json_int_field(const struct json_value *obj, const char *key)
{
	return (long)json_as_number(json_object_get(obj, key));
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static int poll_until_exited(const struct kx_client *c, const char *name, int max_attempts,
                              struct kx_response *out)
{
	int i;
	char path[128];

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < max_attempts; i++) {
		kx_response_free(out);
		if (kx_client_request(c, "GET", path, NULL, out) != 0)
			return -1;
		if (out->status == 200 && str_eq(json_str_field(out->json, "status"), "exited"))
			return 0;
		usleep(100000);
	}
	return -1;
}

int main(void)
{
	pid_t daemon_pid;
	int ok = 1;
	struct kx_response r;
	struct kx_client client;
	char *dargv[4];
	char data_dir_arg[PATH_MAX + 11];

	memset(&r, 0, sizeof(r));
	kx_client_init(&client, "127.0.0.1", TEST_PORT);

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	/* ADR-0107/0108: a manifest.json is required for POST /v1/containers
	 * to resolve this image's own current version -- see
	 * test_image_fixture_write_manifest()'s own header comment. */
	snprintf(g_image_root, sizeof(g_image_root), "%s/images/test/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/images/test", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}

	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/kanxeod";
	dargv[1] = "--port=7621";
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	daemon_pid = fork();
	if (daemon_pid < 0) {
		perror("fork");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (daemon_pid == 0) {
		execve("build/kanxeod", dargv, environ);
		perror("execve build/kanxeod");
		_exit(127);
	}

	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. health */
	if (kx_client_request(&client, "GET", "/v1/health", NULL, &r) != 0 || r.status != 200 ||
	    !str_eq(json_str_field(r.json, "status"), "ok")) {
		fprintf(stderr, "FAIL: GET /v1/health\n");
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. create c1, exits quickly with code 5 */
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c1\",\"image\":\"test\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"5\"]}",
	                       &r) != 0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "status"), "running")) {
		fprintf(stderr, "FAIL: POST /v1/containers (c1), status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. list shows it */
	if (kx_client_request(&client, "GET", "/v1/containers", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/containers\n");
		ok = 0;
	} else {
		const struct json_value *containers = json_object_get(r.json, "containers");
		int found = 0;
		size_t i;

		if (containers != NULL && containers->type == JSON_ARRAY) {
			for (i = 0; i < containers->u.array.count; i++) {
				if (str_eq(json_str_field(containers->u.array.items[i], "name"), "c1"))
					found = 1;
			}
		}
		if (!found) {
			fprintf(stderr, "FAIL: c1 missing from GET /v1/containers\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* 4. poll until c1 exits, check exit_status */
	if (poll_until_exited(&client, "c1", 50, &r) != 0) {
		fprintf(stderr, "FAIL: c1 never reported exited\n");
		ok = 0;
	} else if (json_int_field(r.json, "exit_status") != 5) {
		fprintf(stderr, "FAIL: c1 exit_status expected 5, got %ld\n",
		        json_int_field(r.json, "exit_status"));
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. create c2, sleeps 30s -- then delete it while running */
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c2\",\"image\":\"test\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/containers (c2), status=%d\n", r.status);
		ok = 0;
	}
	{
		long c2_pid = json_int_field(r.json, "pid");
		char proc_path[64];
		struct stat st;

		kx_response_free(&r);

		if (kx_client_request(&client, "DELETE", "/v1/containers/c2", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE /v1/containers/c2, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		snprintf(proc_path, sizeof(proc_path), "/proc/%ld", c2_pid);
		if (stat(proc_path, &st) == 0) {
			fprintf(stderr, "FAIL: c2's process %ld still exists after DELETE\n", c2_pid);
			ok = 0;
		}

		/*
		 * task #738: DELETE must remove c2's own on-disk upper/work/
		 * merged directories, not just the process and registry entry --
		 * deleting a container that was genuinely still RUNNING (this
		 * exact case) is the one that exercises the real hazard the fix
		 * has to get right: unmounting the still-live overlay mount
		 * before recursively removing the directory tree underneath it,
		 * not after.
		 */
		{
			char container_base[PATH_MAX];

			snprintf(container_base, sizeof(container_base), "%s/containers/c2", g_data_dir);
			if (stat(container_base, &st) == 0) {
				fprintf(stderr, "FAIL: c2's own directory %s still exists after DELETE\n",
				        container_base);
				ok = 0;
			} else if (errno != ENOENT) {
				fprintf(stderr, "FAIL: stat(%s) after DELETE: %s\n", container_base,
				        strerror(errno));
				ok = 0;
			}
		}
	}

	/* 6. deleted container is gone */
	if (kx_client_request(&client, "GET", "/v1/containers/c2", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: GET /v1/containers/c2 after delete, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 7. duplicate name -> 409 */
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c1\",\"image\":\"test\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"1\"]}",
	                       &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate name expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 8. missing image -> 400 */
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c3\",\"cmd\":[\"/bin/daemon_child\"]}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: missing image expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 9. clean shutdown */
	kill(daemon_pid, SIGTERM);
	{
		int status;
		pid_t w = waitpid(daemon_pid, &status, 0);

		if (w != daemon_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
			ok = 0;
		}
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "DAEMON RESULT: PASS\n" : "DAEMON RESULT: FAIL\n");
	return ok ? 0 : 1;
}
