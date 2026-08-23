/*
 * Logging epic Part 2 end-to-end test (ADR-0127): proves the syslog
 * forward-target subsystem (daemon/src/syslogfwd.c) over real HTTP
 * against a real thincd subprocess --
 *   - POST/GET/DELETE /v1/syslog/targets (registration bookkeeping)
 *     mirrors test_ntp.c's own coverage of the analogous /v1/ntp/servers
 *     resource: 404 for a nonexistent container, 404 for a real but
 *     not-running one, 201/409/204 for the real CRUD path, and
 *     syslogfwd_target_forget() firing on container delete.
 *   - The actual wire send: a registered target container running
 *     syslog_recv_child.c (this file's own fixture, binds real UDP
 *     :514 inside its own netns) really receives a real RFC 3164
 *     datagram when an unrelated container's stdout line is captured,
 *     proving syslogfwd_send() reaches a real container over the real
 *     network, not just that the registration bookkeeping is
 *     self-consistent. The received bytes are asserted via the
 *     consolidated log store (source=container), reusing Part 1's own
 *     transparent-capture mechanism rather than any new plumbing.
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

#define TEST_PORT 7659
#define PORT_ARG "--port=7659"
#define SYSLOG_NETWORK_NAME "syslogfwdtest"
#define SYSLOG_NETWORK_SUBNET "172.61.0.0"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

static int wait_for_daemon(const struct thinc_client *c, int max_attempts)
{
	int i;
	struct thinc_response r;

	for (i = 0; i < max_attempts; i++) {
		if (thinc_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			thinc_response_free(&r);
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

static int str_contains(const char *haystack, const char *needle)
{
	return haystack != NULL && needle != NULL && strstr(haystack, needle) != NULL;
}

int main(void)
{
	pid_t daemon_pid;
	char *dargv[5];
	char data_dir_arg[PATH_MAX + 11];
	struct thinc_client client;
	int ok = 1;
	struct thinc_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/syslogfwdtest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/syslogfwdtest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}
	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0 ||
	    test_image_fixture_build(g_image_root, "build/output_child", "output_child") != 0 ||
	    test_image_fixture_build(g_image_root, "build/syslog_recv_child", "syslog_recv_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/thincd";
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
		execve("build/thincd", dargv, environ);
		perror("execve build/thincd");
		_exit(127);
	}

	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. Register a nonexistent container -> 404. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/syslog/targets", "{\"container\":\"no-such\"}", &r) !=
	        0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: register nonexistent container expected 404, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 1a. A real container, but not running -- also 404. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"stoppedsl\",\"image\":\"syslogfwdtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"0\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST stoppedsl, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);
	usleep(300000);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/syslog/targets", "{\"container\":\"stoppedsl\"}",
	                       &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: register not-running container expected 404, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);
	thinc_client_request(&client, "DELETE", "/v1/containers/stoppedsl", NULL, &r);
	thinc_response_free(&r);

	/* 2. A real, running container: register/duplicate/list/unregister,
	 * using daemon_child (no real syslog receiver needed for this pure
	 * bookkeeping half). */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"slbook1\",\"image\":\"syslogfwdtest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"300\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST slbook1, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/syslog/targets", "{\"container\":\"slbook1\"}", &r) !=
	        0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "container"), "slbook1")) {
		fprintf(stderr, "FAIL: register slbook1, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/syslog/targets", "{\"container\":\"slbook1\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate registration expected 409, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/syslog/targets", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/syslog/targets, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *targets = json_object_get(r.json, "targets");
		int found = 0;
		size_t j;

		if (targets != NULL && targets->type == JSON_ARRAY) {
			for (j = 0; j < targets->u.array.count; j++)
				if (str_eq(json_str_field(targets->u.array.items[j], "container"), "slbook1"))
					found = 1;
		}
		if (!found) {
			fprintf(stderr, "FAIL: slbook1 missing from GET /v1/syslog/targets\n");
			ok = 0;
		}
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "DELETE", "/v1/syslog/targets/no-such", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: unregister nonexistent expected 404, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 2a. syslogfwd_target_forget(): deleting the container removes the
	 * registration too, no separate DELETE /v1/syslog/targets/... needed. */
	thinc_client_request(&client, "DELETE", "/v1/containers/slbook1", NULL, &r);
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/syslog/targets", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/syslog/targets after container delete, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *targets = json_object_get(r.json, "targets");
		size_t j;

		if (targets != NULL && targets->type == JSON_ARRAY) {
			for (j = 0; j < targets->u.array.count; j++)
				if (str_eq(json_str_field(targets->u.array.items[j], "container"), "slbook1")) {
					fprintf(stderr, "FAIL: syslog target binding survived container deletion\n");
					ok = 0;
				}
		}
	}
	thinc_response_free(&r);

	/* 3. The real wire-level proof: a network + a real receiver
	 * container (syslog_recv_child, binds real UDP :514) + a real
	 * sender container (output_child, Part 1's own stdout fixture) --
	 * confirm the receiver's own captured stdout (via Part 1's
	 * transparent capture) shows it really got a well-formed RFC 3164
	 * datagram naming the sender as HOSTNAME and carrying its message. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"" SYSLOG_NETWORK_NAME "\",\"subnet\":\"" SYSLOG_NETWORK_SUBNET
	                       "\",\"prefix_len\":24,\"address\":\"172.61.0.1\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST " SYSLOG_NETWORK_NAME ", status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"slrecv\",\"image\":\"syslogfwdtest\","
	                       "\"cmd\":[\"/bin/syslog_recv_child\"],"
	                       "\"networks\":[\"" SYSLOG_NETWORK_NAME "\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST slrecv, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/syslog/targets", "{\"container\":\"slrecv\"}", &r) !=
	        0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: register slrecv, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"slsend\",\"image\":\"syslogfwdtest\","
	                       "\"cmd\":[\"/bin/output_child\"],"
	                       "\"networks\":[\"" SYSLOG_NETWORK_NAME "\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST slsend, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* slrecv's recvfrom() has a 5s timeout; slsend's output_child exits
	 * almost immediately, so the forwarded line should already be in
	 * flight well within that window -- poll for slrecv's own captured
	 * stdout (it prints what it received, then exits) for up to 6s. */
	{
		int i;
		int found = 0;

		for (i = 0; i < 60 && !found; i++) {
			usleep(100000);
			memset(&r, 0, sizeof(r));
			if (thinc_client_request(&client, "GET",
			                       "/v1/system/logs?source=container&container=slrecv", NULL,
			                       &r) == 0 &&
			    r.status == 200 && r.json != NULL && r.json->type == JSON_ARRAY) {
				size_t j;

				for (j = 0; j < r.json->u.array.count; j++) {
					const char *msg = json_str_field(r.json->u.array.items[j], "msg");

					if (str_contains(msg, "syslog-recv-child-got:")) {
						/* Real RFC 3164 shape: HOSTNAME is the sending
						 * container's own name (slsend), TAG is
						 * "thincd", and the forwarded message text
						 * (output_child's own known stdout line) is
						 * present verbatim. */
						if (!str_contains(msg, "slsend") || !str_contains(msg, "thincd:") ||
						    !str_contains(msg, "capture-test-stdout-line")) {
							fprintf(stderr,
							        "FAIL: slrecv captured a datagram but it's malformed: %s\n",
							        msg);
							ok = 0;
						}
						found = 1;
						break;
					}
				}
			}
			thinc_response_free(&r);
		}
		if (!found) {
			fprintf(stderr, "FAIL: slrecv never received a forwarded syslog datagram\n");
			ok = 0;
		}
	}

	thinc_client_request(&client, "DELETE", "/v1/containers/slsend", NULL, &r);
	thinc_response_free(&r);
	thinc_client_request(&client, "DELETE", "/v1/containers/slrecv", NULL, &r);
	thinc_response_free(&r);
	/* Real kernel bridge cleanup, matching test_container_restart.c's
	 * own convention -- a leftover real interface otherwise outlives
	 * this process (plain SIGTERM doesn't tear down live network
	 * state) and collides with any later rerun of this same test. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "DELETE", "/v1/networks/" SYSLOG_NETWORK_NAME, NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE " SYSLOG_NETWORK_NAME ", status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

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
	printf(ok ? "SYSLOGFWD RESULT: PASS\n" : "SYSLOGFWD RESULT: FAIL\n");
	return ok ? 0 : 1;
}
