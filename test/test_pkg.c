/*
 * Phase 10 parts 1-2 end-to-end test: proves the package manager
 * (POST /v1/pkg/bootstrap, POST /v1/pkg/install, GET /v1/pkg[/{name}],
 * DELETE /v1/pkg/{name} -- daemon/src/pkg.c) over real HTTP, with a
 * real fetch -> checksum verify -> isolated container build -> merge
 * into the shared base image as the actual payoff, not a mocked
 * pipeline. Kept hermetic: no real internet dependency -- pkg_source
 * uses a file:// URL against a tiny synthetic C fixture this test
 * stages itself, so the real `curl` subprocess code path is genuinely
 * exercised (not skipped/mocked) while the suite stays offline-safe
 * for repeated stress-testing runs. Part 2 additionally proves
 * automatic dependency resolution (with cycle detection) and explicit
 * per-package upgrades.
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
#include <time.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7628
#define PORT_ARG "--port=7628"

static char g_data_dir[PATH_MAX];
static char g_pkg_state_dir[PATH_MAX];
static char g_pkgbuild_rootfs[PATH_MAX];
static char g_images_base_dir[PATH_MAX];
static char g_images_router_dir[PATH_MAX];

/* BASE_ROOTFS/ROUTER_ROOTFS were compile-time macros before --data-dir=
 * isolation (see CLAUDE.md's test-isolation Environment note); every
 * call site below concatenated a literal suffix onto them at compile
 * time. These two helpers are still the one place that difference is
 * absorbed, so every call site below just uses base_path("/x")/
 * router_path("/x") -- now resolving the image's own current version
 * (ADR-0107/0108, test_image_fixture_read_current_version()) fresh on
 * every call instead of a path fixed at test startup, since nearly
 * every scenario in this file installs/upgrades/deletes packages in
 * between rootfs checks. */
static const char *base_path(const char *suffix)
{
	static char buf[PATH_MAX];
	char version[128];

	if (test_image_fixture_read_current_version(g_images_base_dir, version, sizeof(version)) != 0)
		version[0] = '\0';
	snprintf(buf, sizeof(buf), "%s/%s/rootfs%s", g_images_base_dir, version, suffix);
	return buf;
}

static const char *router_path(const char *suffix)
{
	static char buf[PATH_MAX];
	char version[128];

	if (test_image_fixture_read_current_version(g_images_router_dir, version, sizeof(version)) != 0)
		version[0] = '\0';
	snprintf(buf, sizeof(buf), "%s/%s/rootfs%s", g_images_router_dir, version, suffix);
	return buf;
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
	dargv[0] = "build/thincd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/thincd", dargv, environ);
		perror("execve build/thincd");
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

/* Removes all pkg/image state left by a previous run so this test is
 * deterministic and rerunnable -- there is no "unbootstrap" endpoint
 * (bootstrap is meant to be a one-time, idempotent action), so a full
 * reset here (including re-bootstrapping every run) is the only way
 * to keep the suite's own stress-testing discipline (3 consecutive
 * clean runs) meaningful for this phase too. */
static void reset_pkg_state(void)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_pkg_state_dir);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_images_base_dir);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_images_router_dir);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_pkgbuild_rootfs);
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

/* Shared by stage_fixture_tarball() and stage_fixture_plain_file()
 * (multi-source recipe scenario, ADR-0036) -- computes path's real
 * sha256 via the real sha256sum binary, not hand-rolled crypto. */
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
	if (strlen(buf) < 64)
		return -1;
	if (64 >= sha256_size)
		return -1;
	memcpy(out_sha256, buf, 64);
	out_sha256[64] = '\0';
	return 0;
}

/* Stages a tiny synthetic C "hello world" + Makefile source tree at
 * <scratch_dir>/<name>-<version>/, tars it (with the standard
 * <name>-<version>/ wrapper directory every real source tarball has,
 * so --strip-components=1 behaves exactly like it would for a real
 * package), and returns its sha256 in *out_sha256. */
static int stage_fixture_tarball(const char *scratch_dir, const char *name, const char *version,
                                  char *out_tarball_path, size_t tarball_path_size,
                                  char *out_sha256, size_t sha256_size)
{
	char src_dir[512];
	char hello_c[600], makefile[600];
	FILE *f;

	snprintf(src_dir, sizeof(src_dir), "%s/%s-%s", scratch_dir, name, version);
	if (run_cmd("mkdir -p '%s'", src_dir) != 0)
		return -1;

	/* version embedded in the output too, not just the name -- lets a
	 * test tell an old build from a rebuilt-at-a-new-version one apart
	 * by what the binary actually prints, not just its manifest. */
	snprintf(hello_c, sizeof(hello_c), "%s/hello.c", src_dir);
	f = fopen(hello_c, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "#include <stdio.h>\nint main(void){printf(\"hello from %s v%s\\n\");return 0;}\n",
	        name, version);
	fclose(f);

	snprintf(makefile, sizeof(makefile), "%s/Makefile", src_dir);
	f = fopen(makefile, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "hello: hello.c\n\tgcc -o hello hello.c\n");
	fprintf(f, "install:\n\tmkdir -p $(DESTDIR)/usr/bin\n\tcp hello $(DESTDIR)/usr/bin/%s\n", name);
	fclose(f);

	snprintf(out_tarball_path, tarball_path_size, "%s/%s-%s.tarball", scratch_dir, name, version);
	if (run_cmd("tar -cf '%s' -C '%s' '%s-%s'", out_tarball_path, scratch_dir, name, version) != 0)
		return -1;

	return compute_file_sha256(out_tarball_path, out_sha256, sha256_size);
}

/* A plain, non-tarball fixture file -- source index 1+ in the
 * multisrc scenario below (ADR-0036), landing at /build/extra/
 * <basename> verbatim, never extracted. */
static int stage_fixture_plain_file(const char *scratch_dir, const char *filename, const char *content,
                                     char *out_path, size_t out_path_size, char *out_sha256,
                                     size_t sha256_size)
{
	FILE *f;

	snprintf(out_path, out_path_size, "%s/%s", scratch_dir, filename);
	f = fopen(out_path, "w");
	if (f == NULL)
		return -1;
	fputs(content, f);
	fclose(f);

	return compute_file_sha256(out_path, out_sha256, sha256_size);
}

/* ADR-0107: recipes live at recipes/<name>/<version>/build.sh -- the
 * two mkdir()s are best-effort (already-exists is fine, anything else
 * surfaces as the fopen() below failing). */
static int write_recipe(const char *name, const char *version, const char *tarball_path,
                         const char *sha256, const char *depends)
{
	char name_dir[256];
	char path[300];
	FILE *f;

	snprintf(name_dir, sizeof(name_dir), "%s/recipes/%s", g_pkg_state_dir, name);
	mkdir(name_dir, 0755);
	snprintf(path, sizeof(path), "%s/%s", name_dir, version);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/recipes/%s/%s/build.sh", g_pkg_state_dir, name, version);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "pkg_name=%s\n", name);
	fprintf(f, "pkg_version=%s\n", version);
	fprintf(f, "pkg_source=file://%s\n", tarball_path);
	fprintf(f, "pkg_sha256=%s\n", sha256);
	fprintf(f, "pkg_depends=\"%s\"\n\n", depends != NULL ? depends : "");
	fprintf(f, "pkg_build() {\n\tgcc -o hello hello.c\n}\n\n");
	fprintf(f, "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
	           "\"$PKG_DESTDIR/usr/bin/%s\"\n}\n",
	        name);
	fclose(f);
	return 0;
}

/* Multi-source recipe (ADR-0036): source 0 is the usual fixture
 * tarball; sources 1/2 are plain files that land at
 * /build/extra/extra1.txt and /build/extra/extra2.txt. pkg_install()
 * deliberately copies extra1.txt into PKG_DESTDIR so the caller can
 * check its real byte content afterward -- proof the file was
 * genuinely there during the build, not just that the job succeeded. */
static int write_multisrc_recipe(const char *name, const char *version, const char *tarball_path,
                                  const char *tarball_sha256, const char *extra1_path,
                                  const char *extra1_sha256, const char *extra2_path,
                                  const char *extra2_sha256)
{
	char name_dir[256];
	char path[300];
	FILE *f;

	snprintf(name_dir, sizeof(name_dir), "%s/recipes/%s", g_pkg_state_dir, name);
	mkdir(name_dir, 0755);
	snprintf(path, sizeof(path), "%s/%s", name_dir, version);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/recipes/%s/%s/build.sh", g_pkg_state_dir, name, version);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "pkg_name=%s\n", name);
	fprintf(f, "pkg_version=%s\n", version);
	fprintf(f, "pkg_source=\"file://%s file://%s file://%s\"\n", tarball_path, extra1_path,
	        extra2_path);
	fprintf(f, "pkg_sha256=\"%s %s %s\"\n", tarball_sha256, extra1_sha256, extra2_sha256);
	fprintf(f, "pkg_depends=\"\"\n\n");
	fprintf(f, "pkg_build() {\n\tgcc -o hello hello.c\n}\n\n");
	fprintf(f,
	        "pkg_install() {\n"
	        "\tmkdir -p \"$PKG_DESTDIR/usr/bin\" \"$PKG_DESTDIR/usr/share/multisrc\"\n"
	        "\tcp hello \"$PKG_DESTDIR/usr/bin/%s\"\n"
	        "\tcp /build/extra/extra1.txt \"$PKG_DESTDIR/usr/share/multisrc/extra1.txt\"\n"
	        "}\n",
	        name);
	fclose(f);
	return 0;
}

/* Polls GET /v1/pkg/{name} until state leaves fetching/building (or
 * max_attempts is exhausted). Writes the final state into out_state. */
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
		/* Compare via out_state (a stable, owned copy) after freeing
		 * r -- state itself points into r.json's tree and would be a
		 * dangling pointer the instant kx_response_free() runs. */
		snprintf(out_state, out_state_size, "%s", state);
		kx_response_free(&r);
		if (strcmp(out_state, "fetching") != 0 && strcmp(out_state, "building") != 0)
			return 0;
		usleep(300000);
	}
	return -1;
}

