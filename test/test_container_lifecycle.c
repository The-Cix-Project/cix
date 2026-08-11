/*
 * Phase A (ADR-0045) end-to-end test: proves the container lifecycle
 * completeness fixes over real HTTP against a real kanxeod subprocess
 * -- POST .../start actually recovers a stopped-but-defined container
 * without a daemon restart, POST .../pause and .../unpause are a real
 * cgroup v2 freeze (checked against the real cgroup.events file, not
 * just a status string), the freeze-before-kill fix in registry_remove()
 * doesn't hang on DELETE of a paused container, GET now lists a
 * stopped container instead of it vanishing, and containerdef_
 * autostart_all() no longer leaves a revived "always" container's
 * stopped flag permanently stale after a real daemon restart.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7635
#define PORT_ARG "--port=7635"

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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static long json_num_field(const struct json_value *obj, const char *key)
{
	return (long)json_as_number(json_object_get(obj, key));
}

static int json_bool_field(const struct json_value *obj, const char *key)
{
	const struct json_value *v = json_object_get(obj, key);

	return v != NULL && v->type == JSON_BOOL && v->u.boolean;
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
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

static long fetch_pid(const struct kx_client *c, const char *name)
{
	char path[128];
	struct kx_response r;
	long pid = -1;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (kx_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200)
		pid = json_num_field(r.json, "pid");
	kx_response_free(&r);
	return pid;
}

/* Reads /sys/fs/cgroup/<name>/<file> verbatim (trailing newline
 * stripped) -- the real, kernel-authoritative value cgroup_create()
 * wrote via struct cgroup_limits, not just what POST echoed back
 * (this daemon doesn't echo cpu_max/cpuset_cpus/memory_max/pids_max
 * back in any response). Used for both cpu.max (Part 1) and
 * cpuset.cpus (Part 2). */
static int read_cgroup_value(const char *name, const char *file, char *out, size_t out_size)
{
	char path[256];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/%s", name, file);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	if (fgets(out, (int)out_size, f) == NULL) {
		fclose(f);
		return -1;
	}
	fclose(f);
	n = strlen(out);
	if (n > 0 && out[n - 1] == '\n')
		out[n - 1] = '\0';
	return 0;
}

/* Reads /sys/fs/cgroup/<name>/cgroup.events and returns 1 if "frozen 1"
 * is present, 0 if "frozen 0", -1 on any read failure -- the real,
 * kernel-authoritative freeze state, not just what the REST API claims. */
