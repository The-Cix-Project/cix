/*
 * ADR-0160 end-to-end test: the host-level sysctl REST surface, proven
 * against this sandbox's own real /proc/sys -- unlike every other
 * daemon-linked test's persisted state (always a fresh, isolated
 * mkdtemp data dir), a sysctl write is real, live, shared host kernel
 * state with no per-test isolation possible by the nature of the
 * feature itself (ADR-0160's own "fully open, host-level" scope).
 *
 * Every real write in this test is deliberately idempotent -- either
 * writing back the exact value already in effect (net.ipv4.ip_forward,
 * already unconditionally enabled by this daemon's own boot sequence
 * in every single test invocation all session, confirmed safe by
 * having already run hundreds of times) or reading a real tuple-shaped
 * value and writing that identical tuple straight back
 * (net.ipv4.ip_local_port_range) -- so this test never actually
 * changes this sandbox's real behavior, only exercises the real
 * read/write/persist code paths against real kernel state.
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
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7682
#define PORT_ARG "--port=7682"

static char g_data_dir[PATH_MAX];

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/thincd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/thincd", dargv, environ);
		perror("execve build/thincd");
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

int main(void)
{
	pid_t daemon_pid;
	struct thinc_client client;
	int ok = 1;
	struct thinc_response r;
	char forward_value[64] = "";
	char port_range_value[128] = "";

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	thinc_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. GET a real, always-present single-value key. */
	memset(&r, 0, sizeof(r));
	if (thinc_client_request(&client, "GET", "/v1/system/sysctl/net.ipv4.ip_forward", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET net.ipv4.ip_forward: expected 200, got %d\n", r.status);
		ok = 0;
	} else {
		const char *v = json_as_string(json_object_get(r.json, "value"));

		if (v == NULL) {
			fprintf(stderr, "FAIL: GET net.ipv4.ip_forward: value is not a plain string\n");
			ok = 0;
		} else {
			snprintf(forward_value, sizeof(forward_value), "%s", v);
		}
	}
	thinc_response_free(&r);

	/* 2. GET a real, always-present multi-value (tuple) key -- proves
	 * the array-splitting logic against real kernel state. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "GET", "/v1/system/sysctl/net.ipv4.ip_local_port_range",
	                              NULL, &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET net.ipv4.ip_local_port_range: expected 200, got %d\n", r.status);
		ok = 0;
	} else if (ok) {
		const struct json_value *jv = json_object_get(r.json, "value");

		if (jv == NULL || jv->type != JSON_ARRAY || jv->u.array.count != 2) {
			fprintf(stderr,
			        "FAIL: GET net.ipv4.ip_local_port_range: expected a 2-element array\n");
			ok = 0;
		} else {
			const char *lo = json_as_string(jv->u.array.items[0]);
			const char *hi = json_as_string(jv->u.array.items[1]);

			if (lo == NULL || hi == NULL) {
				fprintf(stderr, "FAIL: port range array elements not strings\n");
				ok = 0;
			} else {
				snprintf(port_range_value, sizeof(port_range_value), "%s %s", lo, hi);
			}
		}
	}
	thinc_response_free(&r);

	/* 3. PUT the same single-value key back with its own current
	 * value (idempotent, real write) -- persists by default. */
	memset(&r, 0, sizeof(r));
	if (ok) {
		char body[96];

		snprintf(body, sizeof(body), "{\"value\":\"%s\"}", forward_value);
		if (thinc_client_request(&client, "PUT", "/v1/system/sysctl/net.ipv4.ip_forward", body, &r) !=
		        0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: PUT net.ipv4.ip_forward: expected 200, got %d\n", r.status);
			ok = 0;
		}
	}
	thinc_response_free(&r);

	/* 4. GET /v1/system/sysctl (collection) now shows exactly this
	 * one persisted key. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "GET", "/v1/system/sysctl", NULL, &r) != 0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET /v1/system/sysctl: expected 200, got %d\n", r.status);
		ok = 0;
	} else if (ok) {
		const struct json_value *arr = json_object_get(r.json, "sysctls");
		int found = 0;
		size_t i;

		if (arr == NULL || arr->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: GET /v1/system/sysctl: no sysctls array\n");
			ok = 0;
		} else {
			for (i = 0; i < arr->u.array.count; i++) {
				const char *k = json_as_string(json_object_get(arr->u.array.items[i], "key"));

				if (k != NULL && strcmp(k, "net.ipv4.ip_forward") == 0)
					found = 1;
			}
			if (!found) {
				fprintf(stderr,
				        "FAIL: GET /v1/system/sysctl: net.ipv4.ip_forward not in "
				        "persisted collection after PUT\n");
				ok = 0;
			}
		}
	}
	thinc_response_free(&r);

	/* 5. PUT the tuple key back as an array (its own current value,
	 * still idempotent) with persist:false -- real write, but must
	 * NOT show up in the persisted collection afterward. */
	memset(&r, 0, sizeof(r));
	if (ok && port_range_value[0] != '\0') {
		char lo[32], hi[32], body[160];

		if (sscanf(port_range_value, "%31s %31s", lo, hi) == 2) {
			snprintf(body, sizeof(body), "{\"value\":[\"%s\",\"%s\"],\"persist\":false}", lo, hi);
			if (thinc_client_request(&client, "PUT", "/v1/system/sysctl/net.ipv4.ip_local_port_range",
			                       body, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: PUT net.ipv4.ip_local_port_range (array): expected 200, "
				                "got %d\n",
				        r.status);
				ok = 0;
			}
		}
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "GET", "/v1/system/sysctl", NULL, &r) != 0 ||
	           r.status != 200)) {
		ok = 0;
	} else if (ok) {
		const struct json_value *arr = json_object_get(r.json, "sysctls");
		size_t i;
		int found = 0;

		for (i = 0; arr != NULL && i < arr->u.array.count; i++) {
			const char *k = json_as_string(json_object_get(arr->u.array.items[i], "key"));

			if (k != NULL && strcmp(k, "net.ipv4.ip_local_port_range") == 0)
				found = 1;
		}
		if (found) {
			fprintf(stderr, "FAIL: persist:false entry appeared in the persisted collection "
			                "anyway\n");
			ok = 0;
		}
	}
	thinc_response_free(&r);

	/* 6. DELETE the persisted (from step 3) entry -- removes it from
	 * the collection, does not touch the live value. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "DELETE", "/v1/system/sysctl/net.ipv4.ip_forward", NULL,
	                              &r) != 0 ||
	           r.status != 204)) {
		fprintf(stderr, "FAIL: DELETE net.ipv4.ip_forward: expected 204, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "GET", "/v1/system/sysctl/net.ipv4.ip_forward", NULL, &r) !=
	               0 ||
	           r.status != 200)) {
		fprintf(stderr, "FAIL: GET net.ipv4.ip_forward after DELETE (live value): expected 200, "
		                "got %d\n",
		        r.status);
		ok = 0;
	} else if (ok) {
		const char *v = json_as_string(json_object_get(r.json, "value"));

		if (v == NULL || strcmp(v, forward_value) != 0) {
			fprintf(stderr, "FAIL: live value changed after DELETE -- should be untouched\n");
			ok = 0;
		}
	}
	thinc_response_free(&r);

	/* 7. DELETE again -- no longer persisted, real 404. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "DELETE", "/v1/system/sysctl/net.ipv4.ip_forward", NULL,
	                              &r) != 0 ||
	           r.status != 404)) {
		fprintf(stderr, "FAIL: second DELETE net.ipv4.ip_forward: expected 404, got %d\n",
		        r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 8. Error paths. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "GET", "/v1/system/sysctl/thinc.test.nosuchkey.at.all",
	                              NULL, &r) != 0 ||
	           r.status != 404)) {
		fprintf(stderr, "FAIL: GET nonexistent key: expected 404, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "PUT", "/v1/system/sysctl/net.ipv4.ip_forward", "{}", &r) !=
	               0 ||
	           r.status != 400)) {
		fprintf(stderr, "FAIL: PUT with no value: expected 400, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "GET", "/v1/system/sysctl/bad/key", NULL, &r) != 0 ||
	           r.status != 400)) {
		fprintf(stderr, "FAIL: GET key containing '/': expected 400, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	if (ok)
		printf("SYSCTL RESULT: PASS\n");
	else
		printf("SYSCTL RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