int main(void)
{
	pid_t daemon_pid;
	struct kx_client client;
	int ok = 1;
	struct kx_response r;
	char scratch_dir[] = "/tmp/thinc_test_pkg_XXXXXX";
	char tarball_path[512], sha256[128];
	char bad_sha256[128];
	char state[32];

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_pkg_state_dir, sizeof(g_pkg_state_dir), "%s/rebuildable/pkg", g_data_dir);
	snprintf(g_pkgbuild_rootfs, sizeof(g_pkgbuild_rootfs), "%s/rebuildable/images/pkgbuild", g_data_dir);
	snprintf(g_images_base_dir, sizeof(g_images_base_dir), "%s/rebuildable/images/base", g_data_dir);
	snprintf(g_images_router_dir, sizeof(g_images_router_dir), "%s/rebuildable/images/router", g_data_dir);

	reset_pkg_state();
	run_cmd("mkdir -p '%s/recipes'", g_pkg_state_dir);

	if (mkdtemp(scratch_dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (stage_fixture_tarball(scratch_dir, "greeter", "1.0", tarball_path, sizeof(tarball_path),
	                           sha256, sizeof(sha256)) != 0) {
		fprintf(stderr, "FAIL: could not stage fixture tarball\n");
		return 1;
	}
	if (write_recipe("greeter", "1.0", tarball_path, sha256, "") != 0 ||
	    write_recipe("concurrent", "1.0", tarball_path, sha256, "") != 0 ||
	    write_recipe("overflow", "1.0", tarball_path, sha256, "") != 0 ||
	    write_recipe("hbconcurrent", "1.0", tarball_path, sha256, "") != 0) {
		fprintf(stderr, "FAIL: could not write recipes\n");
		return 1;
	}
	snprintf(bad_sha256, sizeof(bad_sha256),
	         "0000000000000000000000000000000000000000000000000000000000000000");
	bad_sha256[64] = '\0';
	if (write_recipe("badsum", "1.0", tarball_path, bad_sha256, "") != 0) {
		fprintf(stderr, "FAIL: could not write badsum recipe\n");
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

	/* ADR-0157 Phase 3: the real, unmodified default before this test
	 * touches it at all. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/system/pkg-build-config", NULL, &r) != 0 ||
	    r.status != 200 || json_as_number(json_object_get(r.json, "max_concurrent_jobs")) != 10) {
		fprintf(stderr, "FAIL: GET pkg-build-config expected 200 max_concurrent_jobs=10, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 1. bootstrap the build toolchain image */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/bootstrap", NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: POST /v1/pkg/bootstrap, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* ADR-0157 Phase 3 raised the real default ceiling to 10 -- lowered
	 * here to a small, deterministic 2 so every "N chains busy" boundary
	 * check below (originally written against Phase 2's placeholder
	 * default) stays meaningful without needing 10+ concurrent installs
	 * to actually exhaust it. The config endpoint's own behavior
	 * (validation, and the ceiling genuinely taking effect) is proven
	 * separately, later in this test, by deliberately varying it. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":2}", &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: PUT pkg-build-config max_concurrent_jobs=2 (test setup), got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2. install for an unknown recipe -> 400 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"nosuchpackage\"}", &r) !=
	        0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: install unknown recipe expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 3. real install: greeter -> 202, fetching */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"greeter\"}", &r) != 0 ||
	    r.status != 202 || !str_eq(json_str_field(r.json, "state"), "fetching")) {
		fprintf(stderr, "FAIL: POST install greeter, status=%d, state=%s\n", r.status,
		        json_str_field(r.json, "state") ? json_str_field(r.json, "state") : "(null)");
		ok = 0;
	}
	kx_response_free(&r);

	/* 4. ADR-0157 Phase 2: a second install (a DIFFERENT package) while
	 * greeter is still in flight now genuinely fits -> 202, not 409.
	 * Issued immediately, before any polling -- fork() for the fetch
	 * subprocess just happened microseconds ago, so greeter is still
	 * "fetching" at this exact point, deterministically. Both chains
	 * build from the SAME source tarball (see write_recipe() above)
	 * concurrently, each installing under its own binary name into the
	 * SAME target image -- real proof this isn't just "two unrelated
	 * jobs happen not to collide," but genuine concurrent-chain
	 * isolation (distinct build containers, distinct output-capture
	 * pipes, a correctly-serialized final merge into one image). */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"concurrent\"}", &r) !=
	        0 ||
	    r.status != 202 || !str_eq(json_str_field(r.json, "state"), "fetching")) {
		fprintf(stderr, "FAIL: POST install concurrent (2nd chain) status=%d, state=%s\n", r.status,
		        json_str_field(r.json, "state") ? json_str_field(r.json, "state") : "(null)");
		ok = 0;
	}
	kx_response_free(&r);

	/* 4b. a THIRD distinct install while both chain slots are occupied
	 * -> 409 -- proves both slots are genuinely in use (not just that
	 * "concurrent" above got lucky some other way), and that the
	 * PKG_MAX_CONCURRENT_JOBS ceiling is still real and enforced. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"overflow\"}", &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: overflow install with both chains busy expected 409, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. poll until BOTH greeter and concurrent finish; confirm each
	 * actually installed and its binary genuinely runs from the base
	 * image -- the real payoff, not just files with the right names. */
	if (poll_pkg_state(&client, "greeter", state, sizeof(state), 60) != 0) {
		fprintf(stderr, "FAIL: greeter never left fetching/building\n");
		ok = 0;
	} else if (strcmp(state, "installed") != 0) {
		fprintf(stderr, "FAIL: greeter ended in state '%s', not installed\n", state);
		ok = 0;
	} else {
		char run_out[256] = { 0 };
		FILE *fp = popen(base_path("/usr/bin/greeter"), "r");

		if (fp == NULL || fgets(run_out, sizeof(run_out), fp) == NULL ||
		    strstr(run_out, "hello from greeter") == NULL) {
			fprintf(stderr, "FAIL: installed greeter binary did not run/produce expected output, got: %s\n",
			        run_out);
			ok = 0;
		}
		if (fp != NULL)
			pclose(fp);
	}

	if (poll_pkg_state(&client, "concurrent", state, sizeof(state), 60) != 0) {
		fprintf(stderr, "FAIL: concurrent never left fetching/building\n");
		ok = 0;
	} else if (strcmp(state, "installed") != 0) {
		fprintf(stderr, "FAIL: concurrent ended in state '%s', not installed\n", state);
		ok = 0;
	} else {
		char run_out[256] = { 0 };
		FILE *fp = popen(base_path("/usr/bin/concurrent"), "r");

		if (fp == NULL || fgets(run_out, sizeof(run_out), fp) == NULL ||
		    strstr(run_out, "hello from greeter") == NULL) {
			fprintf(stderr,
			        "FAIL: installed concurrent binary did not run/produce expected output, got: %s\n",
			        run_out);
			ok = 0;
		}
		if (fp != NULL)
			pclose(fp);
	}

	/* 6. duplicate install of an already-installed package -> 409 */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"greeter\"}", &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate install expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 7. checksum mismatch -> ends FAILED, never installed */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"badsum\"}", &r) != 0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: POST install badsum, status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (poll_pkg_state(&client, "badsum", state, sizeof(state), 30) != 0) {
		fprintf(stderr, "FAIL: badsum never left fetching/building\n");
		ok = 0;
	} else if (strcmp(state, "failed") != 0) {
		fprintf(stderr, "FAIL: badsum ended in state '%s', expected failed (checksum mismatch)\n",
		        state);
		ok = 0;
	}

	/* 7b. DELETE on a permanently-failed entry actually removes it
	 * (this daemon used to only accept PKG_STATE_INSTALLED, leaving a
	 * failed fetch/build stuck forever) -- and, since it was never
	 * merged into any image, base's own current_version must be
	 * completely unchanged by the removal (no new image version
	 * produced for something that never touched the image). */
	{
		char base_version_before[128], base_version_after[128];

		if (test_image_fixture_read_current_version(g_images_base_dir, base_version_before,
		                                             sizeof(base_version_before)) != 0) {
			fprintf(stderr, "FAIL: could not read base image current_version before badsum delete\n");
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/pkg/badsum", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE badsum (failed state) expected 204, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/badsum", NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: GET badsum after delete expected 404, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (test_image_fixture_read_current_version(g_images_base_dir, base_version_after,
		                                             sizeof(base_version_after)) != 0 ||
		    strcmp(base_version_before, base_version_after) != 0) {
			fprintf(stderr,
			        "FAIL: base image current_version changed after deleting a failed entry (%s -> %s)\n",
			        base_version_before, base_version_after);
			ok = 0;
		}
	}

	/* 8. delete removes the manifested file from the base image, not
	 * just the registry entry */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/pkg/greeter", NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: DELETE greeter expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	{
		struct stat st;

		if (stat(base_path("/usr/bin/greeter"), &st) == 0) {
			fprintf(stderr, "FAIL: greeter binary still exists in base image after delete\n");
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/pkg/greeter", NULL, &r) != 0 || r.status != 404) {
		fprintf(stderr, "FAIL: GET greeter after delete expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 8b. same cleanup for "concurrent" (step 4's second chain) -- left
	 * installed until now so later steps don't have to account for its
	 * presence; nothing past this point depends on it. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "DELETE", "/v1/pkg/concurrent", NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: DELETE concurrent expected 204, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	{
		struct stat st;

		if (stat(base_path("/usr/bin/concurrent"), &st) == 0) {
			fprintf(stderr, "FAIL: concurrent binary still exists in base image after delete\n");
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "GET", "/v1/pkg/concurrent", NULL, &r) != 0 || r.status != 404) {
		fprintf(stderr, "FAIL: GET concurrent after delete expected 404, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 9. dependency resolution: `top` depends on `leaf` -- a single
	 * install of top should transparently install leaf first, both
	 * ending up installed. */
	{
		char leaf_tarball[512], leaf_sha[128];
		char top_tarball[512], top_sha[128];

		if (stage_fixture_tarball(scratch_dir, "leaf", "1.0", leaf_tarball, sizeof(leaf_tarball),
		                           leaf_sha, sizeof(leaf_sha)) != 0 ||
		    stage_fixture_tarball(scratch_dir, "top", "1.0", top_tarball, sizeof(top_tarball),
		                           top_sha, sizeof(top_sha)) != 0) {
			fprintf(stderr, "FAIL: could not stage leaf/top fixtures\n");
			ok = 0;
		} else if (write_recipe("leaf", "1.0", leaf_tarball, leaf_sha, "") != 0 ||
		           write_recipe("top", "1.0", top_tarball, top_sha, "leaf") != 0) {
			fprintf(stderr, "FAIL: could not write leaf/top recipes\n");
			ok = 0;
		} else {
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"top\"}", &r) !=
			        0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: POST install top, status=%d\n", r.status);
				ok = 0;
			}
			kx_response_free(&r);

			if (poll_pkg_state(&client, "leaf", state, sizeof(state), 60) != 0 ||
			    strcmp(state, "installed") != 0) {
				fprintf(stderr, "FAIL: leaf (top's dependency) did not reach installed\n");
				ok = 0;
			}
			if (poll_pkg_state(&client, "top", state, sizeof(state), 60) != 0 ||
			    strcmp(state, "installed") != 0) {
				fprintf(stderr, "FAIL: top did not reach installed\n");
				ok = 0;
			}
		}
	}

	/* 10. circular dependency -> 400, nothing registered */
	if (write_recipe("circ1", "1.0", tarball_path, sha256, "circ2") != 0 ||
	    write_recipe("circ2", "1.0", tarball_path, sha256, "circ1") != 0) {
		fprintf(stderr, "FAIL: could not write circular recipes\n");
		ok = 0;
	} else {
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"circ1\"}", &r) !=
		        0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: circular dependency install expected 400, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/circ1", NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: circ1 should never have been registered, status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 11. a dependency with no matching recipe -> 400 */
	if (write_recipe("needsghost", "1.0", tarball_path, sha256, "ghost") != 0) {
		fprintf(stderr, "FAIL: could not write needsghost recipe\n");
		ok = 0;
	} else {
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"needsghost\"}", &r) !=
		        0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: missing dependency install expected 400, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}

	/* 12. upgrade: bump leaf to 2.0. available_version must be visible
	 * before upgrading; a plain re-POST stays 409; an explicit upgrade
	 * proceeds, ends up installed at 2.0, and the binary genuinely
	 * reflects the NEW source (real proof of a rebuild, not a stale
	 * cache re-served under a new label). */
	{
		char leaf2_tarball[512], leaf2_sha[128];

		if (stage_fixture_tarball(scratch_dir, "leaf", "2.0", leaf2_tarball, sizeof(leaf2_tarball),
		                           leaf2_sha, sizeof(leaf2_sha)) != 0) {
			fprintf(stderr, "FAIL: could not stage leaf 2.0 fixture\n");
			ok = 0;
		} else if (write_recipe("leaf", "2.0", leaf2_tarball, leaf2_sha, "") != 0) {
			fprintf(stderr, "FAIL: could not write leaf 2.0 recipe\n");
			ok = 0;
		} else {
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "GET", "/v1/pkg/leaf", NULL, &r) != 0 ||
			    r.status != 200 || !str_eq(json_str_field(r.json, "available_version"), "2.0")) {
				fprintf(stderr,
				        "FAIL: leaf should show available_version=2.0 before upgrading, got %s\n",
				        json_str_field(r.json, "available_version")
				            ? json_str_field(r.json, "available_version")
				            : "(null)");
				ok = 0;
			}
			kx_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"leaf\"}", &r) !=
			        0 ||
			    r.status != 409) {
				fprintf(stderr, "FAIL: re-install without upgrade expected 409, got %d\n",
				        r.status);
				ok = 0;
			}
			kx_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/pkg/install",
			                       "{\"name\":\"leaf\",\"upgrade\":true}", &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: upgrade install expected 202, got %d\n", r.status);
				ok = 0;
			}
			kx_response_free(&r);

			if (poll_pkg_state(&client, "leaf", state, sizeof(state), 60) != 0 ||
			    strcmp(state, "installed") != 0) {
				fprintf(stderr, "FAIL: leaf upgrade did not reach installed\n");
				ok = 0;
			} else {
				memset(&r, 0, sizeof(r));
				if (kx_client_request(&client, "GET", "/v1/pkg/leaf", NULL, &r) != 0 ||
				    !str_eq(json_str_field(r.json, "version"), "2.0") ||
				    json_str_field(r.json, "available_version") != NULL) {
					fprintf(stderr,
					        "FAIL: leaf after upgrade should be version=2.0, "
					        "available_version=null\n");
					ok = 0;
				}
				kx_response_free(&r);

				{
					char run_out[256] = { 0 };
					FILE *fp = popen(base_path("/usr/bin/leaf"), "r");

					if (fp == NULL || fgets(run_out, sizeof(run_out), fp) == NULL ||
					    strstr(run_out, "hello from leaf v2.0") == NULL) {
						fprintf(stderr,
						        "FAIL: leaf binary after upgrade did not reflect the "
						        "new source, got: %s\n",
						        run_out);
						ok = 0;
					}
					if (fp != NULL)
						pclose(fp);
				}
			}
		}
	}

	/* 13. per-image install: greeter's recipe still on disk (only its
	 * base-image install was deleted in step 8) -- installing it again
	 * with image="router" must land in a wholly separate rootfs, be
	 * independently tracked (compound name+image key), and not collide
	 * with a fresh, unrelated install of the SAME name back into the
	 * default "base" image. */
	{
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"greeter\",\"image\":\"router\"}", &r) != 0 ||
		    r.status != 202 || !str_eq(json_str_field(r.json, "image"), "router")) {
			fprintf(stderr, "FAIL: POST install greeter@router, status=%d, image=%s\n", r.status,
			        json_str_field(r.json, "image") ? json_str_field(r.json, "image") : "(null)");
			ok = 0;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "greeter@router", state, sizeof(state), 60) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: greeter@router did not reach installed\n");
			ok = 0;
		} else {
			struct stat st;

			if (stat(router_path("/usr/bin/greeter"), &st) != 0) {
				fprintf(stderr, "FAIL: greeter@router binary missing from router image\n");
				ok = 0;
			}
			/* Phase 12 part A: runtime seeding is no longer base-only --
			 * a non-default image's first install must land the same C
			 * runtime greeter itself needs to execve() at all. */
			if (stat(router_path("/lib64/ld-linux-x86-64.so.2"), &st) != 0 ||
			    stat(router_path("/lib/x86_64-linux-gnu/libc.so.6"), &st) != 0 ||
			    stat(router_path("/lib/x86_64-linux-gnu/libtinfo.so.6"), &st) != 0) {
				fprintf(stderr,
				        "FAIL: router image missing its own C runtime after first install\n");
				ok = 0;
			}
			/* ADR-0041: the same first-install baseline seeding must also
			 * land the standard dev nodes and an empty /run -- the real,
			 * generic gap Phase 23 (iptables' /run/xtables.lock) and Phase
			 * 24 (bird crashing with no /dev/null) both hit, previously
			 * fixed by hand on the already-built image, not reproducible
			 * from a fresh pkg install until now. */
			if (stat(router_path("/dev/null"), &st) != 0 ||
			    stat(router_path("/dev/zero"), &st) != 0 ||
			    stat(router_path("/dev/full"), &st) != 0 ||
			    stat(router_path("/dev/random"), &st) != 0 ||
			    stat(router_path("/dev/urandom"), &st) != 0 ||
			    stat(router_path("/run"), &st) != 0) {
				fprintf(stderr,
				        "FAIL: router image missing its baseline dev nodes/run dir after "
				        "first install\n");
				ok = 0;
			}
			if (stat(base_path("/usr/bin/greeter"), &st) == 0) {
				fprintf(stderr,
				        "FAIL: greeter@router install leaked into the base image "
				        "(greeter was deleted from base in step 8)\n");
				ok = 0;
			}
		}

		/* bare (base-image) addressing still 404s -- base's own greeter
		 * entry was deleted in step 8 and this install never touched it */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/greeter", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: GET greeter (base) expected 404, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* @-addressed GET reaches the router entry specifically */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/greeter@router", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "image"), "router") ||
		    !str_eq(json_str_field(r.json, "state"), "installed")) {
			fprintf(stderr, "FAIL: GET greeter@router expected 200 installed router, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* a fresh install of the SAME name back into the default image
		 * is independent -- greeter@router being installed must not
		 * make this a 409 duplicate */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"greeter\"}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr,
			        "FAIL: POST install greeter (base) while greeter@router installed "
			        "expected 202, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "greeter", state, sizeof(state), 60) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: greeter (base) did not reach installed\n");
			ok = 0;
		} else {
			struct stat st;

			if (stat(base_path("/usr/bin/greeter"), &st) != 0) {
				fprintf(stderr, "FAIL: greeter (base) binary missing after independent install\n");
				ok = 0;
			}
		}

		/* GET /v1/pkg (list) reports both (name, image) entries distinctly */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/pkg, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *packages = json_object_get(r.json, "packages");
			int saw_base = 0, saw_router = 0;
			size_t i;

			if (packages != NULL && packages->type == JSON_ARRAY) {
				for (i = 0; i < packages->u.array.count; i++) {
					const struct json_value *item = packages->u.array.items[i];

					if (!str_eq(json_str_field(item, "name"), "greeter"))
						continue;
					if (str_eq(json_str_field(item, "image"), "base"))
						saw_base = 1;
					if (str_eq(json_str_field(item, "image"), "router"))
						saw_router = 1;
				}
			}
			if (!saw_base || !saw_router) {
				fprintf(stderr,
				        "FAIL: GET /v1/pkg should list greeter@base AND greeter@router "
				        "as distinct entries (saw_base=%d saw_router=%d)\n",
				        saw_base, saw_router);
				ok = 0;
			}
		}
		kx_response_free(&r);

		/* @-addressed DELETE removes only the router entry, leaving the
		 * independently-installed base entry untouched */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/pkg/greeter@router", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE greeter@router expected 204, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		{
			struct stat st;

			if (stat(router_path("/usr/bin/greeter"), &st) == 0) {
				fprintf(stderr, "FAIL: greeter binary still exists in router image after delete\n");
				ok = 0;
			}
			if (stat(base_path("/usr/bin/greeter"), &st) != 0) {
				fprintf(stderr,
				        "FAIL: deleting greeter@router should not remove greeter@base\n");
				ok = 0;
			}
		}
	}

	/* 14. update-all (Phase 16, ADR-0031): nothing drifted at this point
	 * (leaf's own upgrade in step 12 already brought it to 2.0; top's
	 * own version never changed) -> "nothing to update". Then bump
	 * top's recipe to 2.0 (a real dependency-chain package, not a bare
	 * one) and confirm update-all itself finds it and starts exactly
	 * one real upgrade job for it -- no need for the caller to already
	 * know top drifted. Called again afterwards with the backlog fully
	 * drained, reports nothing to update once more rather than erroring. */
	{
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/update-all", "{}", &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "status"), "nothing to update")) {
			fprintf(stderr,
			        "FAIL: update-all with nothing drifted expected 200 \"nothing to update\", got %d %s\n",
			        r.status,
			        json_str_field(r.json, "status") ? json_str_field(r.json, "status") : "(null)");
			ok = 0;
		}
		kx_response_free(&r);

		{
			char top2_tarball[512], top2_sha[128];

			if (stage_fixture_tarball(scratch_dir, "top", "2.0", top2_tarball, sizeof(top2_tarball),
			                           top2_sha, sizeof(top2_sha)) != 0) {
				fprintf(stderr, "FAIL: could not stage top 2.0 fixture\n");
				ok = 0;
			} else if (write_recipe("top", "2.0", top2_tarball, top2_sha, "leaf") != 0) {
				fprintf(stderr, "FAIL: could not write top 2.0 recipe\n");
				ok = 0;
			} else {
				memset(&r, 0, sizeof(r));
				if (kx_client_request(&client, "POST", "/v1/pkg/update-all", "{}", &r) != 0 ||
				    r.status != 202 || !str_eq(json_str_field(r.json, "name"), "top")) {
					fprintf(stderr,
					        "FAIL: update-all with top drifted expected 202 name=top, got %d name=%s\n",
					        r.status,
					        json_str_field(r.json, "name") ? json_str_field(r.json, "name") : "(null)");
					ok = 0;
				}
				kx_response_free(&r);

				if (poll_pkg_state(&client, "top", state, sizeof(state), 60) != 0 ||
				    strcmp(state, "installed") != 0) {
					fprintf(stderr, "FAIL: top update-all upgrade did not reach installed\n");
					ok = 0;
				} else {
					char run_out[256] = { 0 };
					FILE *fp = popen(base_path("/usr/bin/top"), "r");

					if (fp == NULL || fgets(run_out, sizeof(run_out), fp) == NULL ||
					    strstr(run_out, "hello from top v2.0") == NULL) {
						fprintf(stderr,
						        "FAIL: top binary after update-all did not reflect the new "
						        "source, got: %s\n",
						        run_out);
						ok = 0;
					}
					if (fp != NULL)
						pclose(fp);
				}

				memset(&r, 0, sizeof(r));
				if (kx_client_request(&client, "POST", "/v1/pkg/update-all", "{}", &r) != 0 ||
				    r.status != 200 ||
				    !str_eq(json_str_field(r.json, "status"), "nothing to update")) {
					fprintf(stderr,
					        "FAIL: update-all after draining backlog expected 200 \"nothing to "
					        "update\", got %d\n",
					        r.status);
					ok = 0;
				}
				kx_response_free(&r);
			}
		}
	}

	/* 15. multi-source recipes (ADR-0036): a real install with one main
	 * tarball plus two extra plain files, proving (a) the whole thing
	 * installs end to end exactly like every single-source recipe
	 * already does, (b) an extra file was genuinely available under
	 * /build/extra/ *during* the build -- checked by its real byte
	 * content post-install, not just its presence -- and (c) a bad
	 * checksum on a non-zero-index source fails the *whole* job, the
	 * same all-or-nothing guarantee badsum.recipe already proves for
	 * index 0. */
	{
		char ms_tarball[512], ms_tarball_sha[128];
		char extra1_path[512], extra1_sha[128];
		char extra2_path[512], extra2_sha[128];

		if (stage_fixture_tarball(scratch_dir, "multisrc", "1.0", ms_tarball, sizeof(ms_tarball),
		                           ms_tarball_sha, sizeof(ms_tarball_sha)) != 0 ||
		    stage_fixture_plain_file(scratch_dir, "extra1.txt", "extra-content-one\n", extra1_path,
		                              sizeof(extra1_path), extra1_sha, sizeof(extra1_sha)) != 0 ||
		    stage_fixture_plain_file(scratch_dir, "extra2.txt", "extra-content-two\n", extra2_path,
		                              sizeof(extra2_path), extra2_sha, sizeof(extra2_sha)) != 0) {
			fprintf(stderr, "FAIL: could not stage multisrc fixtures\n");
			ok = 0;
		} else if (write_multisrc_recipe("multisrc", "1.0", ms_tarball, ms_tarball_sha, extra1_path,
		                                  extra1_sha, extra2_path, extra2_sha) != 0) {
			fprintf(stderr, "FAIL: could not write multisrc recipe\n");
			ok = 0;
		} else {
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"multisrc\"}", &r) !=
			        0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: POST install multisrc, status=%d\n", r.status);
				ok = 0;
			}
			kx_response_free(&r);

			if (poll_pkg_state(&client, "multisrc", state, sizeof(state), 30) != 0 ||
			    strcmp(state, "installed") != 0) {
				fprintf(stderr, "FAIL: multisrc ended in state '%s', expected installed\n", state);
				ok = 0;
			} else {
				struct stat st;
				char content[64] = { 0 };
				FILE *cf;

				if (stat(base_path("/usr/bin/multisrc"), &st) != 0) {
					fprintf(stderr, "FAIL: multisrc binary missing from base image\n");
					ok = 0;
				}
				cf = fopen(base_path("/usr/share/multisrc/extra1.txt"), "r");
				if (cf == NULL || fgets(content, sizeof(content), cf) == NULL ||
				    strcmp(content, "extra-content-one\n") != 0) {
					fprintf(stderr,
					        "FAIL: extra1.txt missing or wrong content in base image, got: %s\n",
					        content);
					ok = 0;
				}
				if (cf != NULL)
					fclose(cf);
			}
		}

		/* A bad checksum on the *second* extra (index 2, not index 0)
		 * must still fail the whole job -- confirms verification isn't
		 * limited to the main source. */
		if (write_multisrc_recipe("multisrcbad", "1.0", ms_tarball, ms_tarball_sha, extra1_path,
		                           extra1_sha, extra2_path,
		                           "0000000000000000000000000000000000000000000000000000000000000000") !=
		    0) {
			fprintf(stderr, "FAIL: could not write multisrcbad recipe\n");
			ok = 0;
		} else {
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"multisrcbad\"}",
			                       &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: POST install multisrcbad, status=%d\n", r.status);
				ok = 0;
			}
			kx_response_free(&r);

			if (poll_pkg_state(&client, "multisrcbad", state, sizeof(state), 30) != 0) {
				fprintf(stderr, "FAIL: multisrcbad never left fetching/building\n");
				ok = 0;
			} else if (strcmp(state, "failed") != 0) {
				fprintf(stderr,
				        "FAIL: multisrcbad ended in state '%s', expected failed (checksum "
				        "mismatch on a non-zero-index source)\n",
				        state);
				ok = 0;
			}
			{
				struct stat st;

	if (stat(base_path("/usr/bin/multisrcbad"), &st) == 0) {
					fprintf(stderr,
					        "FAIL: multisrcbad binary present despite a checksum mismatch on "
					        "one of its extra sources\n");
					ok = 0;
				}
			}
		}

		/* 15b. an extra source URL carrying a query string (a real,
		 * live-found bug, not a hypothetical -- a git-raw-file
		 * pkg_source needs a `?ref=<commit>` suffix, e.g.
		 * kernel.recipe's own qemu-part1.config fetch, and
		 * url_basename()'s original plain strrchr('/') left that
		 * query string attached to the staged /build/extra/<name>
		 * filename, so any recipe's own pkg_build()/pkg_install()
		 * reference to the plain filename it expected failed with a
		 * bare "No such file or directory" -- confirmed live on
		 * 192.168.15.95 the first time this exact mechanism was ever
		 * actually exercised). Reuses extra1's own already-staged
		 * fixture file and real sha256 from test 15 above, just
		 * addressed via a URL with "?ref=deadbeef" appended -- curl's
		 * own file:// handling ignores/strips a query string exactly
		 * like a real HTTP(S) fetch would, confirmed directly, so this
		 * exercises the real bug without needing a live HTTP server. */
		{
			char extra1_url_with_query[600];

			snprintf(extra1_url_with_query, sizeof(extra1_url_with_query), "%s?ref=deadbeef",
			         extra1_path);
			if (write_multisrc_recipe("multisrcquery", "1.0", ms_tarball, ms_tarball_sha,
			                           extra1_url_with_query, extra1_sha, extra2_path,
			                           extra2_sha) != 0) {
				fprintf(stderr, "FAIL: could not write multisrcquery recipe\n");
				ok = 0;
			} else {
				memset(&r, 0, sizeof(r));
				if (kx_client_request(&client, "POST", "/v1/pkg/install",
				                       "{\"name\":\"multisrcquery\"}", &r) != 0 ||
				    r.status != 202) {
					fprintf(stderr, "FAIL: POST install multisrcquery, status=%d\n", r.status);
					ok = 0;
				}
				kx_response_free(&r);

				if (poll_pkg_state(&client, "multisrcquery", state, sizeof(state), 30) != 0 ||
				    strcmp(state, "installed") != 0) {
					fprintf(stderr,
					        "FAIL: multisrcquery ended in state '%s', expected installed "
					        "(query-string basename bug)\n",
					        state);
					ok = 0;
				} else {
					char content[64] = { 0 };
					FILE *cf = fopen(base_path("/usr/share/multisrc/extra1.txt"), "r");

					if (cf == NULL || fgets(content, sizeof(content), cf) == NULL ||
					    strcmp(content, "extra-content-one\n") != 0) {
						fprintf(stderr,
						        "FAIL: multisrcquery's extra1.txt missing or wrong content, "
						        "got: %s\n",
						        content);
						ok = 0;
					}
					if (cf != NULL)
						fclose(cf);
				}
			}
		}
	}

	/* 16. real recipe management via the API (ADR-0040), not just
	 * hand-written files on disk like every fixture above -- the actual
	 * fix for "a fresh install has no recipes and no way to add one
	 * short of a full OS reinstall". POST validates before touching
	 * disk (name/pkg_name= mismatch, and outright malformed content,
	 * must both fail with nothing written); a valid recipe added this
	 * way must be genuinely installable, not just accepted; upsert
	 * (adding the same name again) must actually replace the content;
	 * DELETE must remove it and make a subsequent install fail again. */
	{
		char api_tarball[512], api_sha[128];
		char body[2048];
		char body2[2048];
		struct json_writer w;

		if (stage_fixture_tarball(scratch_dir, "apirecipe", "1.0", api_tarball,
		                           sizeof(api_tarball), api_sha, sizeof(api_sha)) != 0) {
			fprintf(stderr, "FAIL: could not stage apirecipe fixture\n");
			ok = 0;
			goto skip_recipe_api;
		}
		snprintf(body, sizeof(body),
		         "pkg_name=apirecipe\npkg_version=1.0\npkg_source=file://%s\n"
		         "pkg_sha256=%s\npkg_depends=\"\"\n\n"
		         "pkg_build() {\n\tgcc -o hello hello.c\n}\n\n"
		         "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
		         "\"$PKG_DESTDIR/usr/bin/apirecipe\"\n}\n",
		         api_tarball, api_sha);

		/* name/pkg_name= mismatch -> 400, nothing written */
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "wrongname");
		jw_key(&w, "content");
		jw_str(&w, body);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: POST recipe with name/pkg_name= mismatch, status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
		jw_free(&w);

		/* outright malformed content (no pkg_source=) -> 400 */
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "malformed");
		jw_key(&w, "content");
		jw_str(&w, "pkg_name=malformed\npkg_version=1.0\n");
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: POST malformed recipe, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
		jw_free(&w);

		/* a real, valid add -> 204, then genuinely installable */
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "apirecipe");
		jw_key(&w, "content");
		jw_str(&w, body);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: POST valid recipe via API, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);
		jw_free(&w);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"apirecipe\"}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install apirecipe (added via API), status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "apirecipe", state, sizeof(state), 30) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: apirecipe ended in state '%s', expected installed\n", state);
			ok = 0;
		} else {
			struct stat st;

			if (stat(base_path("/usr/bin/apirecipe"), &st) != 0) {
				fprintf(stderr, "FAIL: apirecipe binary missing from base image\n");
				ok = 0;
			}
		}

		/* ADR-0107: publish a second, distinct version of the same
		 * name -- immutable-per-version, not an upsert, so this must
		 * NOT reject the first version (a genuinely different
		 * pkg_version=) and both versions must coexist afterward. */
		{
			snprintf(body2, sizeof(body2),
			         "pkg_name=apirecipe\npkg_version=2.0\npkg_source=file://%s\n"
			         "pkg_sha256=%s\npkg_depends=\"\"\n\n"
			         "pkg_build() {\n\tgcc -o hello hello.c\n}\n\n"
			         "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
			         "\"$PKG_DESTDIR/usr/bin/apirecipe\"\n}\n",
			         api_tarball, api_sha);
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "apirecipe");
			jw_key(&w, "content");
			jw_str(&w, body2);
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
			    r.status != 204) {
				fprintf(stderr, "FAIL: upsert apirecipe to 2.0, status=%d\n", r.status);
				ok = 0;
			}
			kx_response_free(&r);
			jw_free(&w);
		}
		/* ADR-0107: recipe versions are immutable and multi-version,
		 * not upsert-by-name -- the list must now show BOTH 1.0 and
		 * 2.0 as separate, independently-published entries for
		 * "apirecipe", neither one replacing the other. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/recipes", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET recipes after publishing a second version, status=%d\n",
			        r.status);
			ok = 0;
		} else {
			const struct json_value *recipes = json_object_get(r.json, "recipes");
			size_t i;
			int found_v1 = 0, found_v2 = 0;

			for (i = 0; recipes != NULL && i < recipes->u.array.count; i++) {
				const struct json_value *item = recipes->u.array.items[i];

				if (!str_eq(json_str_field(item, "name"), "apirecipe"))
					continue;
				if (str_eq(json_str_field(item, "version"), "1.0"))
					found_v1 = 1;
				else if (str_eq(json_str_field(item, "version"), "2.0"))
					found_v2 = 1;
			}
			if (!found_v1 || !found_v2) {
				fprintf(stderr,
				        "FAIL: apirecipe recipe list missing a version after publishing "
				        "2.0 (v1.0 present=%d, v2.0 present=%d)\n",
				        found_v1, found_v2);
				ok = 0;
			}
		}
		kx_response_free(&r);

		/* GET /v1/pkg/recipes/{name} (Phase 16, packages-tree UI): the
		 * raw .recipe text too, not just the list view's metadata --
		 * must reflect the 2.0 upsert above, byte for byte. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/recipes/apirecipe", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET recipe content, status=%d\n", r.status);
			ok = 0;
		} else if (!str_eq(json_str_field(r.json, "name"), "apirecipe") ||
		           !str_eq(json_str_field(r.json, "version"), "2.0") ||
		           !str_eq(json_str_field(r.json, "content"), body2)) {
			fprintf(stderr, "FAIL: GET recipe content mismatch after upsert\n");
			ok = 0;
		}
		kx_response_free(&r);

		/* An unknown recipe name -> 404, not a raw-id-style fallback
		 * (there is no such fallback for recipes -- a name either has
		 * a recipe on file or it doesn't). */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/recipes/never-added-recipe", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: GET unknown recipe content expected 404, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* DELETE removes it; a subsequent install attempt fails again */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/pkg/recipes/apirecipe", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE apirecipe recipe, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* GET after DELETE -> 404 too (recipe genuinely gone, not just
		 * uninstalled). */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/recipes/apirecipe", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: GET recipe content after delete expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"apirecipe2\"}", &r) !=
		        0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: install of a never-added name should 400, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* DELETE of something never added -> 404 */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/pkg/recipes/apirecipe2", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: DELETE of a never-added recipe should 404, status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
