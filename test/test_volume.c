/*
 * Issue #88 end-to-end test: persistent volumes.
 *
 * The property under test is the entire reason volumes exist and is not
 * covered by any other test here: data written by a container SURVIVES
 * that container being deleted, and is visible to a completely different
 * container that mounts the same volume afterwards. Everything a
 * container writes otherwise lives in its overlay upper layer, which
 * DELETE removes (ADR-0106).
 *
 * Also covers the guard rails, because each of them protects real data:
 * an unknown volume name is refused rather than silently created (a typo
 * that quietly produced an empty volume is exactly how someone concludes
 * persistence "didn't work"), and deleting a volume is refused while any
 * container definition still references it.
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

#define TEST_PORT 7781
#define PORT_ARG "--port=7781"

static char g_data_dir[PATH_MAX];
static char g_image_root[PATH_MAX];
static int g_failures;

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		g_failures++;
	}
}

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
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execve("build/thincd", dargv, environ);
		_exit(127);
	}
	return pid;
}

/* Polls until name reports "exited", then returns its captured output. */
static int wait_exited(const struct kx_client *c, const char *name, char *out, size_t out_size)
{
	char path[128];
	int i;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < 100; i++) {
		struct kx_response r;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200) {
			const char *st = json_str_field(r.json, "status");

			if (st != NULL && strcmp(st, "exited") == 0) {
				const char *cap = json_str_field(r.json, "captured_output");

				snprintf(out, out_size, "%s", cap != NULL ? cap : "");
				kx_response_free(&r);
				return 0;
			}
		}
		kx_response_free(&r);
		usleep(100000);
	}
	return -1;
}

/* Waits for a DELETE to fully settle (ADR-0180 async teardown). */
static void wait_gone(const struct kx_client *c, const char *name)
{
	char path[128];
	int i;

	snprintf(path, sizeof(path), "/v1/containers/%s", name);
	for (i = 0; i < 100; i++) {
		struct kx_response r;
		int gone;

		memset(&r, 0, sizeof(r));
		gone = (kx_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 404);
		kx_response_free(&r);
		if (gone)
			return;
		usleep(100000);
	}
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	struct kx_response r;
	char out[256];

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_image_root, sizeof(g_image_root), "%s/rebuildable/images/voltest/v1/rootfs",
	         g_data_dir);
	{
		char image_dir[PATH_MAX];

		snprintf(image_dir, sizeof(image_dir), "%s/rebuildable/images/voltest", g_data_dir);
		if (test_image_fixture_write_manifest(image_dir, "v1") != 0) {
			test_data_dir_cleanup(g_data_dir);
			return 1;
		}
	}
	if (test_image_fixture_build(g_image_root, "build/volume_child", "volume_child") != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	kx_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. create a volume */
	memset(&r, 0, sizeof(r));
	check(kx_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"vol1\"}", &r) == 0 &&
	          r.status == 201,
	      "POST /v1/volumes creates a volume");
	kx_response_free(&r);

	/* 2. an invalid name must be refused -- a volume name becomes a real
	 * directory name, so path-escaping characters can never be accepted. */
	memset(&r, 0, sizeof(r));
	check(kx_client_request(&client, "POST", "/v1/volumes", "{\"name\":\"../escape\"}", &r) == 0 &&
	          r.status == 400,
	      "a path-escaping volume name is refused (400)");
	kx_response_free(&r);

	/* 3. a container naming an UNKNOWN volume is refused rather than
	 * silently getting a fresh empty one. */
	memset(&r, 0, sizeof(r));
	check(kx_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"cbad\",\"image\":\"voltest\","
	                         "\"volumes\":[{\"name\":\"nosuchvol\",\"path\":\"/vol\"}],"
	                         "\"cmd\":[\"/bin/volume_child\"]}",
	                         &r) == 0 &&
	          r.status == 400,
	      "a container naming an unknown volume is refused (400), never auto-created");
	kx_response_free(&r);

	/* 4. write into the volume from a real container */
	memset(&r, 0, sizeof(r));
	check(kx_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"cwrite\",\"image\":\"voltest\",\"capture_output\":true,"
	                         "\"volumes\":[{\"name\":\"vol1\",\"path\":\"/vol\"}],"
	                         "\"cmd\":[\"/bin/volume_child\",\"write\"]}",
	                         &r) == 0 &&
	          r.status == 201,
	      "container with a volume is created");
	kx_response_free(&r);

	out[0] = '\0';
	check(wait_exited(&client, "cwrite", out, sizeof(out)) == 0, "writer container exited");
	check(strstr(out, "WROTE") != NULL, "writer actually wrote into the mounted volume");

	/* 5. THE POINT: delete the container entirely, then read the data
	 * back from a DIFFERENT container mounting the same volume. */
	memset(&r, 0, sizeof(r));
	kx_client_request(&client, "DELETE", "/v1/containers/cwrite", NULL, &r);
	kx_response_free(&r);
	wait_gone(&client, "cwrite");

	memset(&r, 0, sizeof(r));
	check(kx_client_request(&client, "POST", "/v1/containers",
	                         "{\"name\":\"cread\",\"image\":\"voltest\",\"capture_output\":true,"
	                         "\"volumes\":[{\"name\":\"vol1\",\"path\":\"/vol\"}],"
	                         "\"cmd\":[\"/bin/volume_child\"]}",
	                         &r) == 0 &&
	          r.status == 201,
	      "second container with the same volume is created");
	kx_response_free(&r);

	out[0] = '\0';
	check(wait_exited(&client, "cread", out, sizeof(out)) == 0, "reader container exited");
	check(strstr(out, "READ:PERSISTED") != NULL,
	      "volume data SURVIVED the first container being deleted");

	/* 6. deleting a volume is refused while a container definition still
	 * references it -- silently removing data a stopped container will
	 * expect on its next start would be a data-loss bug. */
	memset(&r, 0, sizeof(r));
	check(kx_client_request(&client, "DELETE", "/v1/volumes/vol1", NULL, &r) == 0 &&
	          r.status == 409,
	      "volume delete refused (409) while a container definition references it");
	kx_response_free(&r);

	/* 7. once nothing references it, the volume deletes cleanly. */
	memset(&r, 0, sizeof(r));
	kx_client_request(&client, "DELETE", "/v1/containers/cread", NULL, &r);
	kx_response_free(&r);
	wait_gone(&client, "cread");

	memset(&r, 0, sizeof(r));
	check(kx_client_request(&client, "DELETE", "/v1/volumes/vol1", NULL, &r) == 0 &&
	          r.status == 204,
	      "volume deletes once nothing references it");
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	check(kx_client_request(&client, "GET", "/v1/volumes/vol1", NULL, &r) == 0 && r.status == 404,
	      "deleted volume is gone");
	kx_response_free(&r);

	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);

	if (g_failures == 0) {
		printf("VOLUME RESULT: PASS\n");
		return 0;
	}
	printf("VOLUME RESULT: FAIL (%d)\n", g_failures);
	return 1;
}
