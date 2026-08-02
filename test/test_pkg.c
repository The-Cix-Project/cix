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
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7628
#define PORT_ARG "--port=7628"

static char g_data_dir[PATH_MAX];
static char g_pkg_state_dir[PATH_MAX];
static char g_base_rootfs[PATH_MAX];
static char g_router_rootfs[PATH_MAX];
static char g_pkgbuild_rootfs[PATH_MAX];
static char g_images_base_dir[PATH_MAX];
static char g_images_router_dir[PATH_MAX];

/* BASE_ROOTFS/ROUTER_ROOTFS were compile-time macros before --data-dir=
 * isolation (see CLAUDE.md's test-isolation Environment note); every
 * call site below concatenated a literal suffix onto them at compile
 * time, which a runtime g_base_rootfs/g_router_rootfs pair can't do --
 * these two helpers are the one place that difference is absorbed, so
 * every call site below just swaps the old BASE_ROOTFS "/x" style
 * literal concatenation for base_path("/x")/router_path("/x"). */
static const char *base_path(const char *suffix)
{
	static char buf[PATH_MAX];

	snprintf(buf, sizeof(buf), "%s%s", g_base_rootfs, suffix);
	return buf;
}

static const char *router_path(const char *suffix)
{
	static char buf[PATH_MAX];

	snprintf(buf, sizeof(buf), "%s%s", g_router_rootfs, suffix);
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

static int write_recipe(const char *name, const char *version, const char *tarball_path,
                         const char *sha256, const char *depends)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/recipes/%s.recipe", g_pkg_state_dir, name);
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
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/recipes/%s.recipe", g_pkg_state_dir, name);
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
	char scratch_dir[] = "/tmp/kanxeo_test_pkg_XXXXXX";
	char tarball_path[512], sha256[128];
	char bad_sha256[128];
	char state[32];

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_pkg_state_dir, sizeof(g_pkg_state_dir), "%s/pkg", g_data_dir);
	snprintf(g_base_rootfs, sizeof(g_base_rootfs), "%s/images/base/rootfs", g_data_dir);
	snprintf(g_router_rootfs, sizeof(g_router_rootfs), "%s/images/router/rootfs", g_data_dir);
	snprintf(g_pkgbuild_rootfs, sizeof(g_pkgbuild_rootfs), "%s/images/pkgbuild", g_data_dir);
	snprintf(g_images_base_dir, sizeof(g_images_base_dir), "%s/images/base", g_data_dir);
	snprintf(g_images_router_dir, sizeof(g_images_router_dir), "%s/images/router", g_data_dir);

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
	    write_recipe("concurrent", "1.0", tarball_path, sha256, "") != 0) {
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

	/* 1. bootstrap the build toolchain image */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/bootstrap", NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: POST /v1/pkg/bootstrap, status=%d\n", r.status);
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

	/* 4. a second install (a DIFFERENT package) while greeter is still
	 * in flight -> 409 busy. Issued immediately, before any polling --
	 * fork() for the fetch subprocess just happened microseconds ago,
	 * so greeter is still "fetching" at this exact point, deterministically. */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"concurrent\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: concurrent install while busy expected 409, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 5. poll until greeter finishes; confirm it actually installed and
	 * the binary genuinely runs from the base image -- the real payoff,
	 * not just files with the right names. */
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

		/* upsert: re-add the same name with a bumped version -> the
		 * recipe list must reflect the new version, not the old one */
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
		memset(&r, 0, sizeof(r));
		if (kx_client_request(&client, "GET", "/v1/pkg/recipes", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET recipes after upsert, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *recipes = json_object_get(r.json, "recipes");
			size_t i;
			int found = 0;

			for (i = 0; recipes != NULL && i < recipes->u.array.count; i++) {
				const struct json_value *item = recipes->u.array.items[i];

				if (str_eq(json_str_field(item, "name"), "apirecipe")) {
					found = 1;
					if (!str_eq(json_str_field(item, "version"), "2.0")) {
						fprintf(stderr,
						        "FAIL: apirecipe version after upsert is '%s', expected 2.0\n",
						        json_str_field(item, "version"));
						ok = 0;
					}
				}
			}
			if (!found) {
				fprintf(stderr, "FAIL: apirecipe missing from recipe list after upsert\n");
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
