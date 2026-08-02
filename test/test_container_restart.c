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

#include <limits.h>
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
#define READY_NETWORK_NAME "readytest"
#define READY_NETWORK_SUBNET "172.60.0.0"
#define READY_TCP_PORT 9100
#define NEVER_READY_TCP_PORT 9999
#define STR_(x) #x
#define STR(x) STR_(x)

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static char g_container_defs_path[PATH_MAX];

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

static void reset_state(void)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_container_defs_path);
	system(cmd);
}

/*
 * Fetches container name's current pid, or -1 if it isn't currently
 * known -- "known" meaning genuinely live, not merely "GET returns
 * 200." Since ADR-0045, GET also returns 200 for a stopped-but-
 * defined container (a synthesized entry with pid: null, status:
 * "stopped") -- excluded here explicitly, since json_num_field() on a
 * null "pid" would otherwise read back as 0, which every caller in
 * this file uses (indistinguishable from "not yet observed") to mean
 * "this is a genuinely new/different pid," a false positive this
 * test's own crash-restart-suppression scenarios below depend on not
 * happening.
 */
static long fetch_pid(const struct kx_client *c, const char *name)
{
	char path[128];
	struct kx_response r;
	long pid = -1;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (kx_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200 &&
	    !str_eq(json_str_field(r.json, "status"), "stopped"))
		pid = json_num_field(r.json, "pid");
	kx_response_free(&r);
	return pid;
}

/*
 * True if name is genuinely live right now -- not merely persisted.
 * Since ADR-0045, GET 200 alone no longer implies that (a stopped-but-
 * defined container is also 200, status "stopped"); every call site in
 * this file uses this function to mean "live," so that case reads as
 * not-existing here, matching every call site's actual intent from
 * before ADR-0045 (this test predates it).
 */
static int container_exists(const struct kx_client *c, const char *name)
{
	char path[128];
	struct kx_response r;
	int result;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (kx_client_request(c, "GET", path, NULL, &r) != 0)
		return -1;
	result = r.status == 200 && !str_eq(json_str_field(r.json, "status"), "stopped");
	kx_response_free(&r);
	return result;
}

/* POST .../stop; returns the HTTP status, or -1 if the daemon couldn't be reached. */
static int stop_container(const struct kx_client *c, const char *name)
{
	char path[160];
	struct kx_response r;
	int status;

	snprintf(path, sizeof(path), "/v1/containers/%s/stop", name);
	memset(&r, 0, sizeof(r));
	if (kx_client_request(c, "POST", path, NULL, &r) != 0)
		return -1;
	status = r.status;
	kx_response_free(&r);
	return status;
}

/* Polls until name is gone (GET 404), or max_attempts*100ms elapses.
 * Returns the time it was first observed gone, or 0 on timeout. */
static time_t wait_for_gone(const struct kx_client *c, const char *name, int max_attempts)
{
	int attempts;

	for (attempts = 0; attempts < max_attempts; attempts++) {
		if (container_exists(c, name) == 0)
			return time(NULL);
		usleep(100000);
	}
	return 0;
}

/* Polls until name's pid differs from old_pid (a restart happened), or
 * max_attempts*100ms elapses. Returns the time it was first observed,
 * or 0 on timeout. */
