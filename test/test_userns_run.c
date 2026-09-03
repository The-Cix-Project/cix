/*
 * test_userns_run -- a user-namespaced container's own root must be able
 * to write to /run (#264).
 *
 * The bug this exists to catch: mountns_pivot() mounts /run's tmpfs while
 * still running as real host root, which is UNMAPPED in the container's
 * user namespace, so the tmpfs came up owned by the overflow uid (65534)
 * and the container's root -- the uid the payload actually runs as --
 * could not write to its own scratch directory. `jump` crash-looped on
 * exactly this, and nothing caught it for the life of the feature because
 * nothing in the suite ever wrote to /run: dnsmasq is launched with
 * --pid-file= and glauth writes nothing there.
 *
 * Why it has to be a userns container: without a user namespace the
 * container's root IS host root, /run comes up owned by 0 either way, and
 * the test would pass just as happily against the broken code. The bug is
 * only reachable when the two differ.
 *
 * That means this test needs an environment that can actually run one. The
 * dev sandbox cannot (see test_image_fixture.c, which seeds
 * userns_default=false for that reason), so it SKIPS there rather than
 * failing -- but it skips only when the container never starts at all,
 * never on a container that ran and was refused the write. A skip that
 * could swallow the defect would be worse than no test.
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

#define TEST_PORT 7799
#define PORT_ARG "--port=7799"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static int g_failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		g_failures++;
	}
}

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;

	for (i = 0; i < max_attempts; i++) {
		struct cix_response r;
		int ok;

		memset(&r, 0, sizeof(r));
		ok = (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0 && r.status == 200);
		cix_response_free(&r);
		if (ok)
			return 0;
		usleep(100000);
	}
	return -1;
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
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		_exit(127);
	}
	return pid;
}

/*
 * Polls until the container reports "exited" and returns its exit status,
 * or -1 if it never got there. Its captured output is copied out too: on a
 * failure the RUN_OWNER line names the cause outright.
 */
static int wait_exit_status(const struct cix_client *c, const char *name, char *out,
                             size_t out_size)
{
	char path[128];
	int i;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < 150; i++) {
		struct cix_response r;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200) {
			const char *st = json_as_string(json_object_get(r.json, "status"));
			const struct json_value *jes = json_object_get(r.json, "exit_status");

			if (st != NULL && strcmp(st, "exited") == 0 && jes != NULL &&
			    jes->type == JSON_NUMBER) {
				const char *cap =
				        json_as_string(json_object_get(r.json, "captured_output"));
				int status = (int)jes->u.number;

				snprintf(out, out_size, "%s", cap != NULL ? cap : "");
				cix_response_free(&r);
				return status;
			}
		}
		cix_response_free(&r);
		usleep(100000);
	}
	return -1;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	char out[512];
	int created;
	int status;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/runtest/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/runtest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}
	if (test_image_fixture_build(g_image_root, "build/run_child", "run_child") != 0) {
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

	/*
	 * "userns":true explicitly, never the platform default: the fixture
	 * deliberately seeds userns_default=false, and relying on a default
	 * would make this test silently stop testing anything the day that
	 * default changed.
	 */
	memset(&r, 0, sizeof(r));
	created = (cix_client_request(&client, "POST", "/v1/containers",
	                              "{\"name\":\"runprobe\",\"image\":\"runtest\","
	                              "\"userns\":true,\"capture_output\":true,"
	                              "\"cmd\":[\"/bin/run_child\"]}",
	                              &r) == 0 &&
	           r.status == 201);
	cix_response_free(&r);

	if (!created) {
		printf("SKIP: this environment cannot create a user-namespaced container\n");
		printf("      (the dev sandbox cannot; the real host can -- run it there)\n");
		goto done;
	}

	out[0] = '\0';
	status = wait_exit_status(&client, "runprobe", out, sizeof(out));
	if (out[0] != '\0')
		printf("  container said: %s", out);

	if (status == 42) {
		printf("  PASS: the container's own root can write to /run\n");
	} else if (status == 43) {
		check(0, "a userns container's own root cannot write to /run -- "
		         "the /run tmpfs is owned by an unmapped uid (#264); "
		         "mount_container_tmpfs() in src/mountns.c is what sets this");
	} else if (status < 0) {
		check(0, "the /run probe container never reached 'exited'");
	} else {
		char msg[160];

		snprintf(msg, sizeof(msg),
		         "the /run probe exited %d, which is neither writable (42) nor "
		         "denied (43) -- read its output above rather than trusting this test",
		         status);
		check(0, msg);
	}

	memset(&r, 0, sizeof(r));
	cix_client_request(&client, "DELETE", "/v1/containers/runprobe", NULL, &r);
	cix_response_free(&r);

done:
	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);

	if (g_failures > 0) {
		fprintf(stderr, "USERNS /run: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("USERNS /run: PASS\n");
	return 0;
}
