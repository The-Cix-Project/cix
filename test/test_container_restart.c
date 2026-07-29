/*
 * Phase 13 part 1 end-to-end test: proves persisted, auto-restarting
 * containers ("restart": "always"/"depends_on" on POST/DELETE
 * /v1/containers -- daemon/src/containerdef.c + the create_container_
 * from_body()/containerdef_autostart_all()/timerfd crash-restart
 * wiring in daemon/src/main.c) over real HTTP, with a live daemon
 * process actually restarted (not just its in-memory state reset) to
 * prove persistence genuinely survives that, not just an in-process
 * reload.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7630
#define PORT_ARG "--port=7630"
#define IMAGE_ROOT "/var/lib/kanxeo/images/restarttest/rootfs"
#define CONTAINER_DEFS_PATH "/var/lib/kanxeo/container_defs.json"

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

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static long json_num_field(const struct json_value *obj, const char *key)
{
	return (long)json_as_number(json_object_get(obj, key));
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[3];

	dargv[0] = "build/kanxeod";
	dargv[1] = PORT_ARG;
	dargv[2] = NULL;

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

static void reset_state(void)
{
	system("rm -rf '" CONTAINER_DEFS_PATH "'");
}

/* Fetches container name's current pid, or -1 if it isn't currently known. */
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

static int container_exists(const struct kx_client *c, const char *name)
{
	char path[128];
	struct kx_response r;
	int status;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (kx_client_request(c, "GET", path, NULL, &r) != 0)
		return -1;
	status = r.status;
	kx_response_free(&r);
	return status == 200;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;

	reset_state();
	if (test_image_fixture_build(IMAGE_ROOT, "build/daemon_child", "daemon_child") != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;

	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	/* 1. crash restart: a fast-exiting restart:"always" container comes
	 * back on its own, with a real, measured delay -- not instantly. */
	{
		long pid1, pid2;
		time_t gone_at, back_at;
		int attempts;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"crasher\",\"image\":\"restarttest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"],"
		                       "\"restart\":\"always\"}",
		                       &r) != 0 ||
		    r.status != 201 || !str_eq(json_str_field(r.json, "restart"), "always")) {
			fprintf(stderr, "FAIL: POST crasher, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		pid1 = fetch_pid(&client, "crasher");
		if (pid1 < 0) {
			fprintf(stderr, "FAIL: crasher not running right after create\n");
			ok = 0;
		}

		/* Wait for it to exit (container sleeps 1s) */
		gone_at = 0;
		for (attempts = 0; attempts < 40; attempts++) {
			if (container_exists(&client, "crasher") == 0) {
				gone_at = time(NULL);
				break;
			}
			usleep(100000);
		}
		if (gone_at == 0) {
			fprintf(stderr, "FAIL: crasher never exited\n");
			ok = 0;
		}

		/* Wait for it to come back */
		back_at = 0;
		pid2 = -1;
		for (attempts = 0; attempts < 60; attempts++) {
			pid2 = fetch_pid(&client, "crasher");
			if (pid2 >= 0 && pid2 != pid1) {
				back_at = time(NULL);
				break;
			}
			usleep(100000);
		}
		if (back_at == 0) {
			fprintf(stderr, "FAIL: crasher never came back after exiting\n");
			ok = 0;
		} else if (back_at - gone_at < 1) {
			fprintf(stderr,
			        "FAIL: crasher restarted too fast (%ld s) -- delay doesn't look real\n",
			        (long)(back_at - gone_at));
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", "/v1/containers/crasher", NULL, &r);
		kx_response_free(&r);
	}

	/* 2. DELETE removes the persisted definition permanently -- a
	 * long-sleeping restart:"always" container, deleted, must not come
	 * back even across a real daemon restart. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"deleteme\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST deleteme, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/containers/deleteme", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE deleteme, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. depends_on ordering + boot autostart: depA (no deps) and depB
	 * (depends_on depA) both restart:"always", long-sleeping. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"depA\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST depA, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"depB\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"always\",\"depends_on\":[\"depA\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST depB, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 4. a genuine cycle (cycleA <-> cycleB) plus an independent,
	 * unrelated restart:"always" container ("innocent") -- the cycle
	 * must not prevent innocent from autostarting after restart. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cycleA\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"always\",\"depends_on\":[\"cycleB\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST cycleA, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cycleB\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"always\",\"depends_on\":[\"cycleA\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST cycleB, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"innocent\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST innocent, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. an unknown dependency -- also must not affect anything else. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"needsghost\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"always\",\"depends_on\":[\"ghost\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST needsghost, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* Now the real proof: restart the daemon process itself (not just
	 * reload in-memory state) and confirm every expectation above. */
	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (first instance)\n");
		ok = 0;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: restarted daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}
	/* containerdef_autostart_all() runs synchronously before the
	 * listening socket's own requests are serviced, but give it a
	 * moment regardless -- clone3()+overlay setup for several
	 * containers is real work, not instant. */
	usleep(500000);

	if (container_exists(&client, "deleteme") == 1) {
		fprintf(stderr, "FAIL: deleteme came back after being DELETEd -- persistence "
		                "removal didn't work\n");
		ok = 0;
	}

	{
		int a_exists = container_exists(&client, "depA");
		int b_exists = container_exists(&client, "depB");
		long pid_a = fetch_pid(&client, "depA");
		long pid_b = fetch_pid(&client, "depB");

		if (a_exists != 1 || b_exists != 1) {
			fprintf(stderr,
			        "FAIL: depA/depB did not both autostart after restart "
			        "(a_exists=%d b_exists=%d)\n",
			        a_exists, b_exists);
			ok = 0;
		} else if (pid_a < 0 || pid_b < 0 || pid_a >= pid_b) {
			fprintf(stderr,
			        "FAIL: depA (pid %ld) does not appear to have started before "
			        "depB (pid %ld) -- depends_on ordering looks wrong\n",
			        pid_a, pid_b);
			ok = 0;
		}
	}

	if (container_exists(&client, "cycleA") == 1 || container_exists(&client, "cycleB") == 1) {
		fprintf(stderr, "FAIL: cycleA/cycleB autostarted despite forming a cycle -- "
		                "cycle detection didn't work\n");
		ok = 0;
	}
	if (container_exists(&client, "needsghost") == 1) {
		fprintf(stderr,
		        "FAIL: needsghost autostarted despite depending on an unknown definition\n");
		ok = 0;
	}
	if (container_exists(&client, "innocent") != 1) {
		fprintf(stderr,
		        "FAIL: innocent (unrelated to the cycle/unknown-dep defs) did not "
		        "autostart -- one bad definition incorrectly took down another\n");
		ok = 0;
	}

	/* cleanup */
	{
		const char *cleanup[] = { "depA", "depB", "cycleA", "cycleB", "needsghost", "innocent" };
		size_t i;

		for (i = 0; i < sizeof(cleanup) / sizeof(cleanup[0]); i++) {
			char path[128];

			snprintf(path, sizeof(path), "/v1/containers/%s", cleanup[i]);
			memset(&r, 0, sizeof(r));
			kx_client_request(&client, "DELETE", path, NULL, &r);
			kx_response_free(&r);
		}
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (second instance)\n");
		ok = 0;
	}

	printf(ok ? "CONTAINER RESTART RESULT: PASS\n" : "CONTAINER RESTART RESULT: FAIL\n");
	return ok ? 0 : 1;
}
