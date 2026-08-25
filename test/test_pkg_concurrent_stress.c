/*
 * ADR-0157 Phase 4: the decisive proof for parallel package builds --
 * not just that N genuinely different packages can install
 * concurrently (test_pkg.c's own step 4/4b already proved that for
 * N=2), but that the daemon's own single, shared g_pkgbuild_rootfs
 * merge-back sandbox produces byte-identical final content whether N
 * installs run concurrently or strictly serially. Two entirely
 * separate daemon instances (own --data-dir, own --port) install the
 * exact same N packages -- one concurrently (a real stress on the
 * shared sandbox + chain isolation), one strictly one-at-a-time -- and
 * their resulting base image rootfs trees are diffed byte-for-byte.
 * Anything that let two concurrent merge-backs interleave would show
 * up here as a real, reproducible content mismatch, not just a
 * skipped/late file that a "did both jobs reach state=installed"
 * check alone would never catch.
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
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define PORT_A 7686
#define PORT_B 7687
/* ADR-0157 Phase 3's own real default is 10 -- deliberately lowered
 * here (daemon A only) to a small N so the concurrency boundary
 * (N in flight, N+1th rejected) is exercised deterministically without
 * needing N+1 = 11 real gcc compiles just to prove the ceiling still
 * means something. */
#define N_PACKAGES 3

static int g_failures;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s\n", msg); \
			g_failures++; \
		} \
	} while (0)

static int run_cmd(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;
	int rc;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	rc = system(cmd);
	return (rc == 0) ? 0 : -1;
}

