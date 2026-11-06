/*
 * ADR-0375 phase 2: a running container survives cixd execing itself in
 * place.
 *
 * The daemon runs with --test-handover-after=N. Before then the test
 * creates a network and container "hoa", whose service runs until
 * killed, and records its pid and address. When the handed-over daemon
 * logs "took over", it asserts:
 *
 *   - hoa is listed with the same pid, still running -- the containers
 *     stayed the process's children, which is why this is not the adopt
 *     ADR-0247 removed;
 *   - its stats answer (the cgroup was reacquired by path);
 *   - a new container on the same network gets a different address (the
 *     adopted entry holds its address);
 *   - a stop through the API reaches cix-init over the passed socket
 *     and the exit is reaped through the reacquired pidfd.
 *
 * And it ends, as every daemon-linked test does, with no drift.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"
#include "test_drift.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7693
#define PORT_ARG "--port=7693"
#define HANDOVER_AFTER "--test-handover-after=12"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;
	struct cix_response r;

	for (i = 0; i < max_attempts; i++) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[5];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = HANDOVER_AFTER;
	dargv[4] = NULL;
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
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* hoa's status, pid and first address, from GET /v1/containers/NAME. */
static int read_container(const struct cix_client *c, const char *name, char *status,
                          size_t status_size, long long *pid, char *ip, size_t ip_size)
{
	struct cix_response r;
	char path[128];
	const struct json_value *nets;
	int rc = -1;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200 && r.json != NULL) {
		const char *s = json_as_string(json_object_get(r.json, "status"));

		snprintf(status, status_size, "%s", s != NULL ? s : "");
		*pid = (long long)json_as_number(json_object_get(r.json, "pid"));
		nets = json_object_get(r.json, "networks");
		ip[0] = '\0';
		if (nets != NULL && nets->type == JSON_ARRAY && nets->u.array.count > 0) {
			const char *a = json_as_string(json_object_get(nets->u.array.items[0], "ip"));

			snprintf(ip, ip_size, "%s", a != NULL ? a : "");
		}
		rc = 0;
	}
	cix_response_free(&r);
	return rc;
}

/* Has the handed-over daemon logged its takeover? */
static int took_over(const struct cix_client *c)
{
	struct cix_response r;
	int seen;

	memset(&r, 0, sizeof(r));
	seen = cix_client_request(c, "GET", "/v1/system/logs?source=cixd&regex=took%20over&tail=5",
	                          NULL, &r) == 0 &&
	       r.body != NULL && strstr(r.body, "handover: took over") != NULL;
	cix_response_free(&r);
	return seen;
}

int main(void)
{
	struct cix_client client;
	struct cix_response r;
	pid_t daemon_pid;
	int ok = 1, i;
	char status[32], ip[32], ip_after[32], ip_new[32];
	long long pid_before = 0, pid_after = 0, pid_new = 0;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/hotest/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/hotest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0 ||
		    test_image_fixture_build(g_image_root, "build/stats_child", "stats_child") != 0) {
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

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/networks",
	                       "{\"name\":\"honet\",\"subnet\":\"172.41.0.0\",\"prefix_len\":24}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST /v1/networks, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"hoa\",\"image\":\"hotest\",\"services\":[{\"name\":\"main\","
	                       "\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/stats_child\"]}],"
	                       "\"memory_max\":67108864,\"networks\":[\"honet\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST hoa, status=%d %.300s\n", r.status,
		        r.body != NULL ? r.body : "");
		ok = 0;
	}
	cix_response_free(&r);
	if (ok && (read_container(&client, "hoa", status, sizeof(status), &pid_before, ip,
	                          sizeof(ip)) != 0 ||
	           strcmp(status, "running") != 0 || pid_before <= 0 || ip[0] == '\0')) {
		fprintf(stderr, "FAIL: hoa before the handover: status=%s pid=%lld ip=%s\n", status,
		        pid_before, ip);
		ok = 0;
	}

	/* The handover: the daemon execs itself at 12 s and logs its takeover. */
	for (i = 0; ok && i < 300 && !took_over(&client); i++)
		usleep(200000);
	if (ok && !took_over(&client)) {
		fprintf(stderr, "FAIL: the daemon never logged \"handover: took over\"\n");
		ok = 0;
	}

	if (ok) {
		if (read_container(&client, "hoa", status, sizeof(status), &pid_after, ip_after,
		                   sizeof(ip_after)) != 0 ||
		    strcmp(status, "running") != 0 || pid_after != pid_before ||
		    strcmp(ip_after, ip) != 0) {
			fprintf(stderr,
			        "FAIL: hoa after the handover must be the same running container: "
			        "status=%s pid %lld -> %lld ip %s -> %s\n",
			        status, pid_before, pid_after, ip, ip_after);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/containers/hoa/stats", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: hoa's stats after the handover, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* A new container cannot be given hoa's address. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"hob\",\"image\":\"hotest\",\"services\":[{\"name\":"
		                       "\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/bin/"
		                       "stats_child\"]}],\"memory_max\":67108864,\"networks\":[\"honet\"]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST hob after the handover, status=%d %.300s\n", r.status,
			        r.body != NULL ? r.body : "");
			ok = 0;
		}
		cix_response_free(&r);
		if (read_container(&client, "hob", status, sizeof(status), &pid_new, ip_new,
		                   sizeof(ip_new)) != 0 ||
		    ip_new[0] == '\0' || strcmp(ip_new, ip) == 0) {
			fprintf(stderr,
			        "FAIL: hob must get its own address, not hoa's (%s): got %s\n", ip,
			        ip_new);
			ok = 0;
		}

		/* A stop through the API: cix-init's socket and the pidfd survived. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers/hoa/stop", NULL, &r) != 0 ||
		    (r.status != 200 && r.status != 202 && r.status != 204)) {
			fprintf(stderr, "FAIL: POST hoa/stop after the handover, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		for (i = 0; i < 200; i++) {
			if (read_container(&client, "hoa", status, sizeof(status), &pid_after, ip_after,
			                   sizeof(ip_after)) != 0 ||
			    strcmp(status, "running") != 0)
				break;
			usleep(100000);
		}
		if (i == 200) {
			fprintf(stderr, "FAIL: hoa was still running 20 s after its stop\n");
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/hoa", NULL, &r);
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/hob", NULL, &r);
	cix_response_free(&r);
	for (i = 0; i < 100; i++) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/networks/honet", NULL, &r) == 0 &&
		    r.status == 204) {
			cix_response_free(&r);
			break;
		}
		cix_response_free(&r);
		usleep(100000);
	}
	if (test_assert_no_drift(&client, NULL) != 0)
		ok = 0;
	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}
	test_data_dir_cleanup(g_data_dir);
	printf("HANDOVER RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
