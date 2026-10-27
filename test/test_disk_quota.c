/*
 * test_disk_quota -- a container's disk_quota_bytes is a btrfs qgroup
 * limit on its rootfs snapshot (ADR-0207, ADR-0359, ADR-0360).
 *
 * Proves, over real HTTP against a real cixd subprocess, on the
 * btrfs data directory the gate provides (test_data_dir_create() puts
 * it under $TMPDIR, a build container's snapshot):
 *
 *   1. a create with disk_quota_bytes answers 201, and the container's
 *      rootfs (<data_dir>/rebuildable/rootfs/<name>) is a subvolume
 *      whose qgroup carries exactly that MAX_EXCL limit;
 *   2. a create without one answers 201 and its qgroup carries no
 *      limit, so a quota is never applied that was not asked for;
 *   3. GET .../stats answers disk.usage from the qgroup, not a walk.
 *
 * This test used to prove the ext4 project-quota backend (ADR-0062):
 * the persisted project-id map, and a 500 on a filesystem without the
 * quota feature. ADR-0360 removed that backend with the overlay path it
 * tagged, and compile probe 0.2.57-642, the first gate run on btrfs,
 * failed it for that reason (cix#658). Measured there on the way: a
 * 1 MiB limit is smaller than what cixd stages into a rootfs before the
 * container starts, so that create failed at a mkdir with "Disk quota
 * exceeded". The limits here are well above it.
 *
 * It does not write past the limit from inside a container. ADR-0359
 * measured that on 192.168.15.95: a 100 MiB limit refused a write at
 * 104841216 bytes, in both accounting modes.
 */
#include "httpclient.h"
#include "json.h"
#include "btrfs.h"
#include "test_image_fixture.h"
#include "test_drift.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7637
#define PORT_ARG "--port=7637"

#define QUOTA_BYTES (64ULL * 1024 * 1024)

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];

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

/*
 * The MAX_EXCL limit on container NAME's rootfs qgroup, read host-side
 * with the daemon's own helper. 0 means no limit; -1 means the rootfs is
 * not a subvolume with a qgroup, which is itself a failure here.
 */
static int rootfs_limit(const char *name, unsigned long long *limit)
{
	char rootfs[PATH_MAX];

	if (test_container_tree_path(g_data_dir, name, "", rootfs, sizeof(rootfs)) != 0)
		return -1;
	if (cix_btrfs_qgroup_query(rootfs, NULL, limit, NULL) != 0) {
		fprintf(stderr, "qgroup query on %s: %s\n", rootfs, strerror(errno));
		return -1;
	}
	return 0;
}

static int create(struct cix_client *client, const char *body)
{
	struct cix_response r;
	int status;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(client, "POST", "/v1/containers", body, &r) != 0)
		return -1;
	status = r.status;
	if (status != 201)
		fprintf(stderr, "create answered %d: %.*s\n", status, (int)r.body_len,
		        r.body != NULL ? r.body : "");
	cix_response_free(&r);
	return status;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
	unsigned long long limit = 0;
	char body[512];

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/quotatest/v1/rootfs", g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/quotatest", g_data_dir);
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

	/* 1. A quota is the qgroup limit on the snapshot, exactly. */
	snprintf(body, sizeof(body),
	         "{\"name\":\"quota64\",\"image\":\"quotatest\","
	         "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\","
	         "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}],\"disk_quota_bytes\":%llu}",
	         QUOTA_BYTES);
	if (create(&client, body) != 201) {
		fprintf(stderr, "FAIL: create with disk_quota_bytes: expected 201\n");
		ok = 0;
	} else if (rootfs_limit("quota64", &limit) != 0) {
		fprintf(stderr, "FAIL: quota64's rootfs has no qgroup to read\n");
		ok = 0;
	} else if (limit != QUOTA_BYTES) {
		fprintf(stderr, "FAIL: quota64's qgroup limit is %llu, expected %llu\n", limit,
		        QUOTA_BYTES);
		ok = 0;
	}

	/* 2. No quota asked for, none applied. */
	if (create(&client, "{\"name\":\"noquota\",\"image\":\"quotatest\","
	                    "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\","
	                    "\"cmd\":[\"/bin/daemon_child\",\"120\",\"0\"]}]}") != 201) {
		fprintf(stderr, "FAIL: no-quota create: expected 201\n");
		ok = 0;
	} else if (rootfs_limit("noquota", &limit) != 0) {
		fprintf(stderr, "FAIL: noquota's rootfs has no qgroup to read\n");
		ok = 0;
	} else if (limit != 0) {
		fprintf(stderr, "FAIL: noquota's qgroup carries a limit of %llu it never asked for\n",
		        limit);
		ok = 0;
	}

	/* 3. The stats figure comes from that qgroup. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/containers/quota64/stats", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET quota64/stats, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *disk = json_object_get(r.json, "disk");
		const struct json_value *usage = json_object_get(disk, "usage");
		const struct json_value *source = json_object_get(usage, "source");

		if (source == NULL || source->type != JSON_STRING ||
		    strcmp(source->u.string, "qgroup") != 0) {
			fprintf(stderr, "FAIL: quota64's disk.usage should come from the qgroup: %s\n",
			        r.body != NULL ? r.body : "");
			ok = 0;
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/quota64", NULL, &r);
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/noquota", NULL, &r);
	cix_response_free(&r);

	if (test_assert_no_drift(&client, NULL) != 0)
		ok = 0;
	if (ok)
		printf("DISK QUOTA RESULT: PASS\n");
	else
		printf("DISK QUOTA RESULT: FAIL\n");

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	return ok ? 0 : 1;
}