static pid_t start_daemon(const char *data_dir, int port)
{
	pid_t pid;
	char *dargv[4];
	char data_dir_arg[PATH_MAX + 11];
	char port_arg[32];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", data_dir);
	snprintf(port_arg, sizeof(port_arg), "--port=%d", port);
	dargv[0] = "build/cixd";
	dargv[1] = port_arg;
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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static int compute_file_sha256(const char *path, char *out_sha256, size_t sha256_size)
{
	char shacmd[700];
	FILE *sp;
	char buf[128] = { 0 };

	snprintf(shacmd, sizeof(shacmd), "sha256sum '%s'", path);
	sp = popen(shacmd, "r");
	if (sp == NULL)
		return -1;
	if (fgets(buf, sizeof(buf), sp) == NULL) {
		pclose(sp);
		return -1;
	}
	pclose(sp);
	if (strlen(buf) < 64 || 64 >= sha256_size)
		return -1;
	memcpy(out_sha256, buf, 64);
	out_sha256[64] = '\0';
	return 0;
}

/* Same shape as test_pkg.c's own stage_fixture_tarball() -- a tiny
 * synthetic "hello" C program, version embedded in its own printed
 * output. One tarball per package name, staged once into a shared
 * scratch dir and reused unmodified by BOTH daemons below (the same
 * bytes on disk, read independently by each) -- this is deliberate:
 * the whole point is proving the daemon's own build/merge pipeline is
 * what stays deterministic, not that the input happened to be
 * identical because it literally is the same file. */
static int stage_fixture_tarball(const char *scratch_dir, const char *name, char *out_tarball_path,
                                  size_t tarball_path_size, char *out_sha256, size_t sha256_size)
{
	char src_dir[512];
	char hello_c[600], makefile[600];
	FILE *f;

	snprintf(src_dir, sizeof(src_dir), "%s/%s-1.0", scratch_dir, name);
	if (run_cmd("mkdir -p '%s'", src_dir) != 0)
		return -1;

	snprintf(hello_c, sizeof(hello_c), "%s/hello.c", src_dir);
	f = fopen(hello_c, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "#include <stdio.h>\nint main(void){printf(\"hello from %s v1.0\\n\");return 0;}\n",
	        name);
	fclose(f);

	snprintf(makefile, sizeof(makefile), "%s/Makefile", src_dir);
	f = fopen(makefile, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "hello: hello.c\n\tgcc -o hello hello.c\n");
	fprintf(f, "install:\n\tmkdir -p $(DESTDIR)/usr/bin\n\tcp hello $(DESTDIR)/usr/bin/%s\n", name);
	fclose(f);

	snprintf(out_tarball_path, tarball_path_size, "%s/%s-1.0.tarball", scratch_dir, name);
	if (run_cmd("tar -cf '%s' -C '%s' '%s-1.0'", out_tarball_path, scratch_dir, name) != 0)
		return -1;

	return compute_file_sha256(out_tarball_path, out_sha256, sha256_size);
}

static int write_recipe(const char *pkg_state_dir, const char *name, const char *tarball_path,
                         const char *sha256)
{
	char path[300];
	FILE *f;

	if (run_cmd("mkdir -p '%s/recipes/%s/1.0'", pkg_state_dir, name) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/recipes/%s/1.0/build.sh", pkg_state_dir, name);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "pkg_name=%s\n", name);
	fprintf(f, "pkg_version=1.0\n");
	fprintf(f, "pkg_source=file://%s\n", tarball_path);
	fprintf(f, "pkg_sha256=%s\n", sha256);
	fprintf(f, "pkg_depends=\"\"\n\n");
	fprintf(f, "pkg_build() {\n\tgcc -o hello hello.c\n}\n\n");
	fprintf(f, "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
	           "\"$PKG_DESTDIR/usr/bin/%s\"\n}\n",
	        name);
	fclose(f);
	return 0;
}

static int poll_pkg_state(const struct cix_client *c, const char *name, char *out_state,
                           size_t out_state_size, int max_attempts)
{
	int i;
	char path[256];

	snprintf(path, sizeof(path), "/v1/pkg/%s", name);
	for (i = 0; i < max_attempts; i++) {
		struct cix_response r;
		const char *state;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", path, NULL, &r) != 0 || r.status != 200) {
			cix_response_free(&r);
			return -1;
		}
		state = json_str_field(r.json, "state");
		if (state == NULL) {
			cix_response_free(&r);
			return -1;
		}
		snprintf(out_state, out_state_size, "%s", state);
		cix_response_free(&r);
		if (strcmp(out_state, "fetching") != 0 && strcmp(out_state, "building") != 0)
			return 0;
		usleep(300000);
	}
	return -1;
}

int main(void)
{
	char scratch_dir[] = "/tmp/cix_test_pkgstress_XXXXXX";
	char data_dir_a[PATH_MAX], data_dir_b[PATH_MAX];
	char pkg_state_a[PATH_MAX], pkg_state_b[PATH_MAX];
	char images_base_a[PATH_MAX], images_base_b[PATH_MAX];
	char names[N_PACKAGES + 1][32];
	char tarball_path[N_PACKAGES + 1][512];
	char sha256[N_PACKAGES + 1][128];
	pid_t daemon_a, daemon_b;
	struct cix_client client_a, client_b;
	struct cix_response r;
	char version_a[128], version_b[128];
	int i;

	if (mkdtemp(scratch_dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		return 1;
	}
	for (i = 0; i < N_PACKAGES + 1; i++)
		snprintf(names[i], sizeof(names[i]), "sp%d", i + 1);
	for (i = 0; i < N_PACKAGES + 1; i++) {
		if (stage_fixture_tarball(scratch_dir, names[i], tarball_path[i], sizeof(tarball_path[i]),
		                           sha256[i], sizeof(sha256[i])) != 0) {
			fprintf(stderr, "FAIL: could not stage fixture tarball for %s\n", names[i]);
			run_cmd("rm -rf '%s'", scratch_dir);
			return 1;
		}
	}

	/* ---- Daemon A: N packages installed concurrently ---- */

	if (test_data_dir_create(data_dir_a, sizeof(data_dir_a)) != 0)
		return 1;
	snprintf(pkg_state_a, sizeof(pkg_state_a), "%s/rebuildable/pkg", data_dir_a);
	snprintf(images_base_a, sizeof(images_base_a), "%s/rebuildable/images/base", data_dir_a);
	run_cmd("mkdir -p '%s/recipes'", pkg_state_a);
	for (i = 0; i < N_PACKAGES + 1; i++) {
		if (write_recipe(pkg_state_a, names[i], tarball_path[i], sha256[i]) != 0) {
			fprintf(stderr, "FAIL: could not write recipe %s (daemon A)\n", names[i]);
			g_failures++;
		}
	}

	daemon_a = start_daemon(data_dir_a, PORT_A);
	if (daemon_a < 0) {
		test_data_dir_cleanup(data_dir_a);
		return 1;
	}
	cix_client_init(&client_a, "127.0.0.1", PORT_A);
	CHECK(wait_for_daemon(&client_a, 50) == 0, "daemon A became healthy");

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client_a, "POST", "/v1/pkg/bootstrap", NULL, &r) != 0 || r.status != 204)
		CHECK(0, "POST /v1/pkg/bootstrap (daemon A)");
	cix_response_free(&r);

	/* Pin the concurrency ceiling to exactly N -- deterministic, and
	 * proves the config genuinely governs how many of these N+1
	 * install attempts succeed (a config-independent daemon would
	 * simply admit all N+1, or reject on some unrelated basis). */
	{
		char body[64];

		snprintf(body, sizeof(body), "{\"max_concurrent_jobs\":%d}", N_PACKAGES);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client_a, "PUT", "/v1/system/pkg-build-config", body, &r) != 0 ||
		    r.status != 200)
			CHECK(0, "PUT pkg-build-config max_concurrent_jobs=N (daemon A)");
		cix_response_free(&r);
	}

	/* Fire N installs back to back, no wait between any of them --
	 * every one must be accepted (202), all N chain slots genuinely
	 * fit. */
	for (i = 0; i < N_PACKAGES; i++) {
		char body[64];

		snprintf(body, sizeof(body), "{\"name\":\"%s\"}", names[i]);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client_a, "POST", "/v1/pkg/install", body, &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install %s (concurrent) status=%d\n", names[i], r.status);
			g_failures++;
		}
		cix_response_free(&r);
	}

	/* The (N+1)th, while all N slots are genuinely busy -> 409. */
	{
		char body[64];

		snprintf(body, sizeof(body), "{\"name\":\"%s\"}", names[N_PACKAGES]);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client_a, "POST", "/v1/pkg/install", body, &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: POST install %s (N+1th, all slots busy) expected 409, got %d\n",
			        names[N_PACKAGES], r.status);
			g_failures++;
		}
		cix_response_free(&r);
	}

	for (i = 0; i < N_PACKAGES; i++) {
		char state[32];

		if (poll_pkg_state(&client_a, names[i], state, sizeof(state), 120) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: %s (concurrent) ended in state '%s', expected installed\n",
			        names[i], state);
			g_failures++;
		}
	}

	if (test_image_fixture_read_current_version(images_base_a, version_a, sizeof(version_a)) != 0) {
		fprintf(stderr, "FAIL: could not read daemon A's base image current_version\n");
		g_failures++;
		version_a[0] = '\0';
	}

	stop_daemon(daemon_a);

	/* ---- Daemon B: the SAME N packages installed strictly serially --
	 * a completely separate daemon/data-dir/port, its own fresh
	 * bootstrap, so nothing about daemon A's own run leaks into this
	 * comparison. ---- */

	if (test_data_dir_create(data_dir_b, sizeof(data_dir_b)) != 0) {
		test_data_dir_cleanup(data_dir_a);
		return 1;
	}
	snprintf(pkg_state_b, sizeof(pkg_state_b), "%s/rebuildable/pkg", data_dir_b);
	snprintf(images_base_b, sizeof(images_base_b), "%s/rebuildable/images/base", data_dir_b);
	run_cmd("mkdir -p '%s/recipes'", pkg_state_b);
	for (i = 0; i < N_PACKAGES; i++) {
		if (write_recipe(pkg_state_b, names[i], tarball_path[i], sha256[i]) != 0) {
			fprintf(stderr, "FAIL: could not write recipe %s (daemon B)\n", names[i]);
			g_failures++;
		}
	}

	daemon_b = start_daemon(data_dir_b, PORT_B);
	if (daemon_b < 0) {
		test_data_dir_cleanup(data_dir_a);
		test_data_dir_cleanup(data_dir_b);
		return 1;
	}
	cix_client_init(&client_b, "127.0.0.1", PORT_B);
	CHECK(wait_for_daemon(&client_b, 50) == 0, "daemon B became healthy");

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client_b, "POST", "/v1/pkg/bootstrap", NULL, &r) != 0 || r.status != 204)
		CHECK(0, "POST /v1/pkg/bootstrap (daemon B)");
	cix_response_free(&r);

	for (i = 0; i < N_PACKAGES; i++) {
		char body[64];
		char state[32];

		snprintf(body, sizeof(body), "{\"name\":\"%s\"}", names[i]);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client_b, "POST", "/v1/pkg/install", body, &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install %s (serial) status=%d\n", names[i], r.status);
			g_failures++;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client_b, names[i], state, sizeof(state), 120) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: %s (serial) ended in state '%s', expected installed\n", names[i],
			        state);
			g_failures++;
		}
	}

	if (test_image_fixture_read_current_version(images_base_b, version_b, sizeof(version_b)) != 0) {
		fprintf(stderr, "FAIL: could not read daemon B's base image current_version\n");
		g_failures++;
		version_b[0] = '\0';
	}

	stop_daemon(daemon_b);

	/* ---- The decisive comparison ---- */

	/* ADR-0108: an image version's own directory name is a hash of its
	 * sorted (name, version) manifest -- order-independent by
	 * construction, so two runs that end up with the identical set of
	 * installed packages should land on the identical version string
	 * regardless of install order. A real, meaningful structural
	 * signal on its own, but NOT a substitute for the byte-for-byte
	 * content diff below -- a hash match proves the manifests agree,
	 * not that the actual file bytes underneath do. */
	if (version_a[0] != '\0' && version_b[0] != '\0')
		CHECK(strcmp(version_a, version_b) == 0,
		      "concurrent and serial runs converge on the same image version hash");

	/* The real proof: every file byte, not just the manifest. `diff -rq`
	 * compares content only (never mtimes/ownership), so this can only
	 * fail on a genuine content difference -- exactly what a bad
	 * interleaved merge-back would produce (a package's files
	 * partially overwritten by, or missing entirely because of, a
	 * concurrent sibling build).
	 *
	 * `--exclude=dev`: pkg_seed_image_baseline() (ADR-0150) seeds a
	 * fixed set of character-special device nodes (/dev/null, /dev/
	 * zero, ...) and a dangling /dev/ptmx placeholder symlink into
	 * every fresh image, completely independent of which packages get
	 * installed or how -- confirmed live (a first, unfiltered run of
	 * this exact diff reported both trees' device nodes as real,
	 * structurally-identical character-special files, and the ptmx
	 * symlink as equally absent -- not readable/nonexistent -- on both
	 * sides). `diff` fundamentally cannot compare a special file's own
	 * "content" and always reports it as unquantifiable regardless of
	 * whether the two sides genuinely match, so it's excluded here as
	 * out of scope for what this test actually proves (package-file
	 * merge determinism), not because it might legitimately differ. */
	if (version_a[0] != '\0' && version_b[0] != '\0') {
		char rootfs_a[PATH_MAX], rootfs_b[PATH_MAX];
		char diff_cmd[2 * PATH_MAX + 64];

		snprintf(rootfs_a, sizeof(rootfs_a), "%s/%s/rootfs", images_base_a, version_a);
		snprintf(rootfs_b, sizeof(rootfs_b), "%s/%s/rootfs", images_base_b, version_b);
		snprintf(diff_cmd, sizeof(diff_cmd), "diff -rq --exclude=dev '%s' '%s' >/dev/null 2>&1",
		         rootfs_a, rootfs_b);
		CHECK(system(diff_cmd) == 0,
		      "concurrent-run and serial-run base image rootfs trees are byte-for-byte identical");
	}

	/* Each installed binary genuinely runs and produces the expected,
	 * name-specific output -- confirms the content diff above isn't
	 * vacuously passing over empty/corrupt files. */
	for (i = 0; i < N_PACKAGES; i++) {
		char bin_path[PATH_MAX];
		char run_out[256] = { 0 };
		char expect[64];
		FILE *fp;

		snprintf(bin_path, sizeof(bin_path), "%s/%s/rootfs/usr/bin/%s", images_base_a, version_a,
		         names[i]);
		snprintf(expect, sizeof(expect), "hello from %s v1.0", names[i]);
		fp = popen(bin_path, "r");
		if (fp == NULL || fgets(run_out, sizeof(run_out), fp) == NULL ||
		    strstr(run_out, expect) == NULL) {
			fprintf(stderr, "FAIL: %s binary (concurrent run) did not produce expected output: %s\n",
			        names[i], run_out);
			g_failures++;
		}
		if (fp != NULL)
			pclose(fp);
	}

	test_data_dir_cleanup(data_dir_a);
	test_data_dir_cleanup(data_dir_b);
	run_cmd("rm -rf '%s'", scratch_dir);

	if (g_failures == 0)
		printf("PKG CONCURRENT STRESS RESULT: PASS\n");
	else
		printf("PKG CONCURRENT STRESS RESULT: FAIL (%d failure(s))\n", g_failures);

	return g_failures == 0 ? 0 : 1;
}