static time_t wait_for_new_pid(const struct kx_client *c, const char *name, long old_pid,
                                int max_attempts)
{
	int attempts;

	for (attempts = 0; attempts < max_attempts; attempts++) {
		long pid = fetch_pid(c, name);

		if (pid >= 0 && pid != old_pid)
			return time(NULL);
		usleep(100000);
	}
	return 0;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/images/restarttest/rootfs", g_data_dir);
	snprintf(g_container_defs_path, sizeof(g_container_defs_path), "%s/container_defs.json",
	         g_data_dir);

	reset_state();
	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	{
		char tcp_child_path[PATH_MAX];

		snprintf(tcp_child_path, sizeof(tcp_child_path), "%s/bin/tcp_listen_child",
		         g_image_root);
		if (test_image_fixture_copy_file("build/tcp_listen_child", tcp_child_path) != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
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

	/* Phase 13 part 3 (ADR-0027): restart policy expansion + backoff.
	 * Every scenario in this sub-section is fully self-contained before
	 * a daemon restart and cleans up its own definition inline -- only
	 * the unless-stopped/always comparison further below needs to
	 * survive the real daemon restart this file performs later. */

	/* on-failure: a clean (exit 0) exit must NOT restart. */
	{
		time_t gone_at;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"onfailclean\",\"image\":\"restarttest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"],"
		                       "\"restart\":\"on-failure\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST onfailclean, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		gone_at = wait_for_gone(&client, "onfailclean", 40);
		if (gone_at == 0) {
			fprintf(stderr, "FAIL: onfailclean never exited\n");
			ok = 0;
		}
		/* No restart timer is ever armed for this case -- if it were
		 * about to come back, the default 2s delay would have already
		 * elapsed well within this window. */
		if (wait_for_new_pid(&client, "onfailclean", -1, 40) != 0) {
			fprintf(stderr,
			        "FAIL: onfailclean (restart:on-failure, clean exit) came back -- "
			        "should stay down for the rest of this daemon's uptime\n");
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", "/v1/containers/onfailclean", NULL, &r);
		kx_response_free(&r);
	}

	/* on-failure: a crash (nonzero exit) exit DOES restart, same
	 * measured-gap style as the plain "always" crasher above -- proves
	 * the delay/backoff wiring applies to on-failure too. */
	{
		long pid1;
		time_t gone_at, back_at;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"onfailcrash\",\"image\":\"restarttest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"7\"],"
		                       "\"restart\":\"on-failure\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST onfailcrash, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		pid1 = fetch_pid(&client, "onfailcrash");
		gone_at = wait_for_gone(&client, "onfailcrash", 40);
		/* Its own first-ever exit already gets a 2x backoff multiplier
		 * (consecutive_failures 0->1, see ADR-0027) -- with the default
		 * 2s base that's ~4s, not the bare base itself, hence the
		 * generous window here rather than a tight one around 2s. */
		back_at = wait_for_new_pid(&client, "onfailcrash", pid1, 100);
		if (gone_at == 0 || back_at == 0) {
			fprintf(stderr, "FAIL: onfailcrash (restart:on-failure, crash exit) did not "
			                "come back\n");
			ok = 0;
		} else if (back_at - gone_at < 1) {
			fprintf(stderr, "FAIL: onfailcrash restarted too fast (%ld s)\n",
			        (long)(back_at - gone_at));
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", "/v1/containers/onfailcrash", NULL, &r);
		kx_response_free(&r);
	}

	/* restart_delay_seconds overrides the default base delay. */
	{
		long pid1;
		time_t gone_at, back_at;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"delaytest\",\"image\":\"restarttest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"],"
		                       "\"restart\":\"always\",\"restart_delay_seconds\":5}",
		                       &r) != 0 ||
		    r.status != 201 || json_num_field(r.json, "restart_delay_seconds") != 5) {
			fprintf(stderr, "FAIL: POST delaytest, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		pid1 = fetch_pid(&client, "delaytest");
		gone_at = wait_for_gone(&client, "delaytest", 40);
		/* Its own first-ever exit gets a 2x backoff multiplier on top
		 * of this 5s base (see ADR-0027) -- actual delay is ~10s, so
		 * the window and lower bound both allow for that, not just
		 * the bare 5s override value. */
		back_at = wait_for_new_pid(&client, "delaytest", pid1, 200);
		if (gone_at == 0 || back_at == 0) {
			fprintf(stderr, "FAIL: delaytest never came back\n");
			ok = 0;
		} else if (back_at - gone_at < 8) {
			fprintf(stderr,
			        "FAIL: delaytest restarted after only %ld s -- restart_delay_seconds "
			        "override doesn't look honored\n",
			        (long)(back_at - gone_at));
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", "/v1/containers/delaytest", NULL, &r);
		kx_response_free(&r);
	}

	/* stop during a pending crash-restart delay window -- proves
	 * handle_restart_timer_event()'s own stopped check, not just
	 * containerdef_autostart_all()'s. */
	{
		time_t gone_at;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"stopwindow\",\"image\":\"restarttest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"],"
		                       "\"restart\":\"always\",\"restart_delay_seconds\":5}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST stopwindow, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		gone_at = wait_for_gone(&client, "stopwindow", 40);
		if (gone_at == 0) {
			fprintf(stderr, "FAIL: stopwindow never exited\n");
			ok = 0;
		}
		/* The 5s restart timer is armed and pending right now -- stop
		 * it mid-flight. */
		if (stop_container(&client, "stopwindow") != 200) {
			fprintf(stderr, "FAIL: POST stopwindow/stop during pending delay\n");
			ok = 0;
		}
		/* Past when the timer would have fired -- its own first-ever
		 * exit gets a 2x backoff multiplier on top of the 5s base (see
		 * ADR-0027), so the real armed delay here is ~10s, not 5s. */
		if (wait_for_new_pid(&client, "stopwindow", -1, 200) != 0) {
			fprintf(stderr, "FAIL: stopwindow came back after being stopped mid-delay -- "
			                "handle_restart_timer_event()'s stopped check didn't work\n");
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", "/v1/containers/stopwindow", NULL, &r);
		kx_response_free(&r);
	}

	/* Backoff genuinely grows across consecutive failures: a fast
	 * crash-loop with base delay 2s -- consecutive_failures becomes 1
	 * on the first-ever exit (uptime well under the 30s stability
	 * threshold), so the first restart gap is already ~2x base (~4s),
	 * and the second is ~4x base (~8s). */
	{
		long pid1, pid2;
		time_t gone1, back1, gone2, back2;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"backoffgrow\",\"image\":\"restarttest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"1\"],"
		                       "\"restart\":\"always\",\"restart_delay_seconds\":2}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST backoffgrow, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		pid1 = fetch_pid(&client, "backoffgrow");
		gone1 = wait_for_gone(&client, "backoffgrow", 40);
		back1 = wait_for_new_pid(&client, "backoffgrow", pid1, 100);
		if (gone1 == 0 || back1 == 0) {
			fprintf(stderr, "FAIL: backoffgrow's first restart never happened\n");
			ok = 0;
		} else if (back1 - gone1 < 3) {
			fprintf(stderr,
			        "FAIL: backoffgrow's first restart gap (%ld s) too short -- expected "
			        "~4s (2x the 2s base, consecutive_failures already 1 on a first fast "
			        "exit)\n",
			        (long)(back1 - gone1));
			ok = 0;
		}

		pid2 = fetch_pid(&client, "backoffgrow");
		gone2 = wait_for_gone(&client, "backoffgrow", 40);
		back2 = wait_for_new_pid(&client, "backoffgrow", pid2, 150);
		if (gone2 == 0 || back2 == 0) {
			fprintf(stderr, "FAIL: backoffgrow's second restart never happened\n");
			ok = 0;
		} else if (back2 - gone2 < 6) {
			fprintf(stderr,
			        "FAIL: backoffgrow's second restart gap (%ld s) too short -- expected "
			        "~8s, backoff doesn't look like it's genuinely growing\n",
			        (long)(back2 - gone2));
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", "/v1/containers/backoffgrow", NULL, &r);
		kx_response_free(&r);
	}

	/* Backoff resets for a container that ran stably (>= the 30s
	 * stability threshold) before exiting -- its own first-ever exit
	 * takes the reset branch of the exact same conditional
	 * backoffgrow's first exit took the increment branch of, so the
	 * resulting delay (~1x the 2s base, ~2s) directly contrasts with
	 * backoffgrow's own ~2x-base first gap above. A single container
	 * can't be made to alternate "fast crash" vs "stable run" between
	 * restarts (the daemon replays its exact same cmd verbatim every
	 * time, by design -- ADR-0025), so this is proven by comparison
	 * against backoffgrow rather than one container's own before/after. */
	{
		long pid1;
		time_t gone_at, back_at;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"backoffstable\",\"image\":\"restarttest\","
		                       "\"cmd\":[\"/bin/daemon_child\",\"32\",\"9\"],"
		                       "\"restart\":\"always\",\"restart_delay_seconds\":2}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST backoffstable, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		pid1 = fetch_pid(&client, "backoffstable");
		gone_at = wait_for_gone(&client, "backoffstable", 400);
		back_at = wait_for_new_pid(&client, "backoffstable", pid1, 60);
		if (gone_at == 0 || back_at == 0) {
			fprintf(stderr, "FAIL: backoffstable never came back\n");
			ok = 0;
		} else if (back_at - gone_at < 1) {
			fprintf(stderr, "FAIL: backoffstable restarted too fast (%ld s)\n",
			        (long)(back_at - gone_at));
			ok = 0;
		} else if (back_at - gone_at > 3) {
			fprintf(stderr,
			        "FAIL: backoffstable's restart gap (%ld s) looks doubled, not reset -- "
			        "expected ~2s (1x base), a >=30s-uptime exit should not have grown "
			        "consecutive_failures\n",
			        (long)(back_at - gone_at));
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		kx_client_request(&client, "DELETE", "/v1/containers/backoffstable", NULL, &r);
		kx_response_free(&r);
	}

	/* Validation: the expanded restart enum, restart_delay_seconds
	 * bounds, and stop's 404/idempotent-200 shape. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"badrestart\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"],\"restart\":\"bogus\"}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: restart:\"bogus\" expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"baddelay\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"],\"restart\":\"always\","
	                       "\"restart_delay_seconds\":0}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: restart_delay_seconds=0 expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (stop_container(&client, "no-such-container") != 404) {
		fprintf(stderr, "FAIL: stop on an unknown container expected 404\n");
		ok = 0;
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

	/* Explicit --gateway= (ADR-0037): readiness checks connect() from
	 * the daemon's own root netns straight at a container's IP
	 * (do_readiness_check(), daemon/src/main.c) -- on the new gateway-
	 * less default, the host has no route into this subnet at all
	 * (correct, intended behavior, not a bug), so the readiness check
	 * itself would never be able to reach in. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"" READY_NETWORK_NAME "\",\"subnet\":\"" READY_NETWORK_SUBNET
	                       "\",\"prefix_len\":24,\"gateway\":\"172.60.0.1\"}",
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

	/* 6. unless-stopped vs always across a real daemon restart:
	 * stopping a container is the one thing that's supposed to
	 * distinguish them. Both long-sleeping so a stop actually has a
	 * live process to kill. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"stopalways\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST stopalways, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"stopunless\",\"image\":\"restarttest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"restart\":\"unless-stopped\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST stopunless, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (stop_container(&client, "stopalways") != 200) {
		fprintf(stderr, "FAIL: POST stopalways/stop\n");
		ok = 0;
	}
	if (stop_container(&client, "stopunless") != 200) {
		fprintf(stderr, "FAIL: POST stopunless/stop\n");
		ok = 0;
	}
	/* Idempotent: calling stop again is still 200, not an error. */
	if (stop_container(&client, "stopunless") != 200) {
		fprintf(stderr, "FAIL: POST stopunless/stop a second time expected 200 (idempotent)\n");
		ok = 0;
	}
	if (container_exists(&client, "stopalways") != 0 || container_exists(&client, "stopunless") != 0) {
		fprintf(stderr, "FAIL: stopalways/stopunless still exist right after being stopped\n");
		ok = 0;
	}

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

	/* Phase 13 part 3: the core unless-stopped-vs-always split. Both
	 * were stopped identically above, right before this same restart --
	 * "always" gets a fresh chance at every daemon boot regardless;
	 * "unless-stopped" remembers the stop across exactly this. */
	if (container_exists(&client, "stopalways") != 1) {
		fprintf(stderr,
		        "FAIL: stopalways did not autostart after a daemon restart -- restart:"
		        "\"always\" should not remember a prior stop across a daemon restart\n");
		ok = 0;
	}
	if (container_exists(&client, "stopunless") != 0) {
		fprintf(stderr,
		        "FAIL: stopunless autostarted after a daemon restart despite being "
		        "stopped -- restart:\"unless-stopped\" should stay down until explicitly "
		        "re-created\n");
		ok = 0;
	}

	/* cleanup */
	{
		const char *cleanup[] = { "depA",       "depB",       "cycleA",     "cycleB",
			                       "needsghost", "innocent",   "depR",       "depS",
			                       "neverready", "afterNeverReady", "stopalways", "stopunless" };
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

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "CONTAINER RESTART RESULT: PASS\n" : "CONTAINER RESTART RESULT: FAIL\n");
	return ok ? 0 : 1;
}
