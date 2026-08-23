/*
 * ADR-0067 Part 3 end-to-end test: proves POST/DELETE /v1/system/routes
 * (rtnl_route_add_ipv4()/the new rtnl_route_del_ipv4(), daemon/src/
 * main.c's handle_route_add()/handle_route_del()) actually mutate the
 * host's own real kernel IPv4 routing table, not just parse JSON --
 * verified via a real add+dump+delete+dump round-trip against a live
 * thincd, cross-checked against GET /v1/system/routes (ADR-0066)
 * before ever trusting the mutation succeeded.
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

#define TEST_PORT 7638
#define PORT_ARG "--port=7638"

/* A real, currently-unused test subnet -- routes into it are added and
 * removed purely at the kernel level (no bridge/interface needed,
 * matching what rtnl_route_add_ipv4()/rtnl_route_del_ipv4() actually
 * support: destination + gateway, no RTA_OIF). Chosen well clear of
 * any subnet another test in this suite creates, so a parallel test
 * run can never collide with it. */
#define TEST_DEST "203.0.113.0"
#define TEST_GATEWAY "127.0.0.1" /* always resolvable/on-link in any sandbox -- loopback */

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

/* Re-fetches GET /v1/system/routes and reports whether an entry with
 * this exact dest/prefix appears -- the real, kernel-backed proof a
 * mutation actually took effect, not just that the daemon returned the
 * status code it was supposed to. */
static int route_dump_contains(const struct thinc_client *c, const char *dest, int prefix)
{
	struct thinc_response r;
	const struct json_value *routes;
	size_t i;
	int found = 0;

	memset(&r, 0, sizeof(r));
	if (thinc_client_request(c, "GET", "/v1/system/routes", NULL, &r) != 0 || r.status != 200) {
		thinc_response_free(&r);
		return -1;
	}

	routes = json_object_get(r.json, "routes");
	if (routes != NULL && routes->type == JSON_ARRAY) {
		for (i = 0; i < routes->u.array.count; i++) {
			const struct json_value *entry = routes->u.array.items[i];
			const struct json_value *jdest = json_object_get(entry, "dest");
			const struct json_value *jprefix = json_object_get(entry, "prefix");
			const char *dest_str = json_as_string(jdest);

			if (dest_str != NULL && strcmp(dest_str, dest) == 0 &&
			    (int)json_as_number(jprefix) == prefix) {
				found = 1;
				break;
			}
		}
	}
	thinc_response_free(&r);
	return found;
}

int main(void)
{
	pid_t daemon_pid;
	struct thinc_client client;
	int ok = 1;
	struct thinc_response r;
	int present;

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

	/* 1. Confirm the test route is genuinely absent before we ever add
	 * it -- a false positive later would otherwise be indistinguishable
	 * from a real add. */
	present = route_dump_contains(&client, TEST_DEST, 24);
	if (present != 0) {
		fprintf(stderr, "FAIL: test route already present before add (present=%d)\n", present);
		ok = 0;
	}

	/* 2. Add it via POST, expect 204. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "POST", "/v1/system/routes",
	                              "{\"dest\":\"" TEST_DEST "\",\"prefix\":24,\"gateway\":\"" TEST_GATEWAY
	                              "\"}",
	                              &r) != 0 ||
	           r.status != 204)) {
		fprintf(stderr, "FAIL: POST /v1/system/routes, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 3. Real, kernel-backed proof it's there: re-dump and find it. */
	if (ok) {
		present = route_dump_contains(&client, TEST_DEST, 24);
		if (present != 1) {
			fprintf(stderr, "FAIL: added route not found in GET /v1/system/routes "
			                "(present=%d)\n",
			        present);
			ok = 0;
		}
	}

	/* 4. Adding the exact same route again must fail -- the kernel
	 * itself rejects the duplicate (no NLM_F_REPLACE), and the daemon
	 * must surface that as 400, not silently succeed or crash. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "POST", "/v1/system/routes",
	                              "{\"dest\":\"" TEST_DEST "\",\"prefix\":24,\"gateway\":\"" TEST_GATEWAY
	                              "\"}",
	                              &r) != 0 ||
	           r.status != 400)) {
		fprintf(stderr, "FAIL: duplicate POST /v1/system/routes expected 400, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 5. Malformed body (bad dest) must 400, not crash or silently
	 * install a garbage route. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "POST", "/v1/system/routes",
	                              "{\"dest\":\"not-an-ip\",\"prefix\":24}", &r) != 0 ||
	           r.status != 400)) {
		fprintf(stderr, "FAIL: invalid dest POST expected 400, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 6. Delete it via DELETE, expect 204. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "DELETE", "/v1/system/routes",
	                              "{\"dest\":\"" TEST_DEST "\",\"prefix\":24}", &r) != 0 ||
	           r.status != 204)) {
		fprintf(stderr, "FAIL: DELETE /v1/system/routes, status=%d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	/* 7. Real, kernel-backed proof it's gone: re-dump and confirm it's
	 * absent again. */
	if (ok) {
		present = route_dump_contains(&client, TEST_DEST, 24);
		if (present != 0) {
			fprintf(stderr, "FAIL: deleted route still present in GET /v1/system/routes "
			                "(present=%d)\n",
			        present);
			ok = 0;
		}
	}

	/* 8. Deleting an already-gone route must 404, not crash or silently
	 * report success. */
	memset(&r, 0, sizeof(r));
	if (ok && (thinc_client_request(&client, "DELETE", "/v1/system/routes",
	                              "{\"dest\":\"" TEST_DEST "\",\"prefix\":24}", &r) != 0 ||
	           r.status != 404)) {
		fprintf(stderr, "FAIL: DELETE of already-gone route expected 404, got %d\n", r.status);
		ok = 0;
	}
	thinc_response_free(&r);

	if (ok)
		printf("ROUTES RESULT: PASS\n");
	else
		printf("ROUTES RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
