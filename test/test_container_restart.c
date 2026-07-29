/*
 * Phase 13 part 1 end-to-end test: proves persisted, auto-restarting
 * containers ("restart": "always"/"depends_on" on POST/DELETE
 * /v1/containers -- daemon/src/containerdef.c + the create_container_
 * from_body()/containerdef_autostart_all()/timerfd crash-restart
 * wiring in daemon/src/main.c) over real HTTP, with a live daemon
 * process actually restarted (not just its in-memory state reset) to
 * prove persistence genuinely survives that, not just an in-process
 * reload.
 *
 * Also covers Phase 13 part 2 (ADR-0026): TCP readiness checks on
 * depends_on. wait_for_tcp_ready() only ever runs from inside
 * containerdef_autostart_all() (boot-time autostart), never from the
 * live POST /v1/containers path itself -- so, unlike the crash-restart
 * scenario above, the readiness scenarios below only actually block
 * during the same daemon restart the rest of this file already
 * performs, not at creation time.
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
#define READY_NETWORK_NAME "readytest"
#define READY_NETWORK_SUBNET "172.60.0.0"
#define READY_TCP_PORT 9100
#define NEVER_READY_TCP_PORT 9999
#define STR_(x) #x
#define STR(x) STR_(x)

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
	{
		char tcp_child_path[256];

		snprintf(tcp_child_path, sizeof(tcp_child_path), "%s/bin/tcp_listen_child", IMAGE_ROOT);
		if (test_image_fixture_copy_file("build/tcp_listen_child", tcp_child_path) != 0)
			return 1;
	}

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

	/* 6. readiness (Phase 13 part 2): a network is required (400
	 * without one), mirroring dns_register's existing requirement. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"noreadynet\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"readiness\":{\"tcp_port\":" STR(READY_TCP_PORT) "}}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: readiness without networks expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"" READY_NETWORK_NAME "\",\"subnet\":\"" READY_NETWORK_SUBNET
	                       "\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST " READY_NETWORK_NAME ", status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* depR only starts listening on READY_TCP_PORT after a real 2s
	 * delay (tcp_listen_child) -- depS (depends_on depR, with depR's
	 * own readiness configured) must genuinely wait for that, proven
	 * below by timing the daemon restart itself. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"depR\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/tcp_listen_child\",\"" STR(READY_TCP_PORT) "\",\"2\"],"
	                       "\"networks\":[\"" READY_NETWORK_NAME "\"],\"restart\":\"always\","
	                       "\"readiness\":{\"tcp_port\":" STR(READY_TCP_PORT) ",\"timeout_seconds\":10}}",
	                       &r) != 0 ||
	    r.status != 201 ||
	    json_num_field(json_object_get(r.json, "readiness"), "tcp_port") != READY_TCP_PORT) {
		fprintf(stderr, "FAIL: POST depR, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"depS\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"networks\":[\"" READY_NETWORK_NAME "\"],\"restart\":\"always\","
	                       "\"depends_on\":[\"depR\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST depS, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* neverready never listens on NEVER_READY_TCP_PORT at all -- its
	 * own readiness must time out (1s) without blocking boot forever,
	 * and afterNeverReady (depends on it) must still autostart anyway
	 * (best-effort, not a hard failure). */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"neverready\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"networks\":[\"" READY_NETWORK_NAME "\"],\"restart\":\"always\","
	                       "\"readiness\":{\"tcp_port\":" STR(NEVER_READY_TCP_PORT) ",\"timeout_seconds\":1}}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST neverready, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"afterNeverReady\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"networks\":[\"" READY_NETWORK_NAME "\"],\"restart\":\"always\","
	                       "\"depends_on\":[\"neverready\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST afterNeverReady, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* Now the real proof: restart the daemon process itself (not just
	 * reload in-memory state) and confirm every expectation above. */
	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (first instance)\n");
		ok = 0;
	}

	{
		time_t restart_started, restart_ready;

		restart_started = time(NULL);
		daemon_pid = start_daemon();
		if (daemon_pid < 0)
			return 1;
		if (wait_for_daemon(&client, 50) != 0) {
			fprintf(stderr, "FAIL: restarted daemon never accepted connections\n");
			kill(daemon_pid, SIGKILL);
			waitpid(daemon_pid, NULL, 0);
			return 1;
		}
		/* By the time this call returns, containerdef_autostart_all()
		 * (including any readiness waits it performs) has already run
		 * to completion -- it's a blocking step ahead of the reactor's
		 * own accept loop, so the response to this request's own GET
		 * /v1/health can't have been sent any earlier. depR's readiness
		 * (2s, see below) is the dominant contributor here, so a real
		 * wait shows up as a multi-second floor on restart_ready -
		 * restart_started; a no-op/skipped wait would not. */
		restart_ready = time(NULL);
		if (restart_ready - restart_started < 1) {
			fprintf(stderr,
			        "FAIL: daemon restart (including depR's own 2s readiness wait) "
			        "took only %lds -- readiness wait doesn't look real\n",
			        (long)(restart_ready - restart_started));
			ok = 0;
		}
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

	/* Phase 13 part 2: readiness. */
	{
		int r_exists = container_exists(&client, "depR");
		int s_exists = container_exists(&client, "depS");
		long pid_r = fetch_pid(&client, "depR");
		long pid_s = fetch_pid(&client, "depS");

		if (r_exists != 1 || s_exists != 1) {
			fprintf(stderr,
			        "FAIL: depR/depS did not both autostart after restart "
			        "(r_exists=%d s_exists=%d)\n",
			        r_exists, s_exists);
			ok = 0;
		} else if (pid_r < 0 || pid_s < 0 || pid_r >= pid_s) {
			fprintf(stderr,
			        "FAIL: depR (pid %ld) does not appear to have started before "
			        "depS (pid %ld) -- depends_on ordering looks wrong\n",
			        pid_r, pid_s);
			ok = 0;
		}
	}
	if (container_exists(&client, "neverready") != 1 ||
	    container_exists(&client, "afterNeverReady") != 1) {
		fprintf(stderr,
		        "FAIL: neverready/afterNeverReady did not autostart -- a readiness "
		        "check that never succeeds should time out and let boot proceed "
		        "(best-effort), not block or skip the dependent\n");
		ok = 0;
	}

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/containers/depR", NULL, &r) != 0 ||
	    r.status != 200 ||
	    json_num_field(json_object_get(r.json, "readiness"), "tcp_port") != READY_TCP_PORT) {
		fprintf(stderr, "FAIL: GET depR did not echo its own readiness.tcp_port\n");
		ok = 0;
	}
	kx_response_free(&r);

	/* cleanup */
	{
		const char *cleanup[] = { "depA",       "depB",     "cycleA",         "cycleB",
			                       "needsghost", "innocent", "depR",           "depS",
			                       "neverready", "afterNeverReady" };
		size_t i;

		for (i = 0; i < sizeof(cleanup) / sizeof(cleanup[0]); i++) {
			char path[128];

			snprintf(path, sizeof(path), "/v1/containers/%s", cleanup[i]);
			memset(&r, 0, sizeof(r));
			kx_client_request(&client, "DELETE", path, NULL, &r);
			kx_response_free(&r);
		}
	}

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/" READY_NETWORK_NAME, NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE " READY_NETWORK_NAME ", status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (second instance)\n");
		ok = 0;
	}

	printf(ok ? "CONTAINER RESTART RESULT: PASS\n" : "CONTAINER RESTART RESULT: FAIL\n");
	return ok ? 0 : 1;
}
