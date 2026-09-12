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
#include "test_cleanup.h"

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

/* ADR-0260: one service out of a container GET's services[]. */
static const struct json_value *service_at(const struct cix_response *r, int idx)
{
	const struct json_value *svcs = r->json != NULL ? json_object_get(r->json, "services") : NULL;

	if (svcs == NULL || svcs->type != JSON_ARRAY || (size_t)idx >= svcs->u.array.count)
		return NULL;
	return svcs->u.array.items[idx];
}

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

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static long json_num_field(const struct json_value *obj, const char *key)
{
	return (long)json_as_number(json_object_get(obj, key));
}

/*
 * services[idx].failure.reason, or "" when the service carries no
 * failure (#413). A readiness probe that times out is REPORTED and then
 * treated as ready so dependents proceed, so the probe's own verdict is
 * only ever visible here -- the container is "ready" either way, which
 * is exactly how a probe that could never pass went unnoticed for the
 * whole life of the tcp_port kind.
 */
static const char *svc_fail_reason(const struct cix_response *r, int idx)
{
	const struct json_value *svc = service_at(r, idx);
	const struct json_value *f = svc != NULL ? json_object_get(svc, "failure") : NULL;
	const char *reason;

	if (f == NULL || f->type != JSON_OBJECT)
		return "";
	reason = json_str_field(f, "reason");
	return reason != NULL ? reason : "";
}

/*
 * Wait until services[idx].failure.reason on `name` equals `want`,
 * and copy whatever it actually was into `out` either way. `want` of
 * "" means "no failure at all".
 */
static int wait_svc_fail_reason(const struct cix_client *c, const char *name, int idx,
                                 const char *want, int timeout_ms, char *out, size_t out_size)
{
	char path[128];
	int waited = 0;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	snprintf(out, out_size, "%s", "(never read)");
	for (;;) {
		struct cix_response r;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200) {
			const char *got = svc_fail_reason(&r, idx);

			snprintf(out, out_size, "%s", got);
			if (strcmp(got, want) == 0) {
				cix_response_free(&r);
				return 0;
			}
		}
		cix_response_free(&r);
		if (waited >= timeout_ms)
			return -1;
		usleep(250000);
		waited += 250;
	}
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
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
static long fetch_pid(const struct cix_client *c, const char *name)
{
	char path[128];
	struct cix_response r;
	long pid = -1;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200 &&
	    !str_eq(json_str_field(r.json, "status"), "stopped"))
		pid = json_num_field(r.json, "pid");
	cix_response_free(&r);
	return pid;
}

/*
 * True if name is genuinely live right now -- not merely persisted.
 * GET 200 alone no longer implies that: since ADR-0045 a stopped-but-
 * defined container is 200 with status "stopped", and since ADR-0181's
 * persist-all a container that exited on its own is retained (not
 * deleted) as 200 with status "exited". Both are persisted-not-live, so
 * both read as not-existing here -- every call site in this file uses
 * this function to mean "live", matching its intent from before either
 * ADR (this test predates both).
 */
static int container_exists(const struct cix_client *c, const char *name)
{
	char path[128];
	struct cix_response r;
	const char *status;
	int result;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", path, NULL, &r) != 0)
		return -1;
	status = json_str_field(r.json, "status");
	result = r.status == 200 && !str_eq(status, "stopped") && !str_eq(status, "exited");
	cix_response_free(&r);
	return result;
}

/* POST .../stop; returns the HTTP status, or -1 if the daemon couldn't be reached. */
static int stop_container(const struct cix_client *c, const char *name)
{
	char path[160];
	struct cix_response r;
	int status;

	snprintf(path, sizeof(path), "/v1/containers/%s/stop", name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "POST", path, NULL, &r) != 0)
		return -1;
	status = r.status;
	cix_response_free(&r);
	return status;
}

/* Polls until name is no longer live -- container_exists() counts a 404,
 * a kept-but-"stopped" container, and a self-exited "exited" container
 * (ADR-0181 persist-all) all as gone, which is exactly what every "did
 * NOT restart" check here wants. Times out after max_attempts*100ms.
 * Returns the time it was first observed
 * gone, or 0 on timeout. */