skip_recipe_api:

	/*
	 * 16.5. ADR-0107 rolling auto-rebuild (task #720): an image whose
	 * manifest tracks a package as "rolling" must pick up a newer
	 * recipe version automatically the moment it's published --
	 * publishing 2.0 below is the ONLY action this scenario takes; no
	 * POST /v1/pkg/install is ever issued for the upgrade itself,
	 * proving the daemon's own trigger (pkg_recipe_add() ->
	 * queue_rolling_rebuilds_for() -> pkg_try_start_queued_rebuild())
	 * did the work, not the test driving it directly.
	 */
	{
		char roll_image_dir[PATH_MAX];
		char tarball1[512], sha1[128];
		char tarball2[512], sha2[128];
		char body2[2048];
		struct json_writer w;
		char state[32];
		int i;

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/images", "{\"name\":\"rollingtest\"}", &r) !=
		        0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST rollingtest image, status=%d\n", r.status);
			ok = 0;
			goto skip_rolling_rebuild;
		}
		kx_response_free(&r);

		if (stage_fixture_tarball(scratch_dir, "rollpkg", "1.0", tarball1, sizeof(tarball1), sha1,
		                           sizeof(sha1)) != 0 ||
		    write_recipe("rollpkg", "1.0", tarball1, sha1, NULL) != 0) {
			fprintf(stderr, "FAIL: could not stage/write rollpkg 1.0\n");
			ok = 0;
			goto skip_rolling_rebuild;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"rollpkg\",\"image\":\"rollingtest\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install rollpkg@rollingtest, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "rollpkg@rollingtest", state, sizeof(state), 60) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: rollpkg@rollingtest (1.0) did not reach installed\n");
			ok = 0;
			goto skip_rolling_rebuild;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/images/rollingtest/manifest",
		                       "{\"package\":\"rollpkg\",\"mode\":\"rolling\",\"version\":\"1.0\"}",
		                       &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: POST rollingtest manifest (rollpkg rolling@1.0), status=%d\n",
			        r.status);
			ok = 0;
			goto skip_rolling_rebuild;
		}
		kx_response_free(&r);

		/* Read rollingtest's own current_version now, before the
		 * rebuild -- immutability means this exact directory must
		 * still exist, byte-for-byte, after the rebuild too. */
		snprintf(roll_image_dir, sizeof(roll_image_dir), "%s/rebuildable/images/rollingtest", g_data_dir);
		{
			char old_version[128], old_rootfs_check[PATH_MAX];
			struct stat old_st;

			if (test_image_fixture_read_current_version(roll_image_dir, old_version,
			                                             sizeof(old_version)) != 0) {
				fprintf(stderr, "FAIL: could not read rollingtest's pre-rebuild version\n");
				ok = 0;
				goto skip_rolling_rebuild;
			}

			if (stage_fixture_tarball(scratch_dir, "rollpkg", "2.0", tarball2, sizeof(tarball2),
			                           sha2, sizeof(sha2)) != 0) {
				fprintf(stderr, "FAIL: could not stage rollpkg 2.0\n");
				ok = 0;
				goto skip_rolling_rebuild;
			}
			snprintf(body2, sizeof(body2),
			         "pkg_name=rollpkg\npkg_version=2.0\npkg_source=file://%s\n"
			         "pkg_sha256=%s\npkg_depends=\"\"\n\n"
			         "pkg_build() {\n\tgcc -o hello hello.c\n}\n\n"
			         "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
			         "\"$PKG_DESTDIR/usr/bin/rollpkg\"\n}\n",
			         tarball2, sha2);
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "rollpkg");
			jw_key(&w, "content");
			jw_str(&w, body2);
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
			    r.status != 204) {
				fprintf(stderr, "FAIL: publish rollpkg 2.0, status=%d\n", r.status);
				ok = 0;
			}
			kx_response_free(&r);
			jw_free(&w);

			/* No install/upgrade request follows -- the daemon's own
			 * rolling-rebuild trigger must do this by itself. */
			state[0] = '\0';
			for (i = 0; i < 60; i++) {
				memset(&r, 0, sizeof(r));
				if (kx_client_request(&client, "GET", "/v1/pkg/rollpkg@rollingtest", NULL, &r) ==
				        0 &&
				    r.status == 200) {
					const char *st = json_str_field(r.json, "state");
					const char *ver = json_str_field(r.json, "version");

					if (st != NULL)
						snprintf(state, sizeof(state), "%s", st);
					if (st != NULL && strcmp(st, "installed") == 0 && ver != NULL &&
					    strcmp(ver, "2.0") == 0) {
						kx_response_free(&r);
						break;
					}
				}
				kx_response_free(&r);
				usleep(300000);
			}
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "GET", "/v1/pkg/rollpkg@rollingtest", NULL, &r) != 0 ||
			    r.status != 200 || !str_eq(json_str_field(r.json, "state"), "installed") ||
			    !str_eq(json_str_field(r.json, "version"), "2.0")) {
				fprintf(stderr,
				        "FAIL: rollpkg@rollingtest was not auto-rebuilt to 2.0 (last state "
				        "seen: %s)\n",
				        state);
				ok = 0;
			}
			kx_response_free(&r);

			/* The old version's own rootfs must still exist, untouched
			 * -- copy-forward immutability (ADR-0107/0108), not
			 * mutated in place by the auto-rebuild. */
			snprintf(old_rootfs_check, sizeof(old_rootfs_check), "%s/%s/rootfs", roll_image_dir,
			         old_version);
			if (stat(old_rootfs_check, &old_st) != 0) {
				fprintf(stderr,
				        "FAIL: rollingtest's pre-rebuild version rootfs no longer exists "
				        "(auto-rebuild mutated it in place instead of copy-forwarding)\n");
				ok = 0;
			}

			/* current_version must have actually moved. */
			{
				char new_version[128];

				if (test_image_fixture_read_current_version(roll_image_dir, new_version,
				                                              sizeof(new_version)) != 0 ||
				    strcmp(new_version, old_version) == 0) {
					fprintf(stderr,
					        "FAIL: rollingtest's current_version did not advance after "
					        "the auto-rebuild\n");
					ok = 0;
				}
			}
		}
	}
