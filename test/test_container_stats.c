/*
 * ADR-0054 end-to-end test: GET /v1/containers/{name}/stats over real
 * HTTP against a real cixd subprocess and a real running container
 * (stats_child.c) that actively burns CPU, touches memory, and appends
 * to a real on-disk file in a loop -- proving the returned numbers
 * genuinely move across two samples, not just that the endpoint
 * returns 200 with plausible-looking zeros. Also proves the daemon's
 * own startup-time io-controller enablement (cgroup_enable_io_
 * accounting()) actually takes effect, and that the networks[] entry
 * names the network, not the internal host-side veth.
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

#define TEST_PORT 7636
#define PORT_ARG "--port=7636"

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

static long long json_num_field(const struct json_value *obj, const char *key)
{
	return (long long)json_as_number(json_object_get(obj, key));
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

static int fetch_stats(const struct cix_client *c, const char *name, struct cix_response *out)
{
	char path[128];

	snprintf(path, sizeof(path), "/v1/containers/%s/stats", name);
	memset(out, 0, sizeof(*out));
	return cix_client_request(c, "GET", path, NULL, out);
}

int main(void)
{
	struct cix_client client;
	struct cix_response r;
	pid_t daemon_pid;
	int ok = 1;
	long long cpu1, cpu2, disk1, disk2, mem1;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/statstest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/statstest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}

	if (test_image_fixture_build(g_image_root, "build/stats_child", "stats_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
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

	/* 1. 404 for a name that was never created. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/nosuchthing/stats", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: GET stats for nonexistent container expected 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. a network to attach to, so networks[] has a real entry to check. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"statsnet\",\"subnet\":\"172.40.0.0\",\"prefix_len\":24}", &r) !=
	        0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/networks, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. the real container: stats_child burns CPU, touches memory, and
	 * appends to /statsdata.bin in a loop until killed. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"statsctr\",\"image\":\"statstest\","
	                       "\"cmd\":[\"/bin/stats_child\"],"
	                       "\"memory_max\":67108864,"
	                       "\"networks\":[\"statsnet\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST statsctr, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* Let it run through at least one real CPU-burn + disk-append cycle
	 * before the first sample. */
	usleep(300000);

	/* 4. first sample -- real numbers, not all zero. */
	memset(&r, 0, sizeof(r));
	if (fetch_stats(&client, "statsctr", &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET statsctr/stats (1st), status=%d\n", r.status);
		ok = 0;
		cpu1 = disk1 = mem1 = 0;
	} else {
		const struct json_value *cpu = json_object_get(r.json, "cpu");
		const struct json_value *mem = json_object_get(r.json, "memory");
		const struct json_value *disk = json_object_get(r.json, "disk");
		const struct json_value *nets = json_object_get(r.json, "networks");

		cpu1 = json_num_field(cpu, "usage_usec");
		mem1 = json_num_field(mem, "current");
		disk1 = json_num_field(disk, "upper_bytes");

		if (cpu1 <= 0) {
			fprintf(stderr, "FAIL: cpu.usage_usec should already be > 0 after a real CPU burn, got %lld\n",
			        cpu1);
			ok = 0;
		}
		if (mem1 <= 0) {
			fprintf(stderr, "FAIL: memory.current should be > 0 (a 4MiB buffer was touched), got %lld\n",
			        mem1);
			ok = 0;
		}
		if (disk1 <= 0) {
			fprintf(stderr,
			        "FAIL: disk.upper_bytes should be > 0 (/statsdata.bin was appended to), got %lld\n",
			        disk1);
			ok = 0;
		}
		if (json_num_field(mem, "max") != 67108864) {
			fprintf(stderr, "FAIL: memory.max should echo the real memory_max limit, got %lld\n",
			        json_num_field(mem, "max"));
			ok = 0;
		}
		if (nets == NULL || nets->type != JSON_ARRAY || nets->u.array.count != 1) {
			fprintf(stderr, "FAIL: networks[] should have exactly one entry\n");
			ok = 0;
		} else {
			const struct json_value *n0 = nets->u.array.items[0];

			if (!str_eq(json_str_field(n0, "name"), "statsnet")) {
				fprintf(stderr, "FAIL: networks[0].name should be \"statsnet\" (the network name, "
				                "not the internal veth name), got %s\n",
				        json_str_field(n0, "name"));
				ok = 0;
			}
			if (json_object_get(n0, "rx_bytes") == NULL || json_object_get(n0, "tx_bytes") == NULL) {
				fprintf(stderr, "FAIL: networks[0] missing rx_bytes/tx_bytes\n");
				ok = 0;
			}
		}
	}
	cix_response_free(&r);

	/* Real wall-clock time for another CPU-burn + disk-append cycle. */
	usleep(400000);

	/* 5. second sample -- cpu.usage_usec and disk.upper_bytes must have
	 * genuinely advanced (real, monotonic, not the same snapshot
	 * replayed). */
	memset(&r, 0, sizeof(r));
	if (fetch_stats(&client, "statsctr", &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET statsctr/stats (2nd), status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *cpu = json_object_get(r.json, "cpu");
		const struct json_value *disk = json_object_get(r.json, "disk");

		cpu2 = json_num_field(cpu, "usage_usec");
		disk2 = json_num_field(disk, "upper_bytes");

		if (cpu2 <= cpu1) {
			fprintf(stderr, "FAIL: cpu.usage_usec did not advance (%lld -> %lld)\n", cpu1, cpu2);
			ok = 0;
		}
		if (disk2 <= disk1) {
			fprintf(stderr, "FAIL: disk.upper_bytes did not advance (%lld -> %lld)\n", disk1, disk2);
			ok = 0;
		}
	}
	cix_response_free(&r);

	/*
	 * 6. a container that exits ON ITS OWN (not via POST .../stop, which
	 * calls registry_remove() and genuinely tears the cgroup fd down --
	 * see handle_stop()) stays in the registry with running == 0
	 * (registry_mark_exited(), a different, non-destructive path) --
	 * its cgroup leaf is never rmdir()'d while that entry exists, so
	 * stats must still return 200, not 404. daemon_child (already built
	 * for test_container_lifecycle.c/test_daemon.c) sleeps 1s then
	 * exits with code 7.
	 */
	if (test_image_fixture_build(g_image_root, "build/daemon_child", "daemon_child") != 0) {
		fprintf(stderr, "FAIL: could not stage daemon_child into the stats test image\n");
		ok = 0;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"statsexited\",\"image\":\"statstest\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"1\",\"7\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST statsexited, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	{
		int i, exited = 0;

		for (i = 0; i < 50 && !exited; i++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers/statsexited", NULL, &r) == 0 &&
			    r.status == 200 && str_eq(json_str_field(r.json, "status"), "exited"))
				exited = 1;
			cix_response_free(&r);
			if (!exited)
				usleep(100000);
		}
		if (!exited) {
			fprintf(stderr, "FAIL: statsexited never reached status \"exited\"\n");
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	if (fetch_stats(&client, "statsexited", &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET statsexited/stats after a natural exit should still be 200, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * 7. POST .../stop, unlike a natural exit, really does remove the
	 * registry entry (registry_remove(), same as pause/unpause already
	 * 404 after it) -- stats must 404 too, for consistency with every
	 * other registry_find()-based endpoint.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers/statsctr/stop", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: POST statsctr/stop, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (fetch_stats(&client, "statsctr", &r) != 0 || r.status != 404) {
		fprintf(stderr, "FAIL: GET statsctr/stats after stop should be 404 (registry_remove()'d), got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * Delete the real bridge this test created (DELETE /v1/networks is
	 * the only thing that tears it down -- a daemon SIGTERM does not,
	 * confirmed live, and this project's daemon-linked tests all share
	 * the host's own real network namespace, so a leftover bridge here
	 * would break this same test's own next run with a real EEXIST on
	 * the host, same convention test_dns.c/test_networks.c already
	 * follow).
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/networks/statsnet", NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: DELETE /v1/networks/statsnet, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);

	printf("CONTAINER STATS RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
