/*
 * Phase 12 part B end-to-end test: proves the image lifecycle resource
 * (POST/GET/DELETE /v1/images -- daemon/src/image.c) over real HTTP:
 * create, list, get, duplicate-create 409, its C runtime seeded
 * immediately (ADR-0023), delete refused for "base" itself, delete
 * refused while a running container still references the image,
 * delete refused while pkg.c still tracks a package against it, and a
 * successful delete of an empty, unused image with its directory
 * genuinely gone afterward.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7629
#define PORT_ARG "--port=7629"

static char g_data_dir[PATH_MAX];
static char g_pkg_state_dir[PATH_MAX];
static char g_images_dir[PATH_MAX];

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

static void reset_state(void)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_pkg_state_dir);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s/imgtest_empty'", g_images_dir);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s/imgtest_ctr'", g_images_dir);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s/imgtest_pkg'", g_images_dir);
	system(cmd);
}

static int run_cmd(const char *fmt, ...)
{
	char cmd[1024];
	va_list ap;
	int rc;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	rc = system(cmd);
	return (rc == 0) ? 0 : -1;
}

/* Stages a tiny synthetic tarball -- just needs to be a real,
 * fetchable file:// source; this test intentionally pairs it with a
 * WRONG sha256 (mirroring test_pkg.c's own "badsum" scenario) so the
 * install fails fast at checksum verification, before any build
 * container ever launches -- pkg.c still tracks the entry (in_use)
 * regardless of ending in FAILED, exactly the property being tested. */
static int stage_fixture_tarball(const char *scratch_dir, const char *name,
                                  char *out_tarball_path, size_t tarball_path_size)
{
	char src_dir[512], hello_c[600];
	FILE *f;

	snprintf(src_dir, sizeof(src_dir), "%s/%s-1.0", scratch_dir, name);
	if (run_cmd("mkdir -p '%s'", src_dir) != 0)
		return -1;

	snprintf(hello_c, sizeof(hello_c), "%s/hello.c", src_dir);
	f = fopen(hello_c, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "int main(void){return 0;}\n");
	fclose(f);

	snprintf(out_tarball_path, tarball_path_size, "%s/%s-1.0.tarball", scratch_dir, name);
	return run_cmd("tar -cf '%s' -C '%s' '%s-1.0'", out_tarball_path, scratch_dir, name);
}

static int write_recipe(const char *name, const char *tarball_path)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/recipes/%s.recipe", g_pkg_state_dir, name);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "pkg_name=%s\n", name);
	fprintf(f, "pkg_version=1.0\n");
	fprintf(f, "pkg_source=file://%s\n", tarball_path);
	fprintf(f,
	        "pkg_sha256=0000000000000000000000000000000000000000000000000000000000000000\n");
	fprintf(f, "pkg_depends=\"\"\n\n");
	fprintf(f, "pkg_build() {\n\tgcc -o hello hello.c\n}\n\n");
	fprintf(f, "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
	           "\"$PKG_DESTDIR/usr/bin/%s\"\n}\n",
	        name);
	fclose(f);
	return 0;
}