skip_rolling_rebuild:

	/*
	 * 16.6. ADR-0107/0108 per-version rootfs isolation (task #722): the
	 * core guarantee the whole versioning epic exists to provide -- a
	 * container created against an image stays pinned to the exact
	 * rootfs it was created with, byte-for-byte, even after that image
	 * name is later upgraded to a new version. Proven two-sided: an
	 * EXISTING container (created before the upgrade) must keep seeing
	 * the OLD content; a FRESH container (created after) must see the
	 * NEW content -- both against the very same image name.
	 */
	{
		char pin_image_dir[PATH_MAX];
		char tarball1[512], sha1[128];
		char tarball2[512], sha2[128];
		char v1_version[128], v2_version[128];

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/images", "{\"name\":\"pintest\"}", &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST pintest image, status=%d\n", r.status);
			ok = 0;
			goto skip_pin_isolation;
		}
		kx_response_free(&r);

		if (stage_fixture_tarball(scratch_dir, "pinpkg", "1.0", tarball1, sizeof(tarball1), sha1,
		                           sizeof(sha1)) != 0 ||
		    write_recipe("pinpkg", "1.0", tarball1, sha1, NULL) != 0) {
			fprintf(stderr, "FAIL: could not stage/write pinpkg 1.0\n");
			ok = 0;
			goto skip_pin_isolation;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"pinpkg\",\"image\":\"pintest\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install pinpkg@pintest, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "pinpkg@pintest", state, sizeof(state), 60) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: pinpkg@pintest (1.0) did not reach installed\n");
			ok = 0;
			goto skip_pin_isolation;
		}

		snprintf(pin_image_dir, sizeof(pin_image_dir), "%s/rebuildable/images/pintest", g_data_dir);
		if (test_image_fixture_read_current_version(pin_image_dir, v1_version,
		                                             sizeof(v1_version)) != 0) {
			fprintf(stderr, "FAIL: could not read pintest's version after installing 1.0\n");
			ok = 0;
			goto skip_pin_isolation;
		}

		/* Create a container against pintest BEFORE the upgrade below --
		 * it must pin to v1_version (registry_entry.image_version,
		 * ADR-0107/0108). cmd exits almost immediately (a real
		 * from-source build of the fixture's own hello.c, see
		 * stage_fixture_tarball()), so a short wait after create is
		 * enough for registry_mark_exited() to run and for the
		 * exited-container GET .../files fallback path
		 * (handle_container_file_read()) to be the one actually
		 * exercised below -- the same fallback a real operator's
		 * stopped/restarted container would hit. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"pintest-old\",\"image\":\"pintest\","
		                       "\"cmd\":[\"/usr/bin/pinpkg\"]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST pintest-old, status=%d\n", r.status);
			ok = 0;
			goto skip_pin_isolation;
		}
		kx_response_free(&r);
		usleep(500000);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/containers/pintest-old", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "image_version"), v1_version)) {
			fprintf(stderr,
			        "FAIL: pintest-old should be pinned to %s, image_version=%s\n", v1_version,
			        json_str_field(r.json, "image_version") ? json_str_field(r.json, "image_version")
			                                                 : "(null)");
			ok = 0;
		}
		kx_response_free(&r);

		/* Now upgrade pintest to 2.0 -- produces a NEW immutable
		 * current_version; pintest-old's own overlay lowerdir must stay
		 * exactly what it was pinned to above. */
		if (stage_fixture_tarball(scratch_dir, "pinpkg", "2.0", tarball2, sizeof(tarball2), sha2,
		                           sizeof(sha2)) != 0 ||
		    write_recipe("pinpkg", "2.0", tarball2, sha2, NULL) != 0) {
			fprintf(stderr, "FAIL: could not stage/write pinpkg 2.0\n");
			ok = 0;
			goto skip_pin_isolation;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"pinpkg\",\"image\":\"pintest\",\"upgrade\":true}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST upgrade pinpkg@pintest, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "pinpkg@pintest", state, sizeof(state), 60) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: pinpkg@pintest (2.0) did not reach installed\n");
			ok = 0;
			goto skip_pin_isolation;
		}

		if (test_image_fixture_read_current_version(pin_image_dir, v2_version,
		                                             sizeof(v2_version)) != 0 ||
		    strcmp(v2_version, v1_version) == 0) {
			fprintf(stderr, "FAIL: pintest's current_version did not advance after upgrade\n");
			ok = 0;
			goto skip_pin_isolation;
		}

		/* pintest-old's own registry pin must be UNCHANGED by the
		 * upgrade -- it was resolved once at create time, never
		 * re-resolved by a later, unrelated pkg install. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/containers/pintest-old", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "image_version"), v1_version)) {
			fprintf(stderr,
			        "FAIL: pintest-old's image_version changed after pintest's upgrade "
			        "(was %s, now %s) -- pinning broke\n",
			        v1_version,
			        json_str_field(r.json, "image_version") ? json_str_field(r.json, "image_version")
			                                                 : "(null)");
			ok = 0;
		}
		kx_response_free(&r);

		/* The real proof: pintest-old's own /usr/bin/pinpkg must still
		 * be the OLD binary (embeds "hello from pinpkg v1.0" in its own
		 * rodata, see stage_fixture_tarball()) -- read via GET
		 * .../files, which for an exited container falls back to
		 * e->image_version's own immutable rootfs
		 * (handle_container_file_read(), ADR-0107/0108), never
		 * pintest's current (now 2.0) rootfs. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET",
		                       "/v1/containers/pintest-old/files?path=%2Fusr%2Fbin%2Fpinpkg", NULL,
		                       &r) != 0 ||
		    r.status != 200 || r.body == NULL ||
		    memmem(r.body, r.body_len, "hello from pinpkg v1.0", strlen("hello from pinpkg v1.0")) ==
		        NULL) {
			fprintf(stderr,
			        "FAIL: pintest-old's /usr/bin/pinpkg no longer reflects v1.0 after "
			        "pintest's own upgrade to 2.0 -- per-version isolation broke\n");
			ok = 0;
		}
		kx_response_free(&r);

		/* A FRESH container created now, against the very same image
		 * name, must pin to the NEW version and see the NEW binary --
		 * completing the two-sided proof. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"pintest-new\",\"image\":\"pintest\","
		                       "\"cmd\":[\"/usr/bin/pinpkg\"]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST pintest-new, status=%d\n", r.status);
			ok = 0;
			goto skip_pin_isolation;
		}
		kx_response_free(&r);
		usleep(500000);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/containers/pintest-new", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "image_version"), v2_version)) {
			fprintf(stderr, "FAIL: pintest-new should be pinned to %s, image_version=%s\n",
			        v2_version,
			        json_str_field(r.json, "image_version") ? json_str_field(r.json, "image_version")
			                                                 : "(null)");
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET",
		                       "/v1/containers/pintest-new/files?path=%2Fusr%2Fbin%2Fpinpkg", NULL,
		                       &r) != 0 ||
		    r.status != 200 || r.body == NULL ||
		    memmem(r.body, r.body_len, "hello from pinpkg v2.0", strlen("hello from pinpkg v2.0")) ==
		        NULL) {
			fprintf(stderr, "FAIL: pintest-new's /usr/bin/pinpkg does not reflect v2.0\n");
			ok = 0;
		}
		kx_response_free(&r);
	}
skip_pin_isolation:

	/* 17. hostbuild (ADR-0056): a second mode of the same pipeline that
	 * harvests pkg_install()'s output into ARTIFACTS_DIR/<name>/ instead
	 * of merging it into any image's rootfs, using a named image's own
	 * rootfs as the build container's lowerdir instead of the shared
	 * toolchain sandbox. Proves: a real artifact lands on disk at the
	 * documented path; PKG_ERR_BUSY is enforced against hostbuild the
	 * same way it already is between two ordinary installs (step 4);
	 * a hostbuild recipe with a non-empty pkg_depends is rejected
	 * outright (dependency resolution has no meaning for a one-shot
	 * artifact harvest -- every prerequisite must already be in
	 * build_image's own rootfs). Not a kernel build (far too slow for
	 * this suite) -- the same trivial gcc-a-hello-world fixture every
	 * other step here already uses, just routed through the hostbuild
	 * entry point instead of an ordinary install.
	 */
	{
		char hb_image_rootfs[PATH_MAX];
		char hb_recipe_path[PATH_MAX];
		char hb_artifact_file[PATH_MAX];
		char hb_state[32];
		FILE *f;
		int i;
		struct stat st;

		/*
		 * ADR-0107/0108: build_image must resolve via a real
		 * manifest.json + versioned rootfs, same as any other image
		 * -- pkg_hostbuild_start()/pkg_fetch_completed() now reach it
		 * through image_current_version()/image_version_rootfs_path(),
		 * not a flat "<image>/rootfs" path. This fixture bypasses the
		 * daemon's own image_create()/pkg install pipeline (staging
		 * a toolchain via a real install would be far too slow for
		 * this suite), so it hand-writes the same manifest.json shape
		 * image.c itself produces, pointed at a fixed, made-up version
		 * hash -- image.c's own load_state() only ever reads this
		 * field back as an opaque string, never re-derives or
		 * validates it as a real sha256, so a fixture-chosen literal
		 * is exactly as valid as a real one.
		 */
		snprintf(hb_image_rootfs, sizeof(hb_image_rootfs), "%s/rebuildable/images/hbimage/hbfixture/rootfs",
		         g_data_dir);
		if (test_image_fixture_stage_toolchain(hb_image_rootfs) != 0) {
			fprintf(stderr, "FAIL: could not stage hostbuild build_image toolchain\n");
			ok = 0;
			goto skip_hostbuild;
		}
		{
			char hb_manifest_path[PATH_MAX];

			snprintf(hb_manifest_path, sizeof(hb_manifest_path), "%s/rebuildable/images/hbimage/manifest.json",
			         g_data_dir);
			f = fopen(hb_manifest_path, "w");
			if (f == NULL) {
				fprintf(stderr, "FAIL: could not write hbimage manifest.json\n");
				ok = 0;
				goto skip_hostbuild;
			}
			fprintf(f,
			        "{\"packages\":[],\"current_version\":\"hbfixture\","
			        "\"versions\":[{\"version\":\"hbfixture\",\"created_at\":%ld}]}",
			        (long)time(NULL));
			fclose(f);
		}

		/* A plain hand-written recipe (not stage_fixture_tarball(), no
		 * DESTDIR/usr/bin convention needed -- pkg_install() below
		 * just drops its output at a fixed, predictable name). */
		{
			char hb_recipe_dir[PATH_MAX];

			snprintf(hb_recipe_dir, sizeof(hb_recipe_dir), "%s/recipes/hbtest", g_pkg_state_dir);
			mkdir(hb_recipe_dir, 0755);
			snprintf(hb_recipe_dir, sizeof(hb_recipe_dir), "%s/recipes/hbtest/1.0", g_pkg_state_dir);
			mkdir(hb_recipe_dir, 0755);
		}
		snprintf(hb_recipe_path, sizeof(hb_recipe_path), "%s/recipes/hbtest/1.0/build.sh",
		         g_pkg_state_dir);
		f = fopen(hb_recipe_path, "w");
		if (f == NULL) {
			fprintf(stderr, "FAIL: could not write hbtest.recipe\n");
			ok = 0;
			goto skip_hostbuild;
		}
		fprintf(f, "pkg_name=hbtest\npkg_version=1.0\npkg_source=file://%s\n"
		           "pkg_sha256=%s\npkg_depends=\"\"\n\n"
		           "pkg_build() {\n\tgcc -o hello hello.c\n}\n\n"
		           "pkg_install() {\n\tcp hello \"$PKG_DESTDIR/hello\"\n}\n",
		        tarball_path, sha256);
		fclose(f);

		/* start it -> 202, fetching */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/hostbuild",
		                       "{\"name\":\"hbtest\",\"build_image\":\"hbimage\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST hostbuild hbtest, status=%d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* ADR-0157 Phase 2: while it's in flight, a concurrent
		 * *ordinary* install now genuinely fits in the second chain
		 * slot -> 202, mirroring step 4's own top-level proof in the
		 * other direction (a hostbuild and an ordinary install sharing
		 * the two chain slots, not two ordinary installs). A fresh,
		 * never-yet-installed name ("hbconcurrent") -- unlike badsum,
		 * this one has a real, valid tarball/checksum, so it takes a
		 * genuine gcc build to finish rather than failing fast on a
		 * checksum mismatch during the fetch step alone; the overflow
		 * check right below needs both chains to still be provably
		 * busy by the time it fires, which a same-tick fetch failure
		 * could otherwise race past (confirmed live: badsum's own
		 * checksum failure was fast enough to free its chain slot
		 * before the overflow request even landed). */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"hbconcurrent\"}", &r) !=
		        0 ||
		    r.status != 202 || !str_eq(json_str_field(r.json, "state"), "fetching")) {
			fprintf(stderr,
			        "FAIL: ordinary install alongside hostbuild (2nd chain) status=%d, state=%s\n",
			        r.status, json_str_field(r.json, "state") ? json_str_field(r.json, "state") : "(null)");
			ok = 0;
		}
		kx_response_free(&r);

		/* a THIRD job attempt now that both chain slots are genuinely
		 * occupied (hbtest's hostbuild + hbconcurrent's fetch/build)
		 * -> 409 -- "overflow" is the same never-yet-installed recipe
		 * step 4b already proved gets rejected the same way at the
		 * top level. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"overflow\"}", &r) !=
		        0 ||
		    r.status != 409) {
			fprintf(stderr,
			        "FAIL: overflow install with both chains busy (hostbuild case) expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* poll GET /v1/pkg/hostbuild/{name} (the dedicated route, not
		 * the generic /v1/pkg/{name} -- that one has no way to say
		 * "look under the __hostbuild image" without a name@image
		 * suffix) until it leaves fetching/building. 100 attempts
		 * (30s), not the original 30 (9s) -- ADR-0157 Phase 2 made
		 * this poll window genuinely share the box with a second, real
		 * concurrent gcc build (hbconcurrent, below), so the old
		 * single-build-tuned timeout is now marginal under real CPU
		 * contention, confirmed live (intermittent, non-deterministic
		 * timeouts here across repeated runs, never a wrong RESULT,
		 * always just "didn't finish within the old window yet"). */
		hb_state[0] = '\0';
		for (i = 0; i < 100; i++) {
			const char *state;

			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "GET", "/v1/pkg/hostbuild/hbtest", NULL, &r) != 0 ||
			    r.status != 200) {
				kx_response_free(&r);
				break;
			}
			state = json_str_field(r.json, "state");
			if (state == NULL) {
				kx_response_free(&r);
				break;
			}
			snprintf(hb_state, sizeof(hb_state), "%s", state);
			kx_response_free(&r);
			if (strcmp(hb_state, "fetching") != 0 && strcmp(hb_state, "building") != 0)
				break;
			usleep(300000);
		}
		if (strcmp(hb_state, "installed") != 0) {
			fprintf(stderr, "FAIL: hbtest hostbuild ended in state '%s', expected installed\n",
			        hb_state);
			ok = 0;
			goto skip_hostbuild;
		}

		/* hbconcurrent's own chain (started alongside hbtest's
		 * hostbuild above) must also have finished by now -- drained
		 * here, before the hbdepstest busy-check further below, which
		 * depends on BOTH chain slots genuinely being free again. */
		if (poll_pkg_state(&client, "hbconcurrent", state, sizeof(state), 60) != 0) {
			fprintf(stderr, "FAIL: hbconcurrent (alongside hostbuild) never left fetching/building\n");
			ok = 0;
		} else if (strcmp(state, "installed") != 0) {
			fprintf(stderr,
			        "FAIL: hbconcurrent (alongside hostbuild) ended in state '%s', expected installed\n",
			        state);
			ok = 0;
		} else if (stat(base_path("/usr/bin/hbconcurrent"), &st) != 0) {
			fprintf(stderr, "FAIL: hbconcurrent binary missing from base image after install\n");
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", "/v1/pkg/hbconcurrent", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE hbconcurrent (2nd chain) expected 204, got %d\n", r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* the response itself must say is_hostbuild=true and give the
		 * real artifact_path -- not just that the job finished. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/hostbuild/hbtest", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET hostbuild/hbtest after completion, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *ib = json_object_get(r.json, "is_hostbuild");
			const char *artifact_path = json_str_field(r.json, "artifact_path");
			const struct json_value *files = json_object_get(r.json, "files");

			if (ib == NULL || ib->type != JSON_BOOL || !ib->u.boolean) {
				fprintf(stderr, "FAIL: hbtest is_hostbuild not true\n");
				ok = 0;
			}
			if (artifact_path == NULL) {
				fprintf(stderr, "FAIL: hbtest artifact_path is null after completion\n");
				ok = 0;
			} else {
				snprintf(hb_artifact_file, sizeof(hb_artifact_file), "%s/hello", artifact_path);
			}
			/* issue #6: files used to always be empty for a hostbuild
			 * entry -- confirm it now reports what was really
			 * produced, not just that the job finished. */
			if (files == NULL || files->type != JSON_ARRAY || files->u.array.count == 0) {
				fprintf(stderr, "FAIL: hbtest files field is empty, expected \"hello\"\n");
				ok = 0;
			} else {
				const char *first = json_as_string(files->u.array.items[0]);

				if (first == NULL || strcmp(first, "hello") != 0) {
					fprintf(stderr, "FAIL: hbtest files[0] = '%s', expected \"hello\"\n",
					        first != NULL ? first : "(null)");
					ok = 0;
				}
			}
		}
		kx_response_free(&r);

		/* the real payoff: a real file landed on disk under
		 * ARTIFACTS_DIR/hbtest/ -- not merged into any image's
		 * rootfs (base/router's own rootfs must NOT have gained a
		 * stray "hello" file from this). */
		if (stat(hb_artifact_file, &st) != 0 || !S_ISREG(st.st_mode)) {
			fprintf(stderr, "FAIL: hostbuild artifact missing on disk at '%s'\n",
			        hb_artifact_file);
			ok = 0;
		}
		if (stat(base_path("/hello"), &st) == 0) {
			fprintf(stderr, "FAIL: hostbuild output leaked into the base image's rootfs\n");
			ok = 0;
		}

		/* a hostbuild recipe with a non-empty pkg_depends must be
		 * rejected outright -- dependency resolution targets "merge
		 * into an image," meaningless for a one-shot harvest. The
		 * prior job is done by now (not busy), so this genuinely
		 * exercises the depends check, not PKG_ERR_BUSY. */
		{
			char hb_recipe_dir[PATH_MAX];

			snprintf(hb_recipe_dir, sizeof(hb_recipe_dir), "%s/recipes/hbdepstest",
			         g_pkg_state_dir);
			mkdir(hb_recipe_dir, 0755);
			snprintf(hb_recipe_dir, sizeof(hb_recipe_dir), "%s/recipes/hbdepstest/1.0",
			         g_pkg_state_dir);
			mkdir(hb_recipe_dir, 0755);
		}
		snprintf(hb_recipe_path, sizeof(hb_recipe_path), "%s/recipes/hbdepstest/1.0/build.sh",
		         g_pkg_state_dir);
		f = fopen(hb_recipe_path, "w");
		if (f == NULL) {
			fprintf(stderr, "FAIL: could not write hbdepstest.recipe\n");
			ok = 0;
			goto skip_hostbuild;
		}
		fprintf(f, "pkg_name=hbdepstest\npkg_version=1.0\npkg_source=file://%s\n"
		           "pkg_sha256=%s\npkg_depends=\"badsum\"\n\n"
		           "pkg_build() {\n\tgcc -o hello hello.c\n}\n\n"
		           "pkg_install() {\n\tcp hello \"$PKG_DESTDIR/hello\"\n}\n",
		        tarball_path, sha256);
		fclose(f);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/hostbuild",
		                       "{\"name\":\"hbdepstest\",\"build_image\":\"hbimage\"}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: hostbuild with non-empty pkg_depends expected 400, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* an unknown build_image -> 404, not a silent fall-through */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/hostbuild",
		                       "{\"name\":\"hbtest\",\"build_image\":\"no-such-image\"}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: hostbuild with unknown build_image expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