static int cgroup_is_frozen(const char *name)
{
	char path[256];
	FILE *f;
	char line[64];
	int result = -1;

	snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/cgroup.events", name);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "frozen ", 7) == 0) {
			result = atoi(line + 7);
			break;
		}
	}
	fclose(f);
	return result;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/images/lifecycletest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/images/lifecycletest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}

	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (test_image_fixture_build(g_image_root, "build/output_child", "output_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. Create a long-lived "always" container. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"lc1\",\"image\":\"lifecycletest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST lc1, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. stop -- must still be visible via GET (not 404), status
	 * "stopped", not just "gone." This is the actual bug reported. */
	{
		long pid1 = fetch_pid(&client, "lc1");

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/stop", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: POST lc1/stop, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/containers/lc1", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "status"), "stopped") ||
		    !json_bool_field(r.json, "stopped")) {
			fprintf(stderr, "FAIL: GET lc1 after stop should be 200/stopped, got status=%d body=%s\n",
			        r.status, r.status == 200 ? json_str_field(r.json, "status") : "?");
			ok = 0;
		}
		kx_response_free(&r);

		/* Also confirm it shows up in the plain list, not just single-GET. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/containers", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/containers, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *list = json_object_get(r.json, "containers");
			size_t i;
			int found = 0;

			for (i = 0; list != NULL && i < list->u.array.count; i++) {
				if (str_eq(json_str_field(list->u.array.items[i], "name"), "lc1")) {
					found = 1;
					break;
				}
			}
			if (!found) {
				fprintf(stderr, "FAIL: lc1 missing from GET /v1/containers after stop\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		/* 3. start -- brings it back, no daemon restart, fresh pid. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/start", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "status"), "running") ||
		    json_bool_field(r.json, "stopped")) {
			fprintf(stderr, "FAIL: POST lc1/start, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (fetch_pid(&client, "lc1") == pid1 || fetch_pid(&client, "lc1") < 0) {
			fprintf(stderr, "FAIL: lc1 should have a fresh pid after start\n");
			ok = 0;
		}

		/* Idempotent: starting an already-live container is 200, not an error. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/start", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: idempotent re-start, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* start on a name with no definition at all is 404. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/never-existed/start", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: start on unknown name should be 404, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 4. pause -- real cgroup freeze, checked against the kernel's own
	 * cgroup.events, not just the REST status string. */
	{
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/pause", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "status"), "paused") ||
		    !json_bool_field(r.json, "paused")) {
			fprintf(stderr, "FAIL: POST lc1/pause, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (cgroup_is_frozen("lc1") != 1) {
			fprintf(stderr, "FAIL: /sys/fs/cgroup/lc1/cgroup.events does not report frozen 1 after pause\n");
			ok = 0;
		}

		/* Double-pause is 409, not a silent no-op. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/pause", NULL, &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: double-pause should be 409, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* 5. unpause -- real thaw. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/unpause", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "status"), "running") ||
		    json_bool_field(r.json, "paused")) {
			fprintf(stderr, "FAIL: POST lc1/unpause, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (cgroup_is_frozen("lc1") != 0) {
			fprintf(stderr, "FAIL: /sys/fs/cgroup/lc1/cgroup.events does not report frozen 0 after unpause\n");
			ok = 0;
		}

		/* Double-unpause is 409. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/unpause", NULL, &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: double-unpause should be 409, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* pause/unpause on a stopped-but-defined (not live) container is 404. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/stop", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: POST lc1/stop (pre-pause-404-check), status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/pause", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: pause on a stopped container should be 404, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/start", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: POST lc1/start (post-pause-404-check), status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 6. Freeze-before-kill: DELETE on a paused container must not hang. */
	{
		time_t t0, t1;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc1/pause", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: pause before delete, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		t0 = time(NULL);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/containers/lc1", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE paused lc1, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
		t1 = time(NULL);

		if (t1 - t0 > 5) {
			fprintf(stderr, "FAIL: DELETE of a paused container took %ld s -- looks hung\n",
			        (long)(t1 - t0));
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/containers/lc1", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: lc1 should be fully gone after DELETE, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 7. Autostart stale-flag fix: an "always" container that was
	 * manually stopped, then revived by a real daemon restart, must
	 * come back with stopped == false -- not permanently stuck true
	 * (which would silently defeat its own crash-restart policy from
	 * then on -- see ADR-0045). */
	{
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"lc2\",\"image\":\"lifecycletest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
		                       "\"restart\":\"always\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST lc2, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers/lc2/stop", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: POST lc2/stop, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (stop_daemon(daemon_pid) != 0) {
			fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
			ok = 0;
		}
		daemon_pid = start_daemon();
		if (daemon_pid < 0 || wait_for_daemon(&client, 50) != 0) {
			fprintf(stderr, "FAIL: daemon did not come back up after restart\n");
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/containers/lc2", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "status"), "running") ||
		    json_bool_field(r.json, "stopped")) {
			fprintf(stderr,
			        "FAIL: lc2 should be running with stopped==false after daemon restart "
			        "(autostart stale-flag fix), got status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 8. cpu_max (Part 1 of the bare-metal-readiness plan): a real
	 * cpu.max value round-trips into the container's own real cgroup,
	 * not just accepted and silently dropped. cgroup_create() (src/
	 * cgroup.c) already had this mechanism -- this proves the daemon's
	 * own JSON-to-spec.cg.cpu_max wiring (create_container_from_body(),
	 * daemon/src/main.c) actually reaches it. */
	{
		char cpu_max[64];

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"lc3\",\"image\":\"lifecycletest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"5\",\"0\"],"
		                       "\"cpu_max\":\"50000 100000\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST lc3 with cpu_max, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (read_cgroup_value("lc3", "cpu.max", cpu_max, sizeof(cpu_max)) != 0) {
			fprintf(stderr, "FAIL: could not read /sys/fs/cgroup/lc3/cpu.max\n");
			ok = 0;
		} else if (strcmp(cpu_max, "50000 100000") != 0) {
			fprintf(stderr, "FAIL: /sys/fs/cgroup/lc3/cpu.max = \"%s\", expected \"50000 100000\"\n",
			        cpu_max);
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/containers/lc3", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE lc3, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 9. cpuset_cpus (Part 2 of the bare-metal-readiness plan): same
	 * shape as step 8's cpu_max check -- a real cpuset.cpus value
	 * round-trips into the container's own real cgroup. Uses "0" only
	 * (not a range) so this passes on a single-vCPU test host too. */
	{
		char cpuset_cpus[64];

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"lc4\",\"image\":\"lifecycletest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"5\",\"0\"],"
		                       "\"cpuset_cpus\":\"0\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST lc4 with cpuset_cpus, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (read_cgroup_value("lc4", "cpuset.cpus", cpuset_cpus, sizeof(cpuset_cpus)) != 0) {
			fprintf(stderr, "FAIL: could not read /sys/fs/cgroup/lc4/cpuset.cpus\n");
			ok = 0;
		} else if (strcmp(cpuset_cpus, "0") != 0) {
			fprintf(stderr,
			        "FAIL: /sys/fs/cgroup/lc4/cpuset.cpus = \"%s\", expected \"0\"\n",
			        cpuset_cpus);
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/containers/lc4", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE lc4, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 10. capture_output: a container created with "capture_output":true
	 * has its real stdout/stderr captured into the registry entry's own
	 * "captured_output" field, readable back via GET even after the
	 * process has exited -- the feature that makes an otherwise-silent
	 * crash-loop (e.g. sshd -D -e exiting 1 on a config problem)
	 * diagnosable without a working console/exec path. A sibling
	 * container created WITHOUT the option must report
	 * "captured_output":null, not an empty string -- the two are
	 * deliberately distinguishable (opted out vs. captured-but-empty). */
	{
		int attempt;
		char captured_buf[4096];
		int captured_found = 0;

		captured_buf[0] = '\0';

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"lc5\",\"image\":\"lifecycletest\","
		                       "\"cmd\":[\"/bin/output_child\"],"
		                       "\"capture_output\":true}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST lc5 with capture_output, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* output_child exits almost immediately -- poll briefly for the
		 * pipe's EOF to be drained into captured_output rather than
		 * racing it. The matched string is copied out into a local
		 * buffer BEFORE kx_response_free(&r) -- captured pointed into
		 * r.json's own tree, which that free() invalidates, so holding
		 * onto the struct json_value* itself across the free (as an
		 * earlier version of this test did) is a real use-after-free,
		 * not just untidy. */
		for (attempt = 0; attempt < 50; attempt++) {
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "GET", "/v1/containers/lc5", NULL, &r) == 0 &&
			    r.status == 200) {
				const struct json_value *captured = json_object_get(r.json, "captured_output");

				if (captured != NULL && captured->type == JSON_STRING &&
				    strstr(captured->u.string, "capture-test-stdout-line") != NULL) {
					snprintf(captured_buf, sizeof(captured_buf), "%s", captured->u.string);
					captured_found = 1;
					kx_response_free(&r);
					break;
				}
			}
			kx_response_free(&r);
			usleep(100000);
		}

		if (!captured_found || strstr(captured_buf, "capture-test-stdout-line") == NULL ||
		    strstr(captured_buf, "capture-test-stderr-line") == NULL) {
			fprintf(stderr,
			        "FAIL: lc5's captured_output did not contain both expected lines "
			        "after %d attempts (got: \"%s\")\n",
			        attempt, captured_buf);
			ok = 0;
		}
		/* task #760: output_child.c's stdout line carries a real ANSI
		 * color escape (raw ESC 0x1b) -- jw_escaped_string() writes it
		 * as a \u00XX escape to stay valid JSON, so this only round-trips
		 * correctly if json_parse() (used by kx_client_request() just
		 * above, the same shared client every CLI command goes
		 * through) actually decodes \uXXXX back into a real byte
		 * rather than failing the whole parse. */
		if (captured_found && strstr(captured_buf, "\x1b[31mcolor-marker\x1b[0m") == NULL) {
			fprintf(stderr,
			        "FAIL: lc5's captured_output lost its raw ANSI escape byte "
			        "across the JSON round trip (got: \"%s\")\n",
			        captured_buf);
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/containers/lc5", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE lc5, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* Sibling without capture_output: captured_output must be JSON null. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"lc6\",\"image\":\"lifecycletest\","
		                       "\"cmd\":[\"/bin/output_child\"]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST lc6 without capture_output, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/containers/lc6", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET lc6, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *v = json_object_get(r.json, "captured_output");

			if (v == NULL || v->type != JSON_NULL) {
				fprintf(stderr,
				        "FAIL: lc6's captured_output should be JSON null "
				        "(capture_output not requested)\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		/*
		 * lc6 never asked for capture_output (confirmed null just
		 * above) -- but transparent container-log capture (Part 1 of
		 * the logging/UI epic, ADR-0126) is unconditional, so its own
		 * real stdout/stderr must still be independently discoverable
		 * via GET /v1/system/logs?source=container, tagged with its
		 * own container name. Polled briefly: the same async
		 * container-exits-almost-immediately race the capture_output
		 * check above already has to poll for.
		 */
		{
			int seen = 0;
			int i;

			for (i = 0; i < 30 && !seen; i++) {
				memset(&r, 0, sizeof(r));
				if (kx_client_request(&client, "GET",
				                       "/v1/system/logs?source=container&container=lc6", NULL,
				                       &r) == 0 &&
				    r.status == 200 && r.json != NULL && r.json->type == JSON_ARRAY) {
					size_t j;

					for (j = 0; j < r.json->u.array.count; j++) {
						const struct json_value *e = r.json->u.array.items[j];
						const struct json_value *msg = json_object_get(e, "msg");
						const struct json_value *cont = json_object_get(e, "container");

						if (msg != NULL && msg->type == JSON_STRING &&
						    strstr(msg->u.string, "capture-test-stdout-line") != NULL &&
						    cont != NULL && cont->type == JSON_STRING &&
						    strcmp(cont->u.string, "lc6") == 0) {
							seen = 1;
							break;
						}
					}
				}
				kx_response_free(&r);
				if (!seen)
					usleep(100000);
			}
			if (!seen) {
				fprintf(stderr,
				        "FAIL: lc6's real stdout never showed up in "
				        "GET /v1/system/logs?source=container&container=lc6 "
				        "(transparent capture should be unconditional)\n");
				ok = 0;
			}
		}

		/* A container-name filter for an unrelated name must exclude it. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET",
		                       "/v1/system/logs?source=container&container=no-such-container", NULL,
		                       &r) != 0 ||
		    r.status != 200 || r.json == NULL || r.json->type != JSON_ARRAY ||
		    r.json->u.array.count != 0) {
			fprintf(stderr,
			        "FAIL: container filter for an unrelated name should return zero entries\n");
			ok = 0;
		}
		kx_response_free(&r);

		/* A malformed regex is a 400, not a silent empty match. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/system/logs?regex=%5B", NULL, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: malformed regex filter expected 400, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/containers/lc6", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE lc6, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);

	if (ok)
		printf("test_container_lifecycle: PASS\n");
	return ok ? 0 : 1;
}
