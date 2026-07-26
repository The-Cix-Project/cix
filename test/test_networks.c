/*
 * Phase 7 part 1 end-to-end test: proves the dynamic network resource
 * (POST/GET/DELETE /v1/networks -- daemon/src/network.c) over real
 * HTTP, including its actual reason for existing: a network's bridge
 * is real, persistent kernel state that must survive a daemon
 * restart, unlike containers (safe to be in-memory-only since they
 * die with the daemon).
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7625
#define PORT_ARG "--port=7625"
#define IMAGE_ROOT "/var/lib/kanxeo/images/networkstest/rootfs"

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

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;

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

	/* 1. create two distinct, non-overlapping networks */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"neta\",\"subnet\":\"172.40.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "gateway"), "172.40.0.1")) {
		fprintf(stderr, "FAIL: POST neta, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"netb\",\"subnet\":\"172.41.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST netb, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. GET list shows both; GET one returns it */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/networks", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/networks\n");
		ok = 0;
	} else {
		const struct json_value *networks = json_object_get(r.json, "networks");
		int found_a = 0, found_b = 0;
		size_t i;

		if (networks != NULL && networks->type == JSON_ARRAY) {
			for (i = 0; i < networks->u.array.count; i++) {
				const char *n = json_str_field(networks->u.array.items[i], "name");

				if (str_eq(n, "neta"))
					found_a = 1;
				if (str_eq(n, "netb"))
					found_b = 1;
			}
		}
		if (!found_a || !found_b) {
			fprintf(stderr, "FAIL: GET /v1/networks missing neta/netb\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/networks/neta", NULL, &r) != 0 ||
	    r.status != 200 || !str_eq(json_str_field(r.json, "subnet"), "172.40.0.0")) {
		fprintf(stderr, "FAIL: GET /v1/networks/neta, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. validation */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"neta\",\"subnet\":\"172.42.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate network name expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"netc\",\"subnet\":\"172.43.0.5\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: misaligned subnet expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"netd\",\"subnet\":\"172.40.0.0\",\"prefix_len\":25}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: overlapping subnet expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"nete\",\"subnet\":\"172.44.0.0\",\"prefix_len\":31}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: prefix_len 31 expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 4. delete an unused network -> 204, bridge actually gone */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/netb", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE netb, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);
	if (if_nametoindex("netb") != 0) {
		fprintf(stderr, "FAIL: netb interface still exists after delete\n");
		ok = 0;
	}

	/* 5. a container attached to neta blocks its deletion; removing the
	 * container unblocks it */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c1\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],\"network\":\"neta\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST c1, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/neta", NULL, &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: DELETE in-use neta expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	kx_client_request(&client, "DELETE", "/v1/containers/c1", NULL, &r);
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/neta", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE neta (now unused) expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 6. restart-survival: create a network, restart the daemon,
	 * confirm it's still there (both in the API and as a real bridge)
	 * without a second create call -- the actual point of persistence. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"persisted\",\"subnet\":\"172.45.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST persisted, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

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

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/networks/persisted", NULL, &r) != 0 ||
	    r.status != 200 || !str_eq(json_str_field(r.json, "subnet"), "172.45.0.0")) {
		fprintf(stderr, "FAIL: restarted daemon forgot 'persisted' network, status=%d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);
	if (if_nametoindex("persisted") == 0) {
		fprintf(stderr, "FAIL: 'persisted' bridge does not exist after restart\n");
		ok = 0;
	}

	/* cleanup */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/persisted", NULL, &r) != 0 ||
	    r.status != 204)
		fprintf(stderr, "warning: could not clean up 'persisted' network, status=%d\n", r.status);
	kx_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (second instance)\n");
		ok = 0;
	}

	printf(ok ? "NETWORKS RESULT: PASS\n" : "NETWORKS RESULT: FAIL\n");
	return ok ? 0 : 1;
}