skip_hostbuild:

	/* ADR-0157 Phase 3: PUT /v1/system/pkg-build-config validation --
	 * still at the ceiling=2 this test set right after startup. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":0}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: PUT pkg-build-config max_concurrent_jobs=0 expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":11}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: PUT pkg-build-config max_concurrent_jobs=11 expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* Lowering the ceiling to 1 and confirming chain_alloc() actually
	 * honors it (not just the compile-time PKG_MAX_CONCURRENT_JOBS
	 * array bound) is the real proof this config is load-bearing, not
	 * just a number that gets echoed back. "overflow"/"hbconcurrent"
	 * are both still fresh, never-installed recipes at this point. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":1}", &r) != 0 ||
	    r.status != 200 || json_as_number(json_object_get(r.json, "max_concurrent_jobs")) != 1) {
		fprintf(stderr, "FAIL: PUT pkg-build-config max_concurrent_jobs=1, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"overflow\"}", &r) != 0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: POST install overflow (ceiling=1) status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"hbconcurrent\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr,
		        "FAIL: POST install hbconcurrent while ceiling=1 already busy expected 409, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (poll_pkg_state(&client, "overflow", state, sizeof(state), 60) != 0 ||
	    strcmp(state, "installed") != 0) {
		fprintf(stderr, "FAIL: overflow (ceiling=1) ended in state '%s', expected installed\n",
		        state);
		ok = 0;
	}

	/* Restored to the real default before the second install below --
	 * proves raising the ceiling back up re-admits genuine concurrency
	 * immediately, not just that lowering it worked. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":10}", &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: PUT pkg-build-config restore max_concurrent_jobs=10, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"hbconcurrent\"}", &r) !=
	        0 ||
	    r.status != 202) {
		fprintf(stderr,
		        "FAIL: POST install hbconcurrent after restoring ceiling=10 status=%d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (poll_pkg_state(&client, "hbconcurrent", state, sizeof(state), 60) != 0 ||
	    strcmp(state, "installed") != 0) {
		fprintf(stderr, "FAIL: hbconcurrent (restored ceiling) ended in state '%s'\n", state);
		ok = 0;
	}

	/* 18. keep_on_failure (ADR-0175/issue #35): a real build-container
	 * failure (not a fetch/checksum failure like badsum in step 7 --
	 * this needs a genuine build container to actually spawn) with
	 * keep_on_failure=true must leave the exited build container
	 * registered and its overlay readable instead of tearing it down;
	 * the same failure WITHOUT the flag must still tear down
	 * immediately, exactly like every pre-existing caller's behavior. */
	{
		char kf_recipe_dir[PATH_MAX];
		char kf_recipe_path[PATH_MAX];
		char kept_name[64];
		char kf_container_path[300];
		FILE *f;

		snprintf(kf_recipe_dir, sizeof(kf_recipe_dir), "%s/recipes/keepfail", g_pkg_state_dir);
		mkdir(kf_recipe_dir, 0755);
		snprintf(kf_recipe_dir, sizeof(kf_recipe_dir), "%s/recipes/keepfail/1.0", g_pkg_state_dir);
		mkdir(kf_recipe_dir, 0755);
		snprintf(kf_recipe_path, sizeof(kf_recipe_path), "%s/recipes/keepfail/1.0/build.sh",
		         g_pkg_state_dir);
		f = fopen(kf_recipe_path, "w");
		if (f == NULL) {
			fprintf(stderr, "FAIL: could not write keepfail.recipe\n");
			ok = 0;
			goto skip_keep_on_failure;
		}
		/* A real, valid source/checksum -- fetch succeeds and a real
		 * build container spawns, extracting hello.c into /build/src/
		 * before pkg_build() deliberately fails, distinguishing this
		 * from badsum's own fetch-stage-only failure (step 7). */
		fprintf(f, "pkg_name=keepfail\npkg_version=1.0\npkg_source=file://%s\n"
		           "pkg_sha256=%s\npkg_depends=\"\"\n\n"
		           "pkg_build() {\n\texit 1\n}\n\n"
		           "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n}\n",
		        tarball_path, sha256);
		fclose(f);

		/* 18a. WITHOUT keep_on_failure: unchanged pre-existing
		 * behavior -- the failed build container is gone immediately,
		 * kept_build_container stays null. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"keepfail\"}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install keepfail (no keep_on_failure) status=%d\n",
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "keepfail", state, sizeof(state), 60) != 0 ||
		    strcmp(state, "failed") != 0) {
			fprintf(stderr, "FAIL: keepfail (no keep_on_failure) ended in state '%s'\n", state);
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/keepfail", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET pkg/keepfail (no keep_on_failure) status=%d\n", r.status);
			ok = 0;
		} else if (json_object_get(r.json, "kept_build_container")->type != JSON_NULL) {
			fprintf(stderr,
			        "FAIL: keepfail (no keep_on_failure) has a non-null kept_build_container\n");
			ok = 0;
		}
		kx_response_free(&r);

		/* 18b. WITH keep_on_failure: the build container survives,
		 * readable, until an explicit DELETE. */
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"keepfail\",\"upgrade\":true,\"keep_on_failure\":true}",
		                       &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install keepfail (keep_on_failure) status=%d\n", r.status);
			ok = 0;
			goto skip_keep_on_failure;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "keepfail", state, sizeof(state), 60) != 0 ||
		    strcmp(state, "failed") != 0) {
			fprintf(stderr, "FAIL: keepfail (keep_on_failure) ended in state '%s'\n", state);
			ok = 0;
			goto skip_keep_on_failure;
		}

		memset(&r, 0, sizeof(r));
		kept_name[0] = '\0';
		if (kx_client_request(&client, "GET", "/v1/pkg/keepfail", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET pkg/keepfail (keep_on_failure) status=%d\n", r.status);
			ok = 0;
		} else {
			const char *kbc = json_str_field(r.json, "kept_build_container");

			if (kbc == NULL || strncmp(kbc, "__pkgbuild-", 11) != 0) {
				fprintf(stderr,
				        "FAIL: keepfail (keep_on_failure) kept_build_container='%s', "
				        "expected __pkgbuild-N\n",
				        kbc != NULL ? kbc : "(null)");
				ok = 0;
			} else {
				snprintf(kept_name, sizeof(kept_name), "%s", kbc);
			}
			/* the same preservation note must also be human-readable
			 * in the ordinary error field (belt and suspenders --
			 * kept_build_container is the machine-readable field an
			 * automated caller should actually parse). */
			if (strstr(json_str_field(r.json, "error"), "preserved for debugging") == NULL) {
				fprintf(stderr, "FAIL: keepfail (keep_on_failure) error text missing preservation note: %s\n",
				        json_str_field(r.json, "error") ? json_str_field(r.json, "error") : "(null)");
				ok = 0;
			}
		}
		kx_response_free(&r);

		if (kept_name[0] == '\0')
			goto skip_keep_on_failure;

		/* the preserved container is a completely ordinary, addressable
		 * exited container -- GET still finds it... */
		snprintf(kf_container_path, sizeof(kf_container_path), "/v1/containers/%s", kept_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET %s (preserved build container) status=%d\n",
			        kf_container_path, r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* ...and ADR-0055's GET .../files reads real content back out
		 * of it -- proof the overlay genuinely survived, not just the
		 * registry entry (the actual point of this whole mechanism:
		 * pulling a failed build's own output/state back out for real
		 * debugging). */
		snprintf(kf_container_path, sizeof(kf_container_path),
		         "/v1/containers/%s/files?path=%%2Fbuild%%2Fsrc%%2Fhello.c", kept_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 200 ||
		    r.body == NULL || r.body_len == 0) {
			fprintf(stderr, "FAIL: GET %s status=%d body_len=%zu\n", kf_container_path, r.status,
			        r.body_len);
			ok = 0;
		}
		kx_response_free(&r);

		/*
		 * Issue #61: the same preserved build container must also serve
		 * reads that fall through to its LOWERDIR, not just its upper
		 * (/build/* above). A build container is registered under the
		 * synthetic image name "pkgbuild" with an empty image_version,
		 * so the read fallback could not reconstruct its lowerdir from
		 * image/image_version the way an ordinary container's is -- it
		 * fell through to an EMPTY path prefix, which (a) 404'd real
		 * lowerdir content and (b) turned the read into a bare open() of
		 * the path on the DAEMON HOST's own filesystem. Both halves are
		 * asserted here.
		 *
		 * Positive: /usr/bin/tcc is staged into the build lowerdir by
		 * test_image_fixture_stage_toolchain(), so it must read back.
		 */
		snprintf(kf_container_path, sizeof(kf_container_path),
		         "/v1/containers/%s/files?path=%%2Fusr%%2Fbin%%2Ftcc", kept_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 200 ||
		    r.body == NULL || r.body_len == 0) {
			fprintf(stderr,
			        "FAIL: GET %s (build container lowerdir read) status=%d body_len=%zu\n",
			        kf_container_path, r.status, r.body_len);
			ok = 0;
		}
		kx_response_free(&r);

		/*
		 * Negative (the host-leak half): /etc/os-release exists on the
		 * daemon's own host but is NOT staged into the build lowerdir
		 * (only /etc/alternatives is). It must therefore 404 -- a 200
		 * here means the endpoint served a host file through a
		 * per-container path.
		 */
		snprintf(kf_container_path, sizeof(kf_container_path),
		         "/v1/containers/%s/files?path=%%2Fetc%%2Fos-release", kept_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr,
			        "FAIL: GET %s expected 404 -- a host file must never be readable through a "
			        "container's own files endpoint, status=%d\n",
			        kf_container_path, r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* explicit cleanup -- the completely ordinary DELETE path,
		 * no new mechanism. */
		snprintf(kf_container_path, sizeof(kf_container_path), "/v1/containers/%s", kept_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "DELETE", kf_container_path, NULL, &r) != 0 || r.status != 204) {
			fprintf(stderr, "FAIL: DELETE %s (preserved build container) status=%d\n",
			        kf_container_path, r.status);
			ok = 0;
		}
		kx_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: GET %s after DELETE expected 404, got %d\n", kf_container_path,
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
skip_keep_on_failure:

	/* 19. resume (ADR-0177/issue #46): a build container preserved via
	 * keep_on_failure can be resumed in place under a fixed recipe
	 * version, WITHOUT re-extracting its source tree -- proven by
	 * having the first (deliberately failing) build leave a marker
	 * file in /build/src that only a genuine reuse (never touched by
	 * reset_build_container_dir()/extract_tarball(), which a fresh
	 * restart would run) would still have; the resumed recipe's own
	 * pkg_build() fails loudly if that marker is missing. */
	{
		char rs_recipe_dir[PATH_MAX];
		char rs_recipe_path[PATH_MAX];
		char kept_name[64];
		char rs_container_path[300];
		FILE *f;

		snprintf(rs_recipe_dir, sizeof(rs_recipe_dir), "%s/recipes/resumeme", g_pkg_state_dir);
		mkdir(rs_recipe_dir, 0755);
		snprintf(rs_recipe_dir, sizeof(rs_recipe_dir), "%s/recipes/resumeme/1.0", g_pkg_state_dir);
		mkdir(rs_recipe_dir, 0755);
		snprintf(rs_recipe_path, sizeof(rs_recipe_path), "%s/recipes/resumeme/1.0/build.sh",
		         g_pkg_state_dir);
		f = fopen(rs_recipe_path, "w");
		if (f == NULL) {
			fprintf(stderr, "FAIL: could not write resumeme 1.0 recipe\n");
			ok = 0;
			goto skip_resume;
		}
		fprintf(f, "pkg_name=resumeme\npkg_version=1.0\npkg_source=file://%s\n"
		           "pkg_sha256=%s\npkg_depends=\"\"\n\n"
		           "pkg_build() {\n\ttouch /build/src/.resumed_marker\n\texit 1\n}\n\n"
		           "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n}\n",
		        tarball_path, sha256);
		fclose(f);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"resumeme\",\"keep_on_failure\":true}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install resumeme (v1.0) status=%d\n", r.status);
			ok = 0;
			goto skip_resume;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "resumeme", state, sizeof(state), 60) != 0 ||
		    strcmp(state, "failed") != 0) {
			fprintf(stderr, "FAIL: resumeme (v1.0) ended in state '%s'\n", state);
			ok = 0;
			goto skip_resume;
		}

		memset(&r, 0, sizeof(r));
		kept_name[0] = '\0';
		if (kx_client_request(&client, "GET", "/v1/pkg/resumeme", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET pkg/resumeme (v1.0) status=%d\n", r.status);
			ok = 0;
		} else {
			const char *kbc = json_str_field(r.json, "kept_build_container");

			if (kbc == NULL || strncmp(kbc, "__pkgbuild-", 11) != 0) {
				fprintf(stderr, "FAIL: resumeme (v1.0) kept_build_container='%s'\n",
				        kbc != NULL ? kbc : "(null)");
				ok = 0;
			} else {
				snprintf(kept_name, sizeof(kept_name), "%s", kbc);
			}
		}
		kx_response_free(&r);

		if (kept_name[0] == '\0')
			goto skip_resume;

		/* the marker really is there -- proof the container's own
		 * overlay genuinely has real build state, not just an empty
		 * preserved shell. */
		snprintf(rs_container_path, sizeof(rs_container_path),
		         "/v1/containers/%s/files?path=%%2Fbuild%%2Fsrc%%2F.resumed_marker", kept_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", rs_container_path, NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET %s (marker before resume) status=%d\n", rs_container_path,
			        r.status);
			ok = 0;
		}
		kx_response_free(&r);

		/* publish a "fixed" 1.1 -- its own pkg_build() checks the
		 * marker survived (i.e. this really is a resume, not a
		 * disguised fresh restart) and, if so, succeeds. */
		snprintf(rs_recipe_dir, sizeof(rs_recipe_dir), "%s/recipes/resumeme/1.1", g_pkg_state_dir);
		mkdir(rs_recipe_dir, 0755);
		snprintf(rs_recipe_path, sizeof(rs_recipe_path), "%s/recipes/resumeme/1.1/build.sh",
		         g_pkg_state_dir);
		f = fopen(rs_recipe_path, "w");
		if (f == NULL) {
			fprintf(stderr, "FAIL: could not write resumeme 1.1 recipe\n");
			ok = 0;
			goto skip_resume;
		}
		fprintf(f, "pkg_name=resumeme\npkg_version=1.1\npkg_source=file://%s\n"
		           "pkg_sha256=%s\npkg_depends=\"\"\n\n"
		           "pkg_build() {\n\t[ -f /build/src/.resumed_marker ] || exit 1\n}\n\n"
		           "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n}\n",
		        tarball_path, sha256);
		fclose(f);

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "POST", "/v1/pkg/resume", "{\"name\":\"resumeme\"}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST resume resumeme status=%d body=%s\n", r.status,
			        r.body != NULL ? r.body : "(null)");
			ok = 0;
			goto skip_resume;
		}
		kx_response_free(&r);

		if (poll_pkg_state(&client, "resumeme", state, sizeof(state), 60) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: resumeme (resumed) ended in state '%s'\n", state);
			ok = 0;
			goto skip_resume;
		}

		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/resumeme", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET pkg/resumeme (resumed) status=%d\n", r.status);
			ok = 0;
		} else {
			const char *ver = json_str_field(r.json, "version");
			const struct json_value *jkbc = json_object_get(r.json, "kept_build_container");

			if (ver == NULL || strcmp(ver, "1.1") != 0) {
				fprintf(stderr, "FAIL: resumeme (resumed) version='%s', expected 1.1\n",
				        ver != NULL ? ver : "(null)");
				ok = 0;
			}
			if (jkbc == NULL || jkbc->type != JSON_NULL) {
				fprintf(stderr, "FAIL: resumeme (resumed) kept_build_container not cleared\n");
				ok = 0;
			}
		}
		kx_response_free(&r);

		/* the exact same registry slot/container name was reused, not a
		 * fresh one -- and a genuinely successful build (resumed or
		 * not) always gets torn down automatically afterward, same as
		 * any other pkgbuild container's clean exit. */
		snprintf(rs_container_path, sizeof(rs_container_path), "/v1/containers/%s", kept_name);
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", rs_container_path, NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: GET %s (resumed container after success) expected 404, got %d\n",
			        rs_container_path, r.status);
			ok = 0;
		}
		kx_response_free(&r);
	}
skip_resume:

	/* cleanup */
	run_cmd("rm -rf '%s'", scratch_dir);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "PKG RESULT: PASS\n" : "PKG RESULT: FAIL\n");
	return ok ? 0 : 1;
}