static time_t wait_for_gone(const struct cix_client *c, const char *name, int max_attempts)
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
static time_t wait_for_new_pid(const struct cix_client *c, const char *name, long old_pid,
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
	struct cix_client client;
	int ok = 1;
	struct cix_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/restarttest/v1/rootfs", g_data_dir);
	snprintf(g_container_defs_path, sizeof(g_container_defs_path), "%s/state/container_defs.json",
	         g_data_dir);

	reset_state();
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/restarttest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}
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

	cix_client_init(&client, "127.0.0.1", TEST_PORT);
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
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"crasher\",\"image\":\"restarttest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
		                       "\"restart\":\"always\"}",
		                       &r) != 0 ||
		    r.status != 201 || !str_eq(json_str_field(r.json, "restart"), "always")) {
			fprintf(stderr, "FAIL: POST crasher, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
		cix_client_request(&client, "DELETE", "/v1/containers/crasher", NULL, &r);
		cix_response_free(&r);
	}

	/* Phase 13 part 3 (ADR-0027): restart policy expansion + backoff.
	 * Every scenario in this sub-section is fully self-contained before
	 * a daemon restart and cleans up its own definition inline -- only
	 * the unless-stopped/always comparison further below needs to
	 * survive the real daemon restart this file performs later. */

	/* on-failure: a clean (exit 0) exit must NOT restart. */
	{
		time_t gone_at;
		long pid1;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"onfailclean\",\"image\":\"restarttest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
		                       "\"restart\":\"on-failure\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST onfailclean, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* The running pid, captured before the clean exit. ADR-0181's
		 * persist-all keeps the exited container around (status "exited",
		 * this same pid retained), so "did it restart?" can no longer be
		 * "did any pid >= 0 appear" (-1 sentinel) -- the retained dead pid
		 * would false-positive. It's "did a DIFFERENT pid appear", exactly
		 * as the onfailcrash restart case below already tests. */
		pid1 = fetch_pid(&client, "onfailclean");

		gone_at = wait_for_gone(&client, "onfailclean", 40);
		if (gone_at == 0) {
			fprintf(stderr, "FAIL: onfailclean never exited\n");
			ok = 0;
		}
		/* No restart timer is ever armed for this case -- if it were
		 * about to come back, the default 2s delay would have already
		 * elapsed well within this window. A different pid never appears;
		 * the container stays "exited" until the DELETE below. */
		if (wait_for_new_pid(&client, "onfailclean", pid1, 40) != 0) {
			fprintf(stderr,
			        "FAIL: onfailclean (restart:on-failure, clean exit) came back -- "
			        "should stay down for the rest of this daemon's uptime\n");
			ok = 0;
		}

		/* ADR-0181: the retained "exited" container is startable again by
		 * hand -- POST .../start must actually bring it back with a NEW,
		 * live pid, not silently 200 the stale exited entry. This is the
		 * "persist until delete, start again" model the whole ADR exists
		 * for. It sleeps 1s again before its next clean exit, so there is
		 * a real live window to observe. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers/onfailclean/start", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: start of exited onfailclean, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		/* wait_for_new_pid() returns nonzero once a pid different from
		 * pid1 appears -- i.e. the container genuinely restarted, not a
		 * silent 200 on the stale exited entry. 0 (timeout) is the
		 * failure here. */
		if (wait_for_new_pid(&client, "onfailclean", pid1, 40) == 0) {
			fprintf(stderr,
			        "FAIL: start of exited onfailclean did not bring it back live "
			        "with a new pid -- silent no-op on the retained exited entry?\n");
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/containers/onfailclean", NULL, &r);
		cix_response_free(&r);
	}

	/*
	 * issue #78: a container that dies by a SIGNAL (not a plain exit)
	 * must report term_signal set to that signal number and an
	 * exit_reason naming it -- so a signal death is never mistaken for
	 * a real exit code of the same number. daemon_child faults with a
	 * NULL deref (SIGSEGV, signal 11 -- see its own header for why not
	 * SIGKILL: a container's PID 1 can only signal-kill itself via a
	 * synchronous fault); restart:"no" retains it as "exited"
	 * (ADR-0181), giving a stable window to read the fields back. The
	 * SIGKILL-from-stop/delete path (term_signal 9) is verified live on
	 * a real box, not here. A normal exit, by contrast, reports
	 * term_signal 0 (every other scenario in this file exercises that).
	 */
	{
		int i;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"sigdeath\",\"image\":\"restarttest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"-1\"]}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST sigdeath, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		for (i = 0; i < 60; i++) {
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers/sigdeath", NULL, &r) == 0 &&
			    r.status == 200 && str_eq(json_str_field(r.json, "status"), "exited"))
				break;
			usleep(100000);
		}
		if (r.status != 200 || !str_eq(json_str_field(r.json, "status"), "exited")) {
			fprintf(stderr, "FAIL: sigdeath never reached exited\n");
			ok = 0;
		} else if (json_num_field(r.json, "exit_status") != 128 + 11 ||
		           json_num_field(r.json, "term_signal") != 0) {
			/* ADR-0260: pid 1 is cix-init, so the CONTAINER never dies by
			 * a signal; the service did, and cix-init exits 128+signal. */
			fprintf(stderr, "FAIL: sigdeath expected exit_status 139 / term_signal 0, got %ld / %ld\n",
			        json_num_field(r.json, "exit_status"), json_num_field(r.json, "term_signal"));
			ok = 0;
		} else {
			const struct json_value *svcs = json_object_get(r.json, "services");
			const struct json_value *svc0 = (svcs != NULL && svcs->type == JSON_ARRAY && svcs->u.array.count > 0)
			                                    ? svcs->u.array.items[0] : NULL;
			const struct json_value *last = svc0 != NULL ? json_object_get(svc0, "last_exit") : NULL;

			if (last == NULL || !str_eq(json_str_field(last, "kind"), "killed") ||
			    json_num_field(last, "signal") != 11) {
				fprintf(stderr, "FAIL: sigdeath services[0].last_exit should be killed by signal 11\n");
				ok = 0;
			}
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/containers/sigdeath", NULL, &r);
		cix_response_free(&r);
	}

	/* on-failure: a crash (nonzero exit) exit DOES restart, same
	 * measured-gap style as the plain "always" crasher above -- proves
	 * the delay/backoff wiring applies to on-failure too. */
	{
		long pid1;
		time_t gone_at, back_at;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"onfailcrash\",\"image\":\"restarttest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"7\"]}],"
		                       "\"restart\":\"on-failure\"}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST onfailcrash, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
		cix_client_request(&client, "DELETE", "/v1/containers/onfailcrash", NULL, &r);
		cix_response_free(&r);
	}

	/* restart_delay_seconds overrides the default base delay. */
	{
		long pid1;
		time_t gone_at, back_at;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"delaytest\",\"image\":\"restarttest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
		                       "\"restart\":\"always\",\"restart_delay_seconds\":5}",
		                       &r) != 0 ||
		    r.status != 201 || json_num_field(r.json, "restart_delay_seconds") != 5) {
			fprintf(stderr, "FAIL: POST delaytest, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
		cix_client_request(&client, "DELETE", "/v1/containers/delaytest", NULL, &r);
		cix_response_free(&r);
	}

	/* stop during a pending crash-restart delay window -- proves
	 * handle_restart_timer_event()'s own stopped check, not just
	 * containerdef_autostart_all()'s. */
	{
		time_t gone_at;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"stopwindow\",\"image\":\"restarttest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],"
		                       "\"restart\":\"always\",\"restart_delay_seconds\":5}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST stopwindow, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
		cix_client_request(&client, "DELETE", "/v1/containers/stopwindow", NULL, &r);
		cix_response_free(&r);
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
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"backoffgrow\",\"image\":\"restarttest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"1\"]}],"
		                       "\"restart\":\"always\",\"restart_delay_seconds\":2}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST backoffgrow, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
		cix_client_request(&client, "DELETE", "/v1/containers/backoffgrow", NULL, &r);
		cix_response_free(&r);
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
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"backoffstable\",\"image\":\"restarttest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"32\",\"9\"]}],"
		                       "\"restart\":\"always\",\"restart_delay_seconds\":2}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST backoffstable, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
		cix_client_request(&client, "DELETE", "/v1/containers/backoffstable", NULL, &r);
		cix_response_free(&r);
	}

	/* Validation: the expanded restart enum, restart_delay_seconds
	 * bounds, and stop's 404/idempotent-200 shape. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"badrestart\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],\"restart\":\"bogus\"}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: restart:\"bogus\" expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"baddelay\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"1\",\"0\"]}],\"restart\":\"always\","
	                       "\"restart_delay_seconds\":0}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: restart_delay_seconds=0 expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	if (stop_container(&client, "no-such-container") != 404) {
		fprintf(stderr, "FAIL: stop on an unknown container expected 404\n");
		ok = 0;
	}

	/* 2. DELETE removes the persisted definition permanently -- a
	 * long-sleeping restart:"always" container, deleted, must not come
	 * back even across a real daemon restart. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"deleteme\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST deleteme, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/containers/deleteme", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE deleteme, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. depends_on ordering + boot autostart: depA (no deps) and depB
	 * (depends_on depA) both restart:"always", long-sleeping. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"depA\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST depA, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"depB\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"restart\":\"always\",\"depends_on\":[\"depA\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST depB, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 4. a genuine cycle (cycleA <-> cycleB) plus an independent,
	 * unrelated restart:"always" container ("innocent") -- the cycle
	 * must not prevent innocent from autostarting after restart. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cycleA\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"restart\":\"always\",\"depends_on\":[\"cycleB\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST cycleA, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cycleB\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"restart\":\"always\",\"depends_on\":[\"cycleA\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST cycleB, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"innocent\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST innocent, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 5. an unknown dependency -- also must not affect anything else. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"needsghost\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"restart\":\"always\",\"depends_on\":[\"ghost\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST needsghost, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 6. ADR-0260: readiness is derived from the services and there is
	 * no container-level field any more -- one that carries the old
	 * key is refused as unknown, not silently ignored. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"noreadynet\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"readiness\":{\"tcp_port\":" STR(READY_TCP_PORT) "}}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: a container-level readiness field expected 400 (unknown key), got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* Explicit --address= (ADR-0037, renamed by ADR-0067): readiness
	 * checks connect() from the daemon's own root netns straight at a
	 * container's IP (do_readiness_check(), daemon/src/main.c) -- on
	 * the new address-less default, the host has no route into this
	 * subnet at all (correct, intended behavior, not a bug), so the
	 * readiness check itself would never be able to reach in. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"" READY_NETWORK_NAME "\",\"subnet\":\"" READY_NETWORK_SUBNET
	                       "\",\"prefix_len\":24,\"address\":\"172.60.0.1\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST " READY_NETWORK_NAME ", status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* depR only starts listening on READY_TCP_PORT after a real 2s
	 * delay (tcp_listen_child) -- depS (depends_on depR, whose listener
	 * service declares a tcp ready probe, ADR-0260) must genuinely wait
	 * for that, proven below by timing the daemon restart itself. The
	 * probe is cix-init's, run from inside the container against its
	 * own address. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"depR\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"listener\",\"cmd\":[\"/bin/tcp_listen_child\",\"" STR(READY_TCP_PORT) "\",\"2\"],"
	                       "\"on_exit\":\"stop\",\"ready\":{\"tcp_port\":" STR(READY_TCP_PORT) ",\"timeout_seconds\":10}}],"
	                       "\"networks\":[\"" READY_NETWORK_NAME "\"],\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST depR, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *svcs = json_object_get(r.json, "services");
		const struct json_value *svc0 = (svcs != NULL && svcs->type == JSON_ARRAY && svcs->u.array.count > 0)
		                                    ? svcs->u.array.items[0] : NULL;

		if (svc0 == NULL ||
		    json_num_field(json_object_get(svc0, "ready_probe"), "tcp_port") != READY_TCP_PORT) {
			fprintf(stderr, "FAIL: depR's declared probe is not read back on services[0].ready_probe\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"depS\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"networks\":[\"" READY_NETWORK_NAME "\"],\"restart\":\"always\","
	                       "\"depends_on\":[\"depR\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST depS, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* neverready never listens on NEVER_READY_TCP_PORT at all -- its
	 * service's probe must time out (1s) without blocking boot forever,
	 * and afterNeverReady (depends on it) must still autostart anyway
	 * (best-effort, not a hard failure). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"neverready\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"],"
	                       "\"on_exit\":\"stop\",\"ready\":{\"tcp_port\":" STR(NEVER_READY_TCP_PORT) ",\"timeout_seconds\":1}}],"
	                       "\"networks\":[\"" READY_NETWORK_NAME "\"],\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST neverready, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"afterNeverReady\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"networks\":[\"" READY_NETWORK_NAME "\"],\"restart\":\"always\","
	                       "\"depends_on\":[\"neverready\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST afterNeverReady, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 6. unless-stopped vs always across a real daemon restart:
	 * stopping a container is the one thing that's supposed to
	 * distinguish them. Both long-sleeping so a stop actually has a
	 * live process to kill. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"stopalways\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"restart\":\"always\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST stopalways, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"stopunless\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
	                       "\"restart\":\"unless-stopped\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST stopunless, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

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
	/* ADR-0180: stop is asynchronous now -- the transient "stopping"
	 * registry entry settles to the definition's own "stopped" view
	 * when the reactor reaps the SIGKILLed child. Poll briefly. */
	{
		int i, settled = 0;

		for (i = 0; i < TEST_SETTLE_ATTEMPTS; i++) {
			if (container_exists(&client, "stopalways") == 0 &&
			    container_exists(&client, "stopunless") == 0) {
				settled = 1;
				break;
			}
			usleep(100 * 1000);
		}
		if (!settled) {
			fprintf(stderr, "FAIL: stopalways/stopunless never settled to stopped\n");
			ok = 0;
		}
	}

	/*
	 * ADR-0181 regression guard: a plain restart:"no" container must be
	 * persisted (it is, since ADR-0181) AND its persisted definition must
	 * survive a reload. Both halves matter: the writer started emitting
	 * restart_policy "no" while parse_persisted_entry() still rejected
	 * that value as corruption, so the daemon refused to load its own
	 * state file ("invalid entry at index 0") and never came back up
	 * after ANY restart once a default container existed -- a total
	 * boot failure that no test covered, because before ADR-0181 a "no"
	 * container had no definition to reload. Created immediately before
	 * the restart below so it's in the file the next boot must parse.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"plainno\",\"image\":\"restarttest\","
	                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST plainno (restart:\"no\"), status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

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

	/*
	 * ADR-0181 regression guard, second half: the restart:"no" container
	 * created just before this restart must still be KNOWN (its
	 * definition parsed cleanly off disk -- the daemon booting at all
	 * already proves the file wasn't rejected) but NOT running, since
	 * "no" is never auto-restarted at boot. GET must therefore be a 200
	 * reporting status "stopped", not a 404 and not a live container.
	 */
	{
		struct cix_response pr;

		memset(&pr, 0, sizeof(pr));
		if (cix_client_request(&client, "GET", "/v1/containers/plainno", NULL, &pr) != 0 ||
		    pr.status != 200) {
			fprintf(stderr,
			        "FAIL: plainno (restart:\"no\") not known after a daemon restart "
			        "(status=%d) -- its persisted definition didn't survive the reload\n",
			        pr.status);
			ok = 0;
		} else if (!str_eq(json_str_field(pr.json, "status"), "stopped")) {
			fprintf(stderr,
			        "FAIL: plainno should be \"stopped\" after restart (never autostarted), got \"%s\"\n",
			        json_str_field(pr.json, "status"));
			ok = 0;
		}
		cix_response_free(&pr);
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

		/*
		 * #413: the probe must actually PASS, not time out.
		 *
		 * Everything above this is satisfied by a probe that never
		 * succeeds: cix-init reports the timeout and then treats the
		 * service as ready, so depS still waits and still starts
		 * second. That is how a tcp_port probe that could never report
		 * ready -- a non-blocking connect returns EINPROGRESS even for
		 * a listening local peer, and only rc == 0 was read as success
		 * -- survived in the gate while failing on every single start
		 * of every container that used one.
		 *
		 * The pair is the assertion, not either half: a positive alone
		 * is consistent with a test that reads nothing, and a negative
		 * alone is consistent with a probe that never passes. depR's
		 * listener must end with NO failure (its 2s-delayed listener is
		 * within the 10s timeout), and neverready's must end with
		 * probe-timeout, because nothing ever listens on its port.
		 */
		{
			char got[64];

			if (wait_svc_fail_reason(&client, "depR", 0, "", 15000, got, sizeof(got)) != 0) {
				fprintf(stderr,
				        "FAIL: depR's listener reports failure \"%s\" -- its tcp_port "
				        "readiness probe did not pass, it timed out and was treated as "
				        "ready (#413)\n",
				        got);
				ok = 0;
			}
			if (wait_svc_fail_reason(&client, "neverready", 0, "probe-timeout", 10000, got,
			                          sizeof(got)) != 0) {
				fprintf(stderr,
				        "FAIL: neverready's service reports failure \"%s\", expected "
				        "probe-timeout -- nothing listens on its port, so a probe that "
				        "reports ready is a false positive\n",
				        got);
				ok = 0;
			}
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
	if (cix_client_request(&client, "GET", "/v1/containers/depR", NULL, &r) != 0 ||
	    r.status != 200 ||
	    json_num_field(json_object_get(service_at(&r, 0), "ready_probe"), "tcp_port") != READY_TCP_PORT) {
		fprintf(stderr, "FAIL: GET depR did not echo its listener's own ready probe\n");
		ok = 0;
	}
	cix_response_free(&r);

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

	/*
	 * #421: a recreate that races an in-flight teardown must SAY so.
	 *
	 * The 409 itself was never in doubt -- the name really is taken
	 * while the previous container tears down (ADR-0180). What was
	 * broken is what the rejection said: v2.57.122 added
	 * name_conflict_msg() to distinguish "still shutting down" from a
	 * genuine clash, and put the call AFTER json_free(root) in the
	 * create path's pre-flight guard. `name` is json_as_string(jname),
	 * a pointer into that tree, so registry_find() compared against
	 * freed heap, found nothing, and reported the flat "already
	 * exists" for a container the daemon knew was tearing down --
	 * measured on 192.168.15.95, 2026-09-12, in 92 of 100 racing
	 * rounds, with GET on the same container reporting
	 * "status":"deleting" at the same instant
	 * (recipes/package/probe-delete-409/2).
	 *
	 * So this asserts the MESSAGE, which is the part that was wrong,
	 * and it has to race to do it -- there is no way to hold a
	 * container in teardown on demand. Up to 40 attempts: the probe
	 * measured a 92% hit rate per attempt, so never once opening the
	 * window is not a plausible outcome, and is reported as a failure
	 * rather than a silent skip, because a test that asserted nothing
	 * is what let this ship.
	 *
	 * The holder's own GET is not required to still say "deleting" by
	 * the time it is read -- an 0s teardown can finish first, which the
	 * probe also saw. The message was formed while the entry existed,
	 * so the message is the invariant.
	 */
	{
		const char *body = "{\"name\":\"racemsg\",\"image\":\"restarttest\","
		                    "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\","
		                    "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],"
		                    "\"restart\":\"no\"}";
		int attempt, saw_conflict = 0;

		for (attempt = 0; attempt < 40 && !saw_conflict; attempt++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers", body, &r) != 0 ||
			    r.status != 201) {
				fprintf(stderr, "FAIL: POST racemsg (attempt %d), status=%d\n", attempt,
				        r.status);
				cix_response_free(&r);
				ok = 0;
				break;
			}
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "DELETE", "/v1/containers/racemsg", NULL, &r) != 0 ||
			    (r.status != 204 && r.status != 404)) {
				fprintf(stderr, "FAIL: DELETE racemsg did not land, status=%d\n", r.status);
				cix_response_free(&r);
				ok = 0;
				break;
			}
			cix_response_free(&r);

			/* Immediately, with nothing in between -- that is the race. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers", body, &r) == 0 &&
			    r.status == 409) {
				const char *err = json_str_field(r.json, "error");

				saw_conflict = 1;
				if (err == NULL || strstr(err, "shutting down") == NULL) {
					fprintf(stderr,
					        "FAIL: a recreate racing an in-flight teardown was refused with "
					        "\"%s\" -- it must say the holder is still shutting down, or the "
					        "reader goes looking for a container `container ls` does not show "
					        "(#421)\n",
					        err != NULL ? err : "(no error field)");
					ok = 0;
				}
			}
			cix_response_free(&r);

			/* Settle before the next attempt, whichever way it went. */
			{
				int waited = 0;

				for (;;) {
					int status = 0;

					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "DELETE", "/v1/containers/racemsg", NULL,
					                        &r) == 0)
						status = r.status;
					cix_response_free(&r);
					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "GET", "/v1/containers/racemsg", NULL, &r) ==
					    0)
						status = r.status;
					cix_response_free(&r);
					if (status == 404 || waited >= 20000)
						break;
					usleep(250000);
					waited += 250;
				}
			}
		}
		if (!saw_conflict && ok) {
			fprintf(stderr,
			        "FAIL: 40 delete+recreate races never once hit the teardown 409, so the "
			        "message this case exists to check was never exercised (#421)\n");
			ok = 0;
		}
	}

	/* cleanup -- enumerate rather than name, see test_cleanup.h */
	if (test_cleanup_containers_and_network(&client, READY_NETWORK_NAME) != 0)
		ok = 0;
	cix_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (second instance)\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "CONTAINER RESTART RESULT: PASS\n" : "CONTAINER RESTART RESULT: FAIL\n");
	return ok ? 0 : 1;
}
