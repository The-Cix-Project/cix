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

#include <limits.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7625
#define PORT_ARG "--port=7625"

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

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/images/networkstest/rootfs", g_data_dir);

	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
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

	/* 1. create two distinct, non-overlapping networks. No --gateway=
	 * means the new default: pure L2, no host-owned address at all
	 * (ADR-0037) -- has_gateway false, gateway null. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"neta\",\"subnet\":\"172.40.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST neta, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jhas_gw = json_object_get(r.json, "has_gateway");
		const struct json_value *jgw = json_object_get(r.json, "gateway");

		if (jhas_gw == NULL || jhas_gw->type != JSON_BOOL || jhas_gw->u.boolean ||
		    jgw == NULL || jgw->type != JSON_NULL) {
			fprintf(stderr, "FAIL: neta expected has_gateway=false, gateway=null (new default)\n");
			ok = 0;
		}
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
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],\"networks\":[\"neta\"]}",
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

	/* 5.5. explicit, operator-chosen IP override on a container's
	 * network attachment (Phase 12 part 5b) -- request an address
	 * instead of letting network_alloc_ip() pick one. This network
	 * requests an explicit --gateway= (ADR-0037's operator-chosen
	 * gateway path, exercised end-to-end here) specifically so the
	 * "reserved gateway address rejected" case below still has a
	 * reserved address to reject -- a gateway-less network (the new
	 * default, see neta above) reserves nothing beyond the network/
	 * broadcast addresses themselves. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"netip\",\"subnet\":\"172.46.0.0\",\"prefix_len\":24,"
	                       "\"gateway\":\"172.46.0.1\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST netip, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jhas_gw = json_object_get(r.json, "has_gateway");

		if (jhas_gw == NULL || jhas_gw->type != JSON_BOOL || !jhas_gw->u.boolean ||
		    !str_eq(json_str_field(r.json, "gateway"), "172.46.0.1")) {
			fprintf(stderr, "FAIL: netip expected has_gateway=true, gateway=172.46.0.1\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* explicit, valid IP is honored, not auto-allocated */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c2\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
	                       "\"networks\":[{\"name\":\"netip\",\"ip\":\"172.46.0.42\"}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST c2 with explicit ip, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *networks = json_object_get(r.json, "networks");
		const char *got_ip =
		    networks != NULL && networks->type == JSON_ARRAY && networks->u.array.count == 1
		        ? json_str_field(networks->u.array.items[0], "ip")
		        : NULL;

		if (!str_eq(got_ip, "172.46.0.42")) {
			fprintf(stderr, "FAIL: c2 expected ip=172.46.0.42, got %s\n",
			        got_ip != NULL ? got_ip : "(null)");
			ok = 0;
		}
	}
	kx_response_free(&r);

	/* the reserved gateway address is rejected */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cgw\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
	                       "\"networks\":[{\"name\":\"netip\",\"ip\":\"172.46.0.1\"}]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: explicit gateway ip expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* an address outside the subnet's usable range is rejected */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"coor\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
	                       "\"networks\":[{\"name\":\"netip\",\"ip\":\"172.99.0.5\"}]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: out-of-subnet ip expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* an already-assigned address is rejected (c2 above holds .42) */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cdup\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
	                       "\"networks\":[{\"name\":\"netip\",\"ip\":\"172.46.0.42\"}]}",
	                       &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: already-taken ip expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* cleanup: c2 is holding netip, must go before the network can be deleted */
	memset(&r, 0, sizeof(r));
	kx_client_request(&client, "DELETE", "/v1/containers/c2", NULL, &r);
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/networks/netip", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE netip, status=%d\n", r.status);
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

	/* 7. migration (ADR-0037): a network persisted by code before
	 * gateways became optional has no "has_gateway" key in its JSON at
	 * all -- that specific absence must still load as today's exact
	 * legacy behavior (has_gateway true, gateway = base|1), never
	 * silently demoted to gateway-less. Simulated directly by splicing
	 * a hand-written old-format entry into the real persisted state
	 * file while the daemon is stopped, since the current API can no
	 * longer produce one itself. */
	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (before migration splice)\n");
		ok = 0;
	}
	{
		char state_path[PATH_MAX];
		static const char *const old_format_entry =
		    "{\"name\":\"gwmigrate37\",\"subnet\":\"172.47.0.0\",\"prefix_len\":24}";
		FILE *f;

		snprintf(state_path, sizeof(state_path), "%s/networks.json", g_data_dir);
		f = fopen(state_path, "r+");
		char buf[65536];
		size_t len = 0;
		char *close_bracket;

		if (f == NULL) {
			fprintf(stderr, "FAIL: could not open %s for migration splice\n", state_path);
			ok = 0;
		} else {
			len = fread(buf, 1, sizeof(buf) - 1, f);
			buf[len] = '\0';
			close_bracket = strrchr(buf, ']');
			if (close_bracket == NULL) {
				fprintf(stderr, "FAIL: %s has no closing ']' to splice before\n", state_path);
				ok = 0;
			} else {
				/* Splice ",<old_format_entry>" right before the array's
				 * closing bracket -- valid whether the array already had
				 * entries (a leading ',' before ours) or was genuinely
				 * empty ("[]", the same splice point works since we're
				 * inserting immediately before ']' either way, and an
				 * empty array's own '[' is never itself ']'). */
				size_t prefix_len = (size_t)(close_bracket - buf);
				int need_comma = prefix_len > 0 && buf[prefix_len - 1] != '[';

				fseek(f, 0, SEEK_SET);
				fwrite(buf, 1, prefix_len, f);
				if (need_comma)
					fputc(',', f);
				fputs(old_format_entry, f);
				fputc(']', f);
				if (ftruncate(fileno(f), (long)(prefix_len + (need_comma ? 1 : 0) +
				                                 strlen(old_format_entry) + 1)) != 0) {
					fprintf(stderr, "FAIL: could not truncate %s after migration splice\n",
					        state_path);
					ok = 0;
				}
			}
			fclose(f);
		}
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections after migration splice\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/networks/gwmigrate37", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET migrated old-format network, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jhas_gw = json_object_get(r.json, "has_gateway");

		if (jhas_gw == NULL || jhas_gw->type != JSON_BOOL || !jhas_gw->u.boolean ||
		    !str_eq(json_str_field(r.json, "gateway"), "172.47.0.1")) {
			fprintf(stderr,
			        "FAIL: old-format entry expected has_gateway=true, gateway=172.47.0.1 "
			        "(today's exact legacy behavior)\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	kx_client_request(&client, "DELETE", "/v1/networks/gwmigrate37", NULL, &r);
	kx_response_free(&r);

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

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "NETWORKS RESULT: PASS\n" : "NETWORKS RESULT: FAIL\n");
	return ok ? 0 : 1;
}
