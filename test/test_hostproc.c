/*
 * Logging/web-UI epic Part 6 end-to-end test (ADR-0131): proves the
 * host process list + kill subsystem (daemon/src/hostproc.c) over
 * real HTTP against a real cixd subprocess --
 *   - GET /v1/system/processes: a real /proc scan lists real processes
 *     (this test's own daemon subprocess among them), and a real
 *     running container's own process is correlated to it by name via
 *     hostproc.c's own ppid-chain walk.
 *   - DELETE /v1/system/processes/{pid}: 400 for pid 1, 400 for this
 *     daemon's own real pid (both refused outright), 404 for a
 *     plainly nonexistent pid, 400 for a non-numeric pid, and a real
 *     204 that actually kills a real, disposable process (forked by
 *     this test itself, never a container -- proving the kill
 *     mechanism works without needing to kill anything this test
 *     still needs afterward).
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7661
#define PORT_ARG "--port=7661"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static long json_num_field(const struct json_value *obj, const char *key)
{
	return (long)json_as_number(json_object_get(obj, key));
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

int main(void)
{
	pid_t daemon_pid;
	char *dargv[5];
	char data_dir_arg[PATH_MAX + 11];
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
	char pid_path[64];

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/hostproctest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/hostproctest", g_data_dir);
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
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	daemon_pid = fork();
	if (daemon_pid < 0) {
		perror("fork");
		test_data_dir_cleanup(g_data_dir);
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
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. GET /v1/system/processes: a real scan, this daemon's own real
	 * pid (execve() doesn't change it) is among the results. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/processes", NULL, &r) != 0 ||
	    r.status != 200 || r.json == NULL || r.json->type != JSON_ARRAY) {
		fprintf(stderr, "FAIL: GET /v1/system/processes, status=%d\n", r.status);
		ok = 0;
	} else {
		size_t i;
		int found = 0;

		for (i = 0; i < r.json->u.array.count; i++) {
			if (json_num_field(r.json->u.array.items[i], "pid") == (long)daemon_pid) {
				found = 1;
				if (!str_eq(json_str_field(r.json->u.array.items[i], "comm"), "cixd")) {
					fprintf(stderr, "FAIL: daemon's own process has comm=%s, expected cixd\n",
					        json_str_field(r.json->u.array.items[i], "comm"));
					ok = 0;
				}
			}
		}
		if (!found) {
			fprintf(stderr, "FAIL: daemon's own real pid %d missing from GET /v1/system/processes\n",
			        (int)daemon_pid);
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 2. A real, running container -- its own process must be
	 * correlated to it by name. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"hpc1\",\"image\":\"hostproctest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"300\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST hpc1, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	long container_pid = 0;

	if (cix_client_request(&client, "GET", "/v1/containers/hpc1", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET hpc1, status=%d\n", r.status);
		ok = 0;
	} else {
		container_pid = json_num_field(r.json, "pid");
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/processes", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/system/processes (2), status=%d\n", r.status);
		ok = 0;
	} else {
		size_t i;
		int found = 0;

		for (i = 0; i < r.json->u.array.count; i++) {
			if (json_num_field(r.json->u.array.items[i], "pid") == container_pid) {
				found = 1;
				if (!str_eq(json_str_field(r.json->u.array.items[i], "container"), "hpc1")) {
					fprintf(stderr,
					        "FAIL: hpc1's own process has container=%s, expected hpc1\n",
					        json_str_field(r.json->u.array.items[i], "container")
					            ? json_str_field(r.json->u.array.items[i], "container")
					            : "(empty)");
					ok = 0;
				}
			}
		}
		if (!found) {
			fprintf(stderr, "FAIL: hpc1's own pid %ld missing from GET /v1/system/processes\n",
			        container_pid);
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 3. Kill validation: pid 1 and this daemon's own pid are both
	 * refused (400); a plainly nonexistent pid is 404; a non-numeric
	 * pid is 400. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/system/processes/1", NULL, &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: DELETE pid 1 expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	snprintf(pid_path, sizeof(pid_path), "/v1/system/processes/%d", (int)daemon_pid);
	if (cix_client_request(&client, "DELETE", pid_path, NULL, &r) != 0 || r.status != 400) {
		fprintf(stderr, "FAIL: DELETE daemon's own pid expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/system/processes/999999999", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: DELETE nonexistent pid expected 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/system/processes/notapid", NULL, &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: DELETE non-numeric pid expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 4. A real, disposable process (forked by this test, never a
	 * container) really dies when killed via the API. */
	{
		pid_t victim = fork();

		if (victim == 0) {
			pause();
			_exit(0);
		}
		usleep(100000); /* let it actually reach pause() */

		memset(&r, 0, sizeof(r));
		snprintf(pid_path, sizeof(pid_path), "/v1/system/processes/%d", (int)victim);
		if (cix_client_request(&client, "DELETE", pid_path, NULL, &r) != 0 || r.status != 204) {
			fprintf(stderr, "FAIL: DELETE real victim pid expected 204, got %d\n", r.status);
			ok = 0;
			kill(victim, SIGKILL);
		}
		cix_response_free(&r);

		{
			int status;
			pid_t w = waitpid(victim, &status, 0);

			if (w != victim || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
				fprintf(stderr, "FAIL: victim process did not actually die from SIGKILL\n");
				ok = 0;
			}
		}
	}

	cix_client_request(&client, "DELETE", "/v1/containers/hpc1", NULL, &r);
	cix_response_free(&r);

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
	printf(ok ? "HOSTPROC RESULT: PASS\n" : "HOSTPROC RESULT: FAIL\n");
	return ok ? 0 : 1;
}
