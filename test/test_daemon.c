/*
 * Phase 3 end-to-end test: proves the REST daemon actually implements
 * docs/api/openapi.yaml over real HTTP, not just that its internal
 * functions work. Stages one minimal test image, forks and execve's
 * the built daemon on a test port, then drives it via the shared
 * httpclient.c (the same client library cixctl uses -- see
 * ADR-0005/Phase 4 in docs/roadmap/ROADMAP.md), so there is one implementation
 * of "how to talk to the API," not a test-only copy of it.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7621

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

static long json_int_field(const struct json_value *obj, const char *key)
{
	return (long)json_as_number(json_object_get(obj, key));
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static long long json_num_field(const struct json_value *obj, const char *key)
{
	return (long long)json_as_number(json_object_get(obj, key));
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static int poll_until_exited(const struct cix_client *c, const char *name, int max_attempts,
                              struct cix_response *out)
{
	int i;
	char path[128];

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < max_attempts; i++) {
		cix_response_free(out);
		if (cix_client_request(c, "GET", path, NULL, out) != 0)
			return -1;
		if (out->status == 200 && str_eq(json_str_field(out->json, "status"), "exited"))
			return 0;
		usleep(100000);
	}
	return -1;
}

int main(void)
{
	pid_t daemon_pid;
	int ok = 1;
	struct cix_response r;
	struct cix_client client;
	char *dargv[4];
	char data_dir_arg[PATH_MAX + 11];

	memset(&r, 0, sizeof(r));
	cix_client_init(&client, "127.0.0.1", TEST_PORT);

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	/* ADR-0107/0108: a manifest.json is required for POST /v1/containers
	 * to resolve this image's own current version -- see
	 * test_image_fixture_write_manifest()'s own header comment. */
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/test/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/test", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}

	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = "--port=7621";
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	daemon_pid = fork();
	if (daemon_pid < 0) {
		perror("fork");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (daemon_pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
		_exit(127);
	}

	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. health */
	if (cix_client_request(&client, "GET", "/v1/health", NULL, &r) != 0 || r.status != 200 ||
	    !str_eq(json_str_field(r.json, "status"), "ok")) {
		fprintf(stderr, "FAIL: GET /v1/health\n");
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. create c1, exits quickly with code 5 */
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c1\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"5\"]}]}",
	                       &r) != 0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "status"), "running")) {
		fprintf(stderr, "FAIL: POST /v1/containers (c1), status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. list shows it */
	if (cix_client_request(&client, "GET", "/v1/containers", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/containers\n");
		ok = 0;
	} else {
		const struct json_value *containers = json_object_get(r.json, "containers");
		int found = 0;
		size_t i;

		if (containers != NULL && containers->type == JSON_ARRAY) {
			for (i = 0; i < containers->u.array.count; i++) {
				if (str_eq(json_str_field(containers->u.array.items[i], "name"), "c1"))
					found = 1;
			}
		}
		if (!found) {
			fprintf(stderr, "FAIL: c1 missing from GET /v1/containers\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 4. poll until c1 exits, check exit_status */
	if (poll_until_exited(&client, "c1", 50, &r) != 0) {
		fprintf(stderr, "FAIL: c1 never reported exited\n");
		ok = 0;
	} else if (json_int_field(r.json, "exit_status") != 5) {
		fprintf(stderr, "FAIL: c1 exit_status expected 5, got %ld\n",
		        json_int_field(r.json, "exit_status"));
		ok = 0;
	}
	cix_response_free(&r);

	/* 5. create c2, sleeps 30s -- then delete it while running */
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c2\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/containers (c2), status=%d\n", r.status);
		ok = 0;
	}
	{
		long c2_pid = json_int_field(r.json, "pid");
		char proc_path[64];
		struct stat st;

		cix_response_free(&r);

		if (cix_client_request(&client, "DELETE", "/v1/containers/c2", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE /v1/containers/c2, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* ADR-0180: DELETE of a running container is asynchronous --
		 * 204 records the intent + sends the SIGKILL; the process
		 * reap, registry release, and disk cleanup complete from the
		 * reactor. Settle-poll (bounded tight: a SIGKILLed child
		 * taking >5s to fully tear down is a real regression). */
		{
			int i, reaped = 0;

			snprintf(proc_path, sizeof(proc_path), "/proc/%ld", c2_pid);
			for (i = 0; i < 50; i++) {
				struct cix_response gr;

				memset(&gr, 0, sizeof(gr));
				if (cix_client_request(&client, "GET", "/v1/containers/c2", NULL, &gr) == 0 &&
				    gr.status == 404 && stat(proc_path, &st) != 0) {
					reaped = 1;
					cix_response_free(&gr);
					break;
				}
				cix_response_free(&gr);
				usleep(100 * 1000);
			}
			if (!reaped) {
				fprintf(stderr, "FAIL: c2 (pid %ld) never fully torn down after async DELETE\n",
				        c2_pid);
				ok = 0;
			}
		}

		/*
		 * task #738: DELETE must remove c2's own on-disk upper/work/
		 * merged directories, not just the process and registry entry --
		 * deleting a container that was genuinely still RUNNING (this
		 * exact case) is the one that exercises the real hazard the fix
		 * has to get right: unmounting the still-live overlay mount
		 * before recursively removing the directory tree underneath it,
		 * not after.
		 */
		{
			char container_base[PATH_MAX];

			snprintf(container_base, sizeof(container_base), "%s/containers/c2", g_data_dir);
			if (stat(container_base, &st) == 0) {
				fprintf(stderr, "FAIL: c2's own directory %s still exists after DELETE\n",
				        container_base);
				ok = 0;
			} else if (errno != ENOENT) {
				fprintf(stderr, "FAIL: stat(%s) after DELETE: %s\n", container_base,
				        strerror(errno));
				ok = 0;
			}
		}
	}

	/* 6. deleted container is gone */
	if (cix_client_request(&client, "GET", "/v1/containers/c2", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: GET /v1/containers/c2 after delete, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 6.5. issue #68: unknown top-level fields are a 400 naming the
	 * offender, never silently dropped (a typo'd "cpuset" once cost a
	 * live container half its requested limits with zero signal); and
	 * issue #49: cpuset_cpus/disk_quota_bytes now read back in GET
	 * (cpuset live from the real cgroup file; quota from the
	 * creation-time mirror). */
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c49\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"cpuset\":\"0\"}",
	                       &r) != 0 ||
	    r.status != 400 || r.json == NULL ||
	    json_str_field(r.json, "error") == NULL ||
	    strstr(json_str_field(r.json, "error"), "cpuset") == NULL) {
		fprintf(stderr, "FAIL: unknown field should 400 naming it, got status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	/* disk_quota_bytes deliberately not exercised here: this test
	 * env's filesystem has no prjquota support, so requesting one
	 * correctly 500s -- the quota mirror's read-back shares the same
	 * serializer path asserted below and is verified against a real
	 * quota-capable install instead. */
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c49\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"cpuset_cpus\":\"0\"}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST c49 with cpuset, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (cix_client_request(&client, "GET", "/v1/containers/c49", NULL, &r) != 0 || r.status != 200 ||
	    !str_eq(json_str_field(r.json, "cpuset_cpus"), "0") ||
	    json_object_get(r.json, "disk_quota_bytes") == NULL) {
		fprintf(stderr, "FAIL: c49 cpuset_cpus not read back / quota key missing (status=%d)\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);
	/*
	 * Issue #86: a container's cgroup is a LEAF UNDER the workload
	 * parent, and that parent carries the machine-minus-reservation
	 * ceiling. This is what makes the control plane's share a
	 * kernel-enforced remainder rather than a hope -- a real box was
	 * lost to exactly this (four builds saturating a 2-CPU host, cixd
	 * stopped answering, and with no SSH and no shell that is a
	 * hypervisor trip).
	 *
	 * c49 is still alive at this point, so its leaf must be there.
	 */
	{
		struct stat st;
		char leaf[PATH_MAX];

		snprintf(leaf, sizeof(leaf), "/sys/fs/cgroup/cix-workload/c49");
		if (stat(leaf, &st) != 0) {
			fprintf(stderr, "FAIL: #86 container cgroup %s missing -- containers are not under "
			                "the bounded workload parent\n",
			        leaf);
			ok = 0;
		}

		/*
		 * Issue #51: the zswap endpoint's own contract. A successful
		 * apply is deliberately NOT asserted here -- this sandbox
		 * mounts /sys read-only (like every LXC), so writing a kernel
		 * module parameter is impossible regardless of privilege, and
		 * a test that pretended otherwise would only ever be testing
		 * itself. What IS asserted is the part that holds everywhere:
		 * the reported shape, and that a request the kernel could not
		 * honour is refused before it is written.
		 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/zswap", NULL, &r) != 0 ||
		    r.status != 200 || json_object_get(r.json, "supported") == NULL ||
		    json_object_get(r.json, "kernel") == NULL ||
		    json_object_get(r.json, "available_compressors") == NULL) {
			fprintf(stderr, "FAIL: #51 GET zswap, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/zswap",
		                       "{\"max_pool_percent\":0}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: #51 max_pool_percent=0 should 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/zswap",
		                       "{\"max_pool_percent\":101}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: #51 max_pool_percent=101 should 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* The compressor name is written into a sysfs file; anything
		 * that is not a plain algorithm name is refused, never
		 * escaped. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/zswap",
		                       "{\"compressor\":\"lzo\\nY\"}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: #51 a newline in compressor should 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* A compressor this kernel was not built with is refused
		 * rather than written and silently ignored -- 409, since the
		 * request is well-formed and this machine simply cannot do it. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/zswap",
		                       "{\"compressor\":\"nosuchalgo\"}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: #51 an unavailable compressor should 409, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/*
		 * Issue #65: the kernel-line endpoint's own contract. The
		 * resolution logic itself is test_kernelpolicy's job (no
		 * network there, and none needed here either) -- what this
		 * asserts is that a never-refreshed box answers honestly
		 * rather than guessing, and that an invented channel is
		 * refused rather than silently stored.
		 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/kernel-policy", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "channel"), "pinned") ||
		    json_object_get(r.json, "resolved_version")->type != JSON_NULL ||
		    json_object_get(r.json, "behind")->type != JSON_NULL) {
			fprintf(stderr, "FAIL: #65 a fresh box should be pinned and resolve nothing "
			                "(status=%d)\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/kernel-policy",
		                       "{\"channel\":\"edge\"}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: #65 an invented channel should 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/kernel-policy",
		                       "{\"channel\":\"longterm\"}", &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "channel"), "longterm")) {
			fprintf(stderr, "FAIL: #65 PUT channel=longterm, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* Selecting a channel must not, by itself, invent a version:
		 * nothing has been resolved, and reporting one would be the
		 * kind of confident wrong answer this whole field exists to
		 * replace. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/kernel-policy", NULL, &r) != 0 ||
		    r.status != 200 ||
		    json_object_get(r.json, "resolved_version")->type != JSON_NULL ||
		    json_object_get(r.json, "releases_fetched_at")->type != JSON_NULL) {
			fprintf(stderr, "FAIL: #65 selecting a channel resolved a version out of nothing\n");
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/control-plane-reservation", NULL, &r) !=
		        0 ||
		    r.status != 200 || json_object_get(r.json, "enabled") == NULL ||
		    !str_eq(json_str_field(r.json, "cgroup"), "cix-workload")) {
			fprintf(stderr, "FAIL: #86 GET control-plane-reservation, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* A reservation big enough to be a second workload budget is
		 * refused: the range check is the difference between a safety
		 * margin and an accidental new ceiling. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/control-plane-reservation",
		                       "{\"cpu_percent\":80}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: #86 cpu_percent=80 should 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* A real change takes effect immediately, on the live cgroup --
		 * an operator raising the reservation because the box is under
		 * strain needs it while it is under strain, not at the next
		 * container creation. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/control-plane-reservation",
		                       "{\"cpu_percent\":25}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #86 PUT cpu_percent=25, status=%d\n", r.status);
			ok = 0;
		} else {
			const char *want = json_str_field(r.json, "workload_cpu_max");
			FILE *f = fopen("/sys/fs/cgroup/cix-workload/cpu.max", "r");
			char got[64] = "";

			if (f != NULL) {
				if (fgets(got, sizeof(got), f) != NULL)
					got[strcspn(got, "\n")] = '\0';
				fclose(f);
			}
			if (want == NULL || strcmp(got, want) != 0) {
				fprintf(stderr, "FAIL: #86 workload cpu.max='%s', reported '%s'\n", got,
				        want != NULL ? want : "(null)");
				ok = 0;
			}
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "PUT", "/v1/system/control-plane-reservation",
		                   "{\"cpu_percent\":10}", &r);
		cix_response_free(&r);
	}

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/c49", NULL, &r);
	cix_response_free(&r);

	/* 6.6. issue #66: ldap_client refused with no client config, then
	 * accepted once configured -- and it stages the two identity files
	 * rendered from that config (nslcd.conf carries the bind values,
	 * proving they came from the daemon, not the recipe). */
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cll\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"ldap_client\":true}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: ldap_client with no client config should 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (cix_client_request(&client, "PUT", "/v1/ldap/config",
	                       "{\"client_uri\":\"ldap://10.7.7.7:3893/\","
	                       "\"base_dn\":\"dc=t,dc=local\","
	                       "\"bind_dn\":\"cn=svc,dc=t,dc=local\","
	                       "\"bind_password\":\"llpw\"}", &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: PUT ldap client config for ldap_client, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cll\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"ldap_client\":true}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: ldap_client create once configured, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (cix_client_request(&client, "GET", "/v1/containers/cll/files?path=/etc/nslcd.conf", NULL,
	                       &r) != 0 ||
	    r.status != 200 || r.body == NULL ||
	    strstr(r.body, "uri ldap://10.7.7.7:3893/") == NULL ||
	    strstr(r.body, "bindpw llpw") == NULL) {
		fprintf(stderr, "FAIL: ldap_client nslcd.conf not rendered from daemon config (status=%d)\n",
		        r.status);
		ok = 0;
	} else if (strstr(r.body, "pam_authz_search") != NULL) {
		/* Issue #76: no ldap_allow_groups means no restriction at all,
		 * which has to be the ABSENCE of the search rather than a
		 * permissive one -- a filter that is meant to allow everyone is
		 * a filter that can be got wrong. */
		fprintf(stderr, "FAIL: an unrestricted ldap_client container carries a pam_authz_search\n");
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * Issue #76: host-scoped authorisation. Which users may log into
	 * THIS container is a per-container statement, enforced by nslcd
	 * itself after authentication -- not by anything of ours that has
	 * to still be running.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cllg\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"ldap_client\":true,\"ldap_allow_groups\":[\"nosuchgroup\"]}",
	                       &r) != 0 ||
	    r.status != 400 || json_str_field(r.json, "error") == NULL ||
	    strstr(json_str_field(r.json, "error"), "nosuchgroup") == NULL) {
		fprintf(stderr, "FAIL: an unknown allow-group should 400 naming it, got %d: %.*s\n", r.status, (int)r.body_len, r.body ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/ldap/groups", "{\"name\":\"jumpusers\"}", &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: create group jumpusers, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * A name that would change the filter's meaning is refused, never
	 * escaped: this string is written into a file nslcd parses as an
	 * LDAP filter.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cllg\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"ldap_client\":true,\"ldap_allow_groups\":[\"evil)(uid=*\"]}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: a filter-shaped group name should 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"cllg\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"30\",\"0\"]}],"
	                       "\"ldap_client\":true,\"ldap_allow_groups\":[\"jumpusers\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: ldap_client with an allow-group, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/cllg/files?path=/etc/nslcd.conf", NULL,
	                       &r) != 0 ||
	    r.status != 200 || r.body == NULL ||
	    strstr(r.body, "pam_authz_search (&(objectClass=posixAccount)(uid=$username)"
	                   "(|(memberOf=ou=jumpusers,ou=groups,dc=t,dc=local)))") == NULL) {
		fprintf(stderr, "FAIL: the staged nslcd.conf carries no usable pam_authz_search:\n%.*s\n",
		        (int)r.body_len, r.body != NULL ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/cllg", NULL, &r);
	cix_response_free(&r);
	if (cix_client_request(&client, "GET", "/v1/containers/cll/files?path=/etc/nsswitch.conf", NULL,
	                       &r) != 0 ||
	    r.status != 200 || r.body == NULL || strstr(r.body, "files ldap") == NULL) {
		fprintf(stderr, "FAIL: ldap_client nsswitch.conf not staged (status=%d)\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/cll", NULL, &r);
	cix_response_free(&r);

	/* 7. duplicate name -> 409 */
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c1\",\"image\":\"test\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\",\"0\",\"1\"]}]}",
	                       &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate name expected 409, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 8. missing image -> 400 */
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"c3\",\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/daemon_child\"]}]}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: missing image expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 8a. GET/PUT /v1/system/resolv (ADR-0076/ADR-0132): a real
	 * round-trip proving resolv_set()'s own write mechanism (changed
	 * from persist_atomic_write() to an in-place O_TRUNC write, so a
	 * real --init-mode boot's /etc/resolv.conf bind mount stays live
	 * across a PUT, ADR-0132) still leaves GET reporting exactly what
	 * was set -- the actual bind-mount liveness itself can't be
	 * exercised here, since boot_init() only runs under --init-mode
	 * (not this plain dev/test daemon invocation), same documented
	 * limitation as every other real-boot-only behavior in this
	 * project. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/system/resolv",
	                       "{\"nameservers\":[\"192.168.15.101\",\"192.168.15.102\"]}", &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: PUT /v1/system/resolv, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/resolv", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/system/resolv, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *nameservers = json_object_get(r.json, "nameservers");

		if (nameservers == NULL || nameservers->type != JSON_ARRAY || nameservers->u.array.count != 2 ||
		    !str_eq(json_as_string(nameservers->u.array.items[0]), "192.168.15.101") ||
		    !str_eq(json_as_string(nameservers->u.array.items[1]), "192.168.15.102")) {
			fprintf(stderr, "FAIL: GET /v1/system/resolv did not reflect the PUT\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 9. clean shutdown */
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
	printf(ok ? "DAEMON RESULT: PASS\n" : "DAEMON RESULT: FAIL\n");
	return ok ? 0 : 1;
}