static int poll_pkg_state(const struct kx_client *c, const char *name, char *out_state,
                           size_t out_state_size, int max_attempts)
{
	int i;
	char path[256];

	snprintf(path, sizeof(path), "/v1/pkg/%s", name);
	for (i = 0; i < max_attempts; i++) {
		struct kx_response r;
		const char *state;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(c, "GET", path, NULL, &r) != 0 || r.status != 200) {
			kx_response_free(&r);
			return -1;
		}
		state = json_str_field(r.json, "state");
		if (state == NULL) {
			kx_response_free(&r);
			return -1;
		}
		snprintf(out_state, out_state_size, "%s", state);
		kx_response_free(&r);
		if (strcmp(out_state, "fetching") != 0 && strcmp(out_state, "building") != 0)
			return 0;
		usleep(200000);
	}
	return -1;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char scratch_dir[] = "/tmp/kanxeo_test_images_XXXXXX";
	struct stat st;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_pkg_state_dir, sizeof(g_pkg_state_dir), "%s/pkg", g_data_dir);
	snprintf(g_images_dir, sizeof(g_images_dir), "%s/images", g_data_dir);

	reset_state();
	run_cmd("mkdir -p '%s/recipes'", g_pkg_state_dir);

	if (mkdtemp(scratch_dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
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

	/* 1. create, list, get */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/images", "{\"name\":\"imgtest_empty\"}", &r) !=
	        0 ||
	    r.status != 201 || !str_eq(json_str_field(r.json, "name"), "imgtest_empty")) {
		fprintf(stderr, "FAIL: POST imgtest_empty, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/images", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/images, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *images = json_object_get(r.json, "images");
		int found = 0;
		size_t i;

		if (images != NULL && images->type == JSON_ARRAY) {
			for (i = 0; i < images->u.array.count; i++) {
				if (str_eq(json_str_field(images->u.array.items[i], "name"),
				           "imgtest_empty"))
					found = 1;
			}
		}
		if (!found) {
			fprintf(stderr, "FAIL: GET /v1/images missing imgtest_empty\n");
			ok = 0;
		}
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/images/imgtest_empty", NULL, &r) != 0 ||
	    r.status != 200 || !str_eq(json_str_field(r.json, "name"), "imgtest_empty")) {
		fprintf(stderr, "FAIL: GET imgtest_empty, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. duplicate create -> 409 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/images", "{\"name\":\"imgtest_empty\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate create expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. runtime seeded immediately -- ADR-0023 */
	{
		char p1[PATH_MAX], p2[PATH_MAX], p3[PATH_MAX];

		snprintf(p1, sizeof(p1), "%s/imgtest_empty/rootfs/lib64/ld-linux-x86-64.so.2",
		         g_images_dir);
		snprintf(p2, sizeof(p2), "%s/imgtest_empty/rootfs/lib/x86_64-linux-gnu/libc.so.6",
		         g_images_dir);
		snprintf(p3, sizeof(p3), "%s/imgtest_empty/rootfs/lib/x86_64-linux-gnu/libtinfo.so.6",
		         g_images_dir);
		if (stat(p1, &st) != 0 || stat(p2, &st) != 0 || stat(p3, &st) != 0) {
			fprintf(stderr, "FAIL: imgtest_empty missing its own C runtime right after create\n");
			ok = 0;
		}
	}

	/* 4. "base" is protected -- under --data-dir= isolation this daemon
	 * has never seen a "base" image before (unlike a real deployment,
	 * where install/pkg-bootstrap always creates one long before any
	 * test runs), so it has to be created here first for the protected-
	 * delete check below to actually exercise IMAGE_ERR_PROTECTED rather
	 * than IMAGE_ERR_NOT_FOUND. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/images", "{\"name\":\"base\"}", &r) != 0 ||
	    (r.status != 201 && r.status != 409)) {
		fprintf(stderr, "FAIL: POST base, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/images/base", NULL, &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: DELETE base expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. unknown image -> 404 on both GET and DELETE */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/images/doesnotexist", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: GET doesnotexist expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/images/doesnotexist", NULL, &r) != 0 ||
	    r.status != 404) {
		fprintf(stderr, "FAIL: DELETE doesnotexist expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 6. delete refused while a running container references the image;
	 * succeeds once that container is gone, directory genuinely removed */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/images", "{\"name\":\"imgtest_ctr\"}", &r) !=
	        0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST imgtest_ctr, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	{
		char ctr_rootfs[PATH_MAX];

		snprintf(ctr_rootfs, sizeof(ctr_rootfs), "%s/imgtest_ctr/rootfs", g_images_dir);
		if (test_image_fixture_build(ctr_rootfs, "build/daemon_child", "daemon_child") != 0) {
			fprintf(stderr, "FAIL: could not stage daemon_child into imgtest_ctr\n");
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/containers",
	                       "{\"name\":\"imgtest-c1\",\"image\":\"imgtest_ctr\","
	                       "\"cmd\":[\"/bin/daemon_child\",\"5\",\"0\"]}",
	                       &r) != 0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST imgtest-c1, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/images/imgtest_ctr", NULL, &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: DELETE imgtest_ctr while in use expected 409, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/containers/imgtest-c1", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE imgtest-c1, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/images/imgtest_ctr", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: DELETE imgtest_ctr (now unused) expected 204, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);
	{
		char ctr_dir[PATH_MAX];

		snprintf(ctr_dir, sizeof(ctr_dir), "%s/imgtest_ctr", g_images_dir);
		if (stat(ctr_dir, &st) == 0) {
			fprintf(stderr, "FAIL: imgtest_ctr directory still exists after delete\n");
			ok = 0;
		}
	}

	/* 7. delete refused while pkg.c still tracks a package against it --
	 * a deliberately-wrong checksum ends the install FAILED fast (no
	 * real build ever launches), but the entry stays tracked either
	 * way -- exactly the property under test. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/images", "{\"name\":\"imgtest_pkg\"}", &r) !=
	        0 ||
	    r.status != 201) {
		fprintf(stderr, "FAIL: POST imgtest_pkg, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	{
		char tarball_path[512];

		if (stage_fixture_tarball(scratch_dir, "badsum2", tarball_path,
		                           sizeof(tarball_path)) != 0 ||
		    write_recipe("badsum2", tarball_path) != 0) {
			fprintf(stderr, "FAIL: could not stage badsum2 fixture/recipe\n");
			ok = 0;
		} else {
			char state[32];

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/pkg/install",
			                       "{\"name\":\"badsum2\",\"image\":\"imgtest_pkg\"}",
			                       &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: POST install badsum2@imgtest_pkg, status=%d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);

			if (poll_pkg_state(&client, "badsum2@imgtest_pkg", state, sizeof(state), 30) !=
			        0 ||
			    strcmp(state, "failed") != 0) {
				fprintf(stderr, "FAIL: badsum2@imgtest_pkg never reached failed\n");
				ok = 0;
			}

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "DELETE", "/v1/images/imgtest_pkg", NULL, &r) !=
			        0 ||
			    r.status != 409) {
				fprintf(stderr,
				        "FAIL: DELETE imgtest_pkg with a tracked package expected 409, "
				        "got %d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);
		}
	}

	/* cleanup */
	run_cmd("rm -rf '%s'", scratch_dir);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "IMAGES RESULT: PASS\n" : "IMAGES RESULT: FAIL\n");
	return ok ? 0 : 1;
}
