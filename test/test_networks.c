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
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7625
#define PORT_ARG "--port=7625"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

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

/* ADR-0180: container delete is asynchronous -- the not-yet-reaped
 * entry still holds its network attachment for a moment, so a network
 * delete straight after a container delete can transiently 409.
 * Settle-poll the container to 404 first (bounded tight). */
static void wait_container_gone(const struct cix_client *c, const char *name)
{
	char path[128];
	struct cix_response r;
	int i;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < 50; i++) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 404) {
			cix_response_free(&r);
			return;
		}
		cix_response_free(&r);
		usleep(100 * 1000);
	}
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/networkstest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/networkstest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}

	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;

	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	/* 1. create two distinct, non-overlapping networks. No --gateway=
	 * means the new default: pure L2, no host-owned address at all
	 * (ADR-0037) -- has_address false, gateway null. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"neta\",\"subnet\":\"172.40.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST neta, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jhas_gw = json_object_get(r.json, "has_address");
		const struct json_value *jgw = json_object_get(r.json, "address");

		if (jhas_gw == NULL || jhas_gw->type != JSON_BOOL || jhas_gw->u.boolean ||
		    jgw == NULL || jgw->type != JSON_NULL) {
			fprintf(stderr, "FAIL: neta expected has_address=false, address=null (new default)\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"netb\",\"subnet\":\"172.41.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST netb, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. GET list shows both; GET one returns it */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/networks", NULL, &r) != 0 || r.status != 200) {
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
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/networks/neta", NULL, &r) != 0 ||
	    r.status != 200 || !str_eq(json_str_field(r.json, "subnet"), "172.40.0.0")) {
		fprintf(stderr, "FAIL: GET /v1/networks/neta, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. validation */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"neta\",\"subnet\":\"172.42.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate network name expected 409, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"netc\",\"subnet\":\"172.43.0.5\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: misaligned subnet expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"netd\",\"subnet\":\"172.40.0.0\",\"prefix_len\":25}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: overlapping subnet expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"nete\",\"subnet\":\"172.44.0.0\",\"prefix_len\":31}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: prefix_len 31 expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 4. delete an unused network -> 204, bridge actually gone */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/networks/netb", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE netb, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (if_nametoindex("netb") != 0) {
		fprintf(stderr, "FAIL: netb interface still exists after delete\n");
		ok = 0;
	}

	/* 5. a container attached to neta blocks its deletion; removing the
	 * container unblocks it */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c1\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],\"networks\":[\"neta\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST c1, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * Issue #26: the switch panel's own data. The port list comes from
	 * the kernel's view of the bridge, so a running container has to
	 * show up as a real port -- attributed to itself, carrying its own
	 * IP, with counters read from the port's own side.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/networks/neta/ports", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: #26 GET neta/ports, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *ports = json_object_get(r.json, "ports");
		int found = 0;
		size_t pi;

		if (ports == NULL || ports->type != JSON_ARRAY || ports->u.array.count == 0) {
			fprintf(stderr, "FAIL: #26 a running container is not a port on its own bridge\n");
			ok = 0;
		} else {
			for (pi = 0; pi < ports->u.array.count; pi++) {
				const struct json_value *pv = ports->u.array.items[pi];

				if (!str_eq(json_str_field(pv, "container"), "c1"))
					continue;
				found = 1;
				if (!str_eq(json_str_field(pv, "kind"), "container")) {
					fprintf(stderr, "FAIL: #26 c1's port is not kind=container\n");
					ok = 0;
				}
				if (json_str_field(pv, "ip") == NULL) {
					fprintf(stderr, "FAIL: #26 c1's port carries no IP\n");
					ok = 0;
				}
				if (json_object_get(pv, "rx_bytes") == NULL ||
				    json_object_get(pv, "tx_bytes") == NULL) {
					fprintf(stderr, "FAIL: #26 c1's port carries no counters\n");
					ok = 0;
				}
				/* vlan_id belongs to an uplink; a container port must
				 * report null rather than a number that means nothing. */
				if (json_object_get(pv, "vlan_id") == NULL ||
				    json_object_get(pv, "vlan_id")->type != JSON_NULL) {
					fprintf(stderr, "FAIL: #26 a container port reported a vlan_id\n");
					ok = 0;
				}
			}
			if (!found) {
				fprintf(stderr, "FAIL: #26 no port on neta is attributed to c1\n");
				ok = 0;
			}
		}
	}
	cix_response_free(&r);

	/* A network that does not exist is a 404 here too -- the ports of
	 * nothing is not an empty list. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/networks/nosuchnet/ports", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: #26 ports of an unknown network should 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/networks/neta", NULL, &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: DELETE in-use neta expected 409, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/c1", NULL, &r);
	cix_response_free(&r);
	wait_container_gone(&client, "c1");

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/networks/neta", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE neta (now unused) expected 204, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 5.5. explicit, operator-chosen IP override on a container's
	 * network attachment (Phase 12 part 5b) -- request an address
	 * instead of letting network_alloc_ip() pick one. This network
	 * requests an explicit --address= (ADR-0037's operator-chosen
	 * address path, renamed by ADR-0067, exercised end-to-end here)
	 * specifically so the "reserved address rejected" case below
	 * still has a reserved address to reject -- an address-less
	 * network (the new default, see neta above) reserves nothing
	 * beyond the network/broadcast addresses themselves. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"netip\",\"subnet\":\"172.46.0.0\",\"prefix_len\":24,"
	                       "\"address\":\"172.46.0.1\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST netip, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jhas_gw = json_object_get(r.json, "has_address");

		if (jhas_gw == NULL || jhas_gw->type != JSON_BOOL || !jhas_gw->u.boolean ||
		    !str_eq(json_str_field(r.json, "address"), "172.46.0.1")) {
			fprintf(stderr, "FAIL: netip expected has_address=true, address=172.46.0.1\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* explicit, valid IP is honored, not auto-allocated */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
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
	cix_response_free(&r);

	/* the reserved gateway address is rejected */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cgw\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
	                       "\"networks\":[{\"name\":\"netip\",\"ip\":\"172.46.0.1\"}]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: explicit gateway ip expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* an address outside the subnet's usable range is rejected */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"coor\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
	                       "\"networks\":[{\"name\":\"netip\",\"ip\":\"172.99.0.5\"}]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: out-of-subnet ip expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* an already-assigned address is rejected (c2 above holds .42) */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cdup\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
	                       "\"networks\":[{\"name\":\"netip\",\"ip\":\"172.46.0.42\"}]}",
	                       &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: already-taken ip expected 409, got %d\n", r.status);
		ok = 0;
	} else if (r.body == NULL || strstr(r.body, "c2") == NULL) {
		/*
		 * Issue #148: the rejection must NAME the container holding
		 * the address. The old message said "assigned to a running
		 * container" and nothing more, which is actively misleading
		 * after a delete: deletion is asynchronous, so the holder can
		 * be one already torn down and absent from `container ls`,
		 * and the operator goes looking for a conflict that does not
		 * exist. Cost a real debugging detour on a live host.
		 */
		fprintf(stderr,
		        "FAIL: the 409 should name the holding container (c2); body was: %s\n",
		        r.body != NULL ? r.body : "(null)");
		ok = 0;
	}
	cix_response_free(&r);

	/* cleanup: c2 is holding netip, must go before the network can be deleted */
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/c2", NULL, &r);
	cix_response_free(&r);
	wait_container_gone(&client, "c2");

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/networks/netip", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE netip, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 6. restart-survival: create a network, restart the daemon,
	 * confirm it's still there (both in the API and as a real bridge)
	 * without a second create call -- the actual point of persistence. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"persisted\",\"subnet\":\"172.45.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST persisted, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

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
	if (cix_client_request(&client, "GET", "/v1/networks/persisted", NULL, &r) != 0 ||
	    r.status != 200 || !str_eq(json_str_field(r.json, "subnet"), "172.45.0.0")) {
		fprintf(stderr, "FAIL: restarted daemon forgot 'persisted' network, status=%d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (if_nametoindex("persisted") == 0) {
		fprintf(stderr, "FAIL: 'persisted' bridge does not exist after restart\n");
		ok = 0;
	}

	/* 7. migration (ADR-0037): a network persisted by code before
	 * gateways became optional has no "has_address" key in its JSON at
	 * all -- that specific absence must still load as today's exact
	 * legacy behavior (has_address true, gateway = base|1), never
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

		snprintf(state_path, sizeof(state_path), "%s/state/networks.json", g_data_dir);
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
	if (cix_client_request(&client, "GET", "/v1/networks/gwmigrate37", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET migrated old-format network, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *jhas_gw = json_object_get(r.json, "has_address");

		if (jhas_gw == NULL || jhas_gw->type != JSON_BOOL || !jhas_gw->u.boolean ||
		    !str_eq(json_str_field(r.json, "address"), "172.47.0.1")) {
			fprintf(stderr,
			        "FAIL: old-format entry expected has_address=true, address=172.47.0.1 "
			        "(today's exact legacy behavior)\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/networks/gwmigrate37", NULL, &r);
	cix_response_free(&r);

	/* issue #70: a per-network auto-allocation window -- a container
	 * with no explicit ip gets an address inside [alloc_start,
	 * alloc_end], never .1/.2. An EXPLICIT ip outside the window is
	 * still honored (the window constrains auto-alloc only). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"allocwin\",\"subnet\":\"172.29.0.0\",\"prefix_len\":24,"
	                       "\"alloc_start\":\"172.29.0.100\",\"alloc_end\":\"172.29.0.109\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create network with alloc window, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"awc1\",\"image\":\"networkstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
	                       "\"networks\":[\"allocwin\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create container on allocwin, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *nets = json_object_get(r.json, "networks");
		const char *ip = (nets != NULL && nets->type == JSON_ARRAY && nets->u.array.count > 0)
		                     ? json_as_string(json_object_get(nets->u.array.items[0], "ip"))
		                     : NULL;
		/* first free host-part in the window is .100 */
		if (ip == NULL || strcmp(ip, "172.29.0.100") != 0) {
			fprintf(stderr, "FAIL: auto-alloc ignored the window, got ip=%s\n",
			        ip != NULL ? ip : "(null)");
			ok = 0;
		}
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/awc1", NULL, &r);
	cix_response_free(&r);
	wait_container_gone(&client, "awc1");
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/networks/allocwin", NULL, &r);
	cix_response_free(&r);

	/*
	 * Issue #137: a network BRIDGED to a physical interface with no
	 * declared pool must REFUSE to auto-allocate -- a 409, rather than
	 * what actually happened, which was handing out .2 and .3 on the
	 * operator's real LAN inside a range they had reserved.
	 *
	 * The bridged state is seeded into the persisted network list
	 * rather than created through the API, deliberately: attaching an
	 * interface requires a REAL hardware NIC (enumerate_net_one()
	 * excludes everything under /sys/devices/virtual/net, so a dummy
	 * or veth is correctly refused), and a test must not enslave this
	 * machine's actual uplink. Seeding is the same fixture technique
	 * test_esp.c uses for pkg state, and it exercises exactly the
	 * field the refusal keys on: interface_count > 0 with no pool.
	 */
	{
		char state_path[PATH_MAX];
		FILE *sf;
		pid_t alloc_pid;

		if (stop_daemon(daemon_pid) != 0)
			fprintf(stderr, "warning: daemon did not stop cleanly before the #137 fixture\n");

		snprintf(state_path, sizeof(state_path), "%s/state/networks.json", g_data_dir);
		/*
		 * APPEND, never overwrite: this file already holds networks
		 * earlier assertions in this test depend on (notably
		 * "persisted", whose whole point is surviving a restart).
		 * Rewriting it wholesale silently destroyed them and made an
		 * unrelated assertion fail -- a good reminder that a fixture
		 * sharing real state has to preserve it.
		 */
		{
			char *cur = NULL;
			long curlen = 0;
			FILE *rf = fopen(state_path, "rb");

			if (rf != NULL) {
				fseek(rf, 0, SEEK_END);
				curlen = ftell(rf);
				fseek(rf, 0, SEEK_SET);
				cur = malloc((size_t)curlen + 1);
				if (cur != NULL && fread(cur, 1, (size_t)curlen, rf) == (size_t)curlen)
					cur[curlen] = '\0';
				else {
					free(cur);
					cur = NULL;
				}
				fclose(rf);
			}
			/* trim the closing bracket so the two fixtures can be
			 * appended to whatever is already there */
			if (cur != NULL) {
				char *close_bracket = strrchr(cur, ']');

				if (close_bracket != NULL)
					*close_bracket = '\0';
			}
			sf = fopen(state_path, "w");
			if (sf != NULL && cur != NULL && cur[0] != '\0')
				fprintf(sf, "%s,", cur);
			else if (sf != NULL)
				fprintf(sf, "[");
			free(cur);
		}
		if (sf == NULL) {
			fprintf(stderr, "FAIL: cannot seed %s\n", state_path);
			ok = 0;
		} else {
			/* one bridged network with NO pool, one with a pool -- same
			 * enslaved interface name, so the only difference between
			 * them is the thing under test. */
			fprintf(sf,
			        "{\"name\":\"bridgednopool\",\"subnet\":\"172.31.7.0\",\"prefix_len\":24,"
			        "\"has_address\":false,\"is_management\":false,"
			        "\"interfaces\":[{\"ifname\":\"cixfake0\",\"vlan_id\":0}]},"
			        "{\"name\":\"bridgedpool\",\"subnet\":\"172.31.8.0\",\"prefix_len\":24,"
			        "\"has_address\":false,\"is_management\":false,"
			        "\"alloc_start_host\":101,\"alloc_end_host\":109,"
			        "\"interfaces\":[{\"ifname\":\"cixfake0\",\"vlan_id\":0}]}]\n");
			fclose(sf);
		}

		alloc_pid = start_daemon();
		if (alloc_pid < 0 || wait_for_daemon(&client, 100) != 0) {
			fprintf(stderr, "FAIL: daemon did not restart for the #137 fixture\n");
			ok = 0;
		} else {
			/* 1. bridged + no pool -> auto-allocation REFUSED with 409 */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers",
			                       "{\"name\":\"c137a\",\"image\":\"networkstest\","
			                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
			                       "\"networks\":[\"bridgednopool\"]}",
			                       &r) == 0) {
				if (r.status != 409) {
					fprintf(stderr,
					        "FAIL: auto-alloc on a bridged pool-less network must be refused "
					        "with 409, got %d\n",
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);
			}

			/* 2. the SAME network still honours an explicit address --
			 * the refusal is about guessing, never about operator intent */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers",
			                       "{\"name\":\"c137b\",\"image\":\"networkstest\","
			                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
			                       "\"networks\":[{\"name\":\"bridgednopool\","
			                       "\"ip\":\"172.31.7.55\"}]}",
			                       &r) == 0) {
				if (r.status != 201) {
					fprintf(stderr,
					        "FAIL: an explicit ip must still be honoured on a bridged "
					        "pool-less network, got %d\n",
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);
				memset(&r, 0, sizeof(r));
				cix_client_request(&client, "DELETE", "/v1/containers/c137b", NULL, &r);
				cix_response_free(&r);
				wait_container_gone(&client, "c137b");
			}

			/* 3. declaring a pool re-enables auto-allocation, inside it */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/containers",
			                       "{\"name\":\"c137c\",\"image\":\"networkstest\","
			                       "\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"],"
			                       "\"networks\":[\"bridgedpool\"]}",
			                       &r) == 0) {
				const struct json_value *nets =
				    (r.json != NULL) ? json_object_get(r.json, "networks") : NULL;
				const char *ip =
				    (nets != NULL && nets->type == JSON_ARRAY && nets->u.array.count > 0)
				        ? json_as_string(json_object_get(nets->u.array.items[0], "ip"))
				        : NULL;

				if (r.status != 201 || ip == NULL || strcmp(ip, "172.31.8.101") != 0) {
					fprintf(stderr,
					        "FAIL: with a pool declared, auto-alloc must resume inside it "
					        "(status=%d ip=%s)\n",
					        r.status, ip != NULL ? ip : "(null)");
					ok = 0;
				}
				cix_response_free(&r);
				memset(&r, 0, sizeof(r));
				cix_client_request(&client, "DELETE", "/v1/containers/c137c", NULL, &r);
				cix_response_free(&r);
				wait_container_gone(&client, "c137c");
			}
		}
		/*
		 * Delete the seeded networks through the API before moving on:
		 * a network is a REAL kernel bridge that outlives this process,
		 * so a fixture that seeds two of them and walks away leaves
		 * debris behind. That debris is not harmless -- a leftover
		 * bridge makes the next run's create fail with a 500 that
		 * looks nothing like its actual cause, which is precisely the
		 * trap this test just fell into.
		 */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/networks/bridgednopool", NULL, &r);
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/networks/bridgedpool", NULL, &r);
		cix_response_free(&r);

		daemon_pid = alloc_pid;
	}

	/* cleanup */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/networks/persisted", NULL, &r) != 0 ||
	    r.status != 204)
		fprintf(stderr, "warning: could not clean up 'persisted' network, status=%d\n", r.status);
	cix_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM (second instance)\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "NETWORKS RESULT: PASS\n" : "NETWORKS RESULT: FAIL\n");
	return ok ? 0 : 1;
}
