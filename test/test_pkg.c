/*
 * Phase 10 parts 1-2 end-to-end test: proves the package manager
 * (POST /v1/pkg/bootstrap, POST /v1/pkg/install, GET /v1/pkg[/{name}],
 * DELETE /v1/pkg/{name} -- daemon/src/pkg.c) over real HTTP, with a
 * real fetch -> checksum verify -> isolated container build -> merge
 * into the shared base image as the actual payoff, not a mocked
 * pipeline. Kept hermetic: no real internet dependency -- pkg_source
 * is a loopback http:// URL (test_http_src()) serving a tiny synthetic C
 * fixture this test stages itself, so the real fetch path is genuinely
 * exercised (not skipped/mocked) while the suite stays offline-safe
 * for repeated stress-testing runs. Part 2 additionally proves
 * automatic dependency resolution (with cycle detection) and explicit
 * per-package upgrades.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"
#include "test_floor.h"

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
		/* A stall is 10 minutes in production, which no test should
		 * sit through to check that the reporting works. */
		setenv("CIX_BUILD_STALL_SECONDS", "15", 1);
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
	fprintf(f, "hello: hello.c\n\ttcc -o hello hello.c\n");
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

/*
 * The fixture recipe most of this file's packages are built from.
 *
 * CPDL, and published through POST /v1/pkg/recipes rather than written
 * into the store (cix#516). A CPDL recipe's identity comes from
 * `cbs explain --json`, derived at publish (ADR-0305) or by the
 * daemon's startup sweep; a file dropped into the store while the
 * daemon is running has neither and every install of it is refused
 * 400. A shell recipe needed no derivation at all, which is why
 * writing the file worked for every caller here regardless of when it
 * ran, and is the one thing this conversion cannot carry across. The
 * callers that used to run before start_daemon() now run after it.
 */
static int write_recipe(const struct cix_client *c, const char *name, const char *version,
                         const char *tarball_path, const char *sha256, const char *depends)
{
	struct json_writer w;
	struct cix_response r;
	char runtime[192] = "";
	char content[2400];
	int ok;
	/*
	 * The wrapping directory inside the tarball, which is NOT the
	 * package name: several fixtures here build different packages
	 * from one tarball -- `relinked` is built from
	 * greeter-1.0.tarball -- and assuming the name gave "cannot
	 * enter directory ${src}/relinked/relinked-1.0; errno=2"
	 * (CPDL-E4004, measured on 192.168.15.95, 2026-09-25).
	 * stage_fixture_tarball() names the tarball after the directory
	 * it wraps, so the path already carries the answer.
	 */
	char srcdir[160];
	const char *base = strrchr(tarball_path, '/');
	size_t blen;

	base = (base != NULL) ? base + 1 : tarball_path;
	snprintf(srcdir, sizeof(srcdir), "%s", base);
	blen = strlen(srcdir);
	if (blen > 8 && strcmp(srcdir + blen - 8, ".tarball") == 0)
		srcdir[blen - 8] = '\0';

	if (depends != NULL && depends[0] != '\0')
		snprintf(runtime, sizeof(runtime),
		         "        runtime {\n"
		         "            package \"%s\"\n"
		         "        }\n",
		         depends);

	snprintf(content, sizeof(content),
	         "package \"%s\" {\n"
	         "    version \"%s\"\n"
	         "    release 1\n"
	         "    format \"cixpkg\"\n"
	         "\n"
	         "    sources {\n"
	         "        main \"%s\" {\n"
	         "            url \"%s\"\n"
	         "            sha256 \"%s\"\n"
	         "        }\n"
	         "    }\n"
	         "\n"
	         "    requires {\n"
	         "        build {\n"
	         "            compiler \"tcc\"\n"
	         "            tool \"linux-headers\"\n"
	         "            tool \"bash\"\n"
	         "            tool \"coreutils\"\n"
	         "            tool \"binutils\"\n"
	         "        }\n"
	         "%s"
	         "    }\n"
	         "\n"
	         "    build {\n"
	         "        cd \"${src}/%s/%s\" {\n"
	         "%s"
	         "            run \"tcc\" {\n"
	         "                \"-o\" \"hello\" \"hello.c\"\n"
	         "            }\n"
	         "        }\n"
	         "    }\n"
	         "\n"
	         "    install {\n"
	         "        mkdir \"${dest}/usr/bin\" chmod 0755\n"
	         "        copy \"${src}/%s/%s/hello\" to \"${dest}/usr/bin/%s\"\n"
	         "    }\n"
	         "}\n",
	         name, version, name, test_http_src(tarball_path), sha256, runtime, name, srcdir,
	         /*
	          * Issue #192, second pass: ONE recipe builds slowly, on
	          * purpose. The build-ceiling check asserts a 409 that is
	          * only true while a previous job still holds the single
	          * slot, and nothing made that install slow -- these
	          * fixtures compile one five-line hello.c, so the slot was
	          * legitimately free and 202 was the CORRECT answer. The
	          * test failed while the daemon was right, about one run
	          * in three. "hbconcurrent" is the same bug in the
	          * hostbuild ceiling check, found the same way.
	          *
	          * coreutils is in the declared build tools above, so
	          * `sleep` is genuinely present rather than assumed.
	          */
	         (strcmp(name, "slowhold") == 0 || strcmp(name, "hbconcurrent") == 0)
	                 ? "            run \"sleep\" {\n"
	                   "                \"5\"\n"
	                   "            }\n"
	                 : "",
	         name, srcdir, name);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_key(&w, "format");
	jw_str(&w, "pbs");
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	memset(&r, 0, sizeof(r));
	ok = (cix_client_request(c, "POST", "/v1/pkg/recipes", w.buf, &r) == 0 && r.status == 204);
	if (!ok)
		fprintf(stderr, "      POST /v1/pkg/recipes %s@%s: status=%d %.200s\n", name, version,
		        r.status, r.body != NULL ? r.body : "");
	cix_response_free(&r);
	jw_free(&w);
	return ok ? 0 : -1;
}

/*

 * One CPDL recipe, published through POST /v1/pkg/recipes.
 *
 * Every fixture writer in this file used to carry its own copy of the
 * same scaffold -- store path, name, version, source, checksum -- and
 * differ only in its build and install steps. Seven copies of a
 * scaffold is what gets one of them fixed and the others missed, so
 * this is the scaffold and the callers bring their bodies.
 *
 * Published rather than written to disk: a CPDL recipe's identity comes
 * from `cbs explain --json`, derived at publish (ADR-0305) or by the
 * daemon's startup sweep, and every caller here runs after the daemon
 * is up. A shell recipe needed no derivation, which is why writing the
 * file worked and is the one thing the conversion cannot carry across.
 *
 * `tools` is the lines inside requires{build{}}, so a caller that is
 * testing declaration handling passes exactly what it means to declare
 * and nothing is added behind it. `extra_sources`, `runtime`, and the
 * two bodies are CPDL text, indented to sit where they are placed.
 *
 * srcdir is derived, not passed: several fixtures build different
 * packages from ONE tarball (`relinked` from greeter-1.0.tarball), so
 * the wrapping directory is not the package name -- assuming it was
 * gave "cannot enter directory ${src}/relinked/relinked-1.0; errno=2"
 * (CPDL-E4004, 192.168.15.95, 2026-09-25). stage_fixture_tarball()
 * names a tarball after the directory it wraps, so the path says it.
 */
static int publish_cpdl_recipe(const struct cix_client *c, const char *name, const char *version,
                                const char *tarball_path, const char *sha256,
                                const char *extra_sources, const char *tools,
                                const char *runtime, const char *build_body,
                                const char *install_body)
{
	struct json_writer w;
	struct cix_response r;
	char content[4096];
	int ok;

	snprintf(content, sizeof(content),
	         "package \"%s\" {\n"
	         "    version \"%s\"\n"
	         "    release 1\n"
	         "    format \"cixpkg\"\n"
	         "\n"
	         "    sources {\n"
	         "        main \"%s\" {\n"
	         "            url \"%s\"\n"
	         "            sha256 \"%s\"\n"
	         "        }\n"
	         "%s"
	         "    }\n"
	         "\n"
	         "    requires {\n"
	         "        build {\n"
	         "%s"
	         "        }\n"
	         "%s"
	         "    }\n"
	         "\n"
	         "    build {\n"
	         "%s"
	         "    }\n"
	         "\n"
	         "    install {\n"
	         "%s"
	         "    }\n"
	         "}\n",
	         name, version, name, test_http_src(tarball_path), sha256,
	         extra_sources != NULL ? extra_sources : "", tools,
	         runtime != NULL ? runtime : "", build_body, install_body);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_key(&w, "format");
	jw_str(&w, "pbs");
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	memset(&r, 0, sizeof(r));
	ok = (cix_client_request(c, "POST", "/v1/pkg/recipes", w.buf, &r) == 0 && r.status == 204);
	if (!ok)
		fprintf(stderr, "      POST /v1/pkg/recipes %s@%s: status=%d %.200s\n", name, version,
		        r.status, r.body != NULL ? r.body : "");
	cix_response_free(&r);
	jw_free(&w);
	return ok ? 0 : -1;
}

/* The wrapping directory inside a fixture tarball -- see
 * publish_cpdl_recipe() for why it is derived rather than assumed. */
static void fixture_srcdir(const char *tarball_path, char *out, size_t out_size)
{
	const char *base = strrchr(tarball_path, '/');
	size_t blen;

	base = (base != NULL) ? base + 1 : tarball_path;
	snprintf(out, out_size, "%s", base);
	blen = strlen(out);
	if (blen > 8 && strcmp(out + blen - 8, ".tarball") == 0)
		out[blen - 8] = '\0';
}

/*
 * A recipe that installs one file of its own AND one deliberately
 * shared with another package (issue #175). Two packages owning one
 * path is normal, not pathological: glibc and linux-headers both own
 * parts of usr/include, and any two packages built from a shared
 * upstream tree overlap.
 */
static int write_shared_path_recipe(const struct cix_client *c, const char *name,
                                    const char *version, const char *tarball_path,
                                    const char *sha256, const char *shared_rel)
{
	char srcdir[160];
	char build_body[512], install_body[768];

	fixture_srcdir(tarball_path, srcdir, sizeof(srcdir));
	snprintf(build_body, sizeof(build_body),
	         "        cd \"${src}/%s/%s\" {\n"
	         "            run \"tcc\" {\n"
	         "                \"-o\" \"hello\" \"hello.c\"\n"
	         "            }\n"
	         "        }\n",
	         name, srcdir);
	/* One file of its own and one deliberately shared with another
	 * package (#175). `mkdir` takes the shared file's parent because
	 * the shell form did the same with dirname -- the path is a
	 * caller's choice and need not be one level deep. */
	snprintf(install_body, sizeof(install_body),
	         "        mkdir \"${dest}/usr/bin\" chmod 0755\n"
	         "        copy \"${src}/%s/%s/hello\" to \"${dest}/usr/bin/%s\"\n"
	         "        mkdir \"${dest}/usr/share/shared175\"\n"
	         "        write \"${dest}/%s\" \"\"\"\n"
	         "            %s\n"
	         "            \"\"\"\n",
	         name, srcdir, name, shared_rel, name);

	return publish_cpdl_recipe(c, name, version, tarball_path, sha256, "",
	                            "            compiler \"tcc\"\n"
	                            "            tool \"linux-headers\"\n"
	                            "            tool \"bash\"\n"
	                            "            tool \"coreutils\"\n"
	                            "            tool \"binutils\"\n",
	                            "", build_body, install_body);
}

/*
 * The version image_create() gives a brand-new image: SHA-256 of the
 * empty string, because a new image's manifest is empty. Spelled out
 * here rather than derived, so this test would still catch a composed
 * environment sitting at it even if the daemon's own derivation broke.
 */
#define EMPTY_MANIFEST_VERSION "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

/*
 * A recipe that goes quiet: it produces one line of output and then
 * sleeps. That is exactly what a stalled build looks like from the
 * daemon's side -- no exit, no error, no further output -- and it is
 * what a real gcc build did for an hour while reporting nothing.
 */
static int write_stalling_recipe(const struct cix_client *c, const char *name,
                                  const char *version, const char *tarball_path,
                                  const char *sha256)
{
	/* The build goes quiet on purpose and never finishes, so the
	 * install phase is never reached -- it exists only because a
	 * staged tree that is empty is refused. */
	return publish_cpdl_recipe(c, name, version, tarball_path, sha256, "",
	                            "            tool \"bash\"\n"
	                            "            tool \"coreutils\"\n",
	                            "",
	                            "        run \"echo\" {\n"
	                            "            \"starting\"\n"
	                            "        }\n"
	                            "        run \"sleep\" {\n"
	                            "            \"90\"\n"
	                            "        }\n",
	                            "        mkdir \"${dest}/usr/share/stalled\"\n");
}

/*
 * A recipe whose pkg_install() has a failing command in the MIDDLE,
 * followed by one that succeeds. Without `set -e` the function returns
 * the status of the last command and the package is recorded as
 * installed with whatever happened to make it into $PKG_DESTDIR --
 * which is how a real libcap shipped missing four of its binaries.
 */
static int write_midfail_recipe(const struct cix_client *c, const char *name,
                                 const char *version, const char *tarball_path,
                                 const char *sha256)
{
	char install_body[512];

	/*
	 * The failing command sits in the MIDDLE, with a succeeding one
	 * after it. Under the shell form the hazard was that pkg_install()
	 * returns the status of its LAST command, so the package recorded
	 * as installed with whatever happened to reach $PKG_DESTDIR --
	 * which is how a real libcap shipped missing four binaries.
	 *
	 * CPDL checks every `run` (expect exit 0 unless told otherwise), so
	 * the phase aborts at the failure and the later write never
	 * happens. That is the behaviour this test asserts, reached by the
	 * language rather than by a `set -e` someone has to remember.
	 */
	snprintf(install_body, sizeof(install_body),
	         "        mkdir \"${dest}/usr/bin\" chmod 0755\n"
	         "        run \"/nonexistent/command/that/fails\" {\n"
	         "        }\n"
	         "        write \"${dest}/usr/bin/%s\" \"\"\"\n"
	         "            late\n"
	         "            \"\"\"\n",
	         name);

	return publish_cpdl_recipe(c, name, version, tarball_path, sha256, "",
	                            "            tool \"bash\"\n"
	                            "            tool \"coreutils\"\n",
	                            "",
	                            "        run \"true\" {\n"
	                            "        }\n",
	                            install_body);
}

/*
 * Like write_recipe(), but the installed file records which version
 * produced it -- so a test can tell WHICH of several installed copies
 * of a package ended up in a composed build environment.
 */
static int write_stamped_recipe(const struct cix_client *c, const char *name,
                                 const char *version, const char *tarball_path,
                                 const char *sha256)
{
	char install_body[512];

	/* The installed file records which version produced it, so a test
	 * can tell WHICH of several installed copies reached a composed
	 * build environment. */
	snprintf(install_body, sizeof(install_body),
	         "        mkdir \"${dest}/usr/share\" chmod 0755\n"
	         "        write \"${dest}/usr/share/%s.version\" \"\"\"\n"
	         "            %s\n"
	         "            \"\"\"\n",
	         name, version);

	return publish_cpdl_recipe(c, name, version, tarball_path, sha256, "",
	                            "            tool \"bash\"\n"
	                            "            tool \"coreutils\"\n",
	                            "",
	                            "        run \"true\" {\n"
	                            "        }\n",
	                            install_body);
}

/*
 * Issue #109: a recipe that DECLARES its build tools. The build
 * container is then composed from exactly those packages and nothing
 * else, so what this build ran against is a property of the recipe
 * rather than of whatever the shared sandbox happens to hold.
 */
static int write_builddeps_recipe(const char *name, const char *version, const char *tarball_path,
                                   const char *sha256, const char *build_depends)
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
	fprintf(f, "pkg_source=%s\n", test_http_src(tarball_path));
	fprintf(f, "pkg_sha256=%s\n", sha256);
	fprintf(f, "pkg_depends=\"\"\n");
	/* This writer takes its declaration from the caller -- that is its
	 * whole purpose (proving a recipe's declared tools are honoured,
	 * including a deliberately unavailable one). It must NOT also get
	 * the standard floor: two pkg_build_depends lines mean the parser
	 * reads the first, the caller's is silently ignored, and a test
	 * that should fail passes instead. Which is exactly what happened. */
	fprintf(f, "pkg_build_depends=\"%s\"\n\n", build_depends);
	fprintf(f, "pkg_build() {\n\ttcc -o hello hello.c\n}\n\n");
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
/*
 * Like write_builddeps_recipe(), but its pkg_install() copies the
 * stamp file its declared build tool left in the environment into its
 * own PKG_DESTDIR. The installed package therefore records which
 * version of that tool the build genuinely ran against -- observable
 * afterwards without the environment still existing, which it will not
 * (ADR-0209 tears it down with the build).
 */
static int write_observing_recipe(const char *name, const char *version, const char *tarball_path,
                                   const char *sha256, const char *build_depends)
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
	fprintf(f, "pkg_source=%s\n", test_http_src(tarball_path));
	fprintf(f, "pkg_sha256=%s\n", sha256);
	fprintf(f, "pkg_depends=\"\"\n");
	fprintf(f, "pkg_build_depends=\"%s\"\n\n", build_depends);
	fprintf(f, "pkg_build() {\n\ttrue\n}\n\n");
	fprintf(f, "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/share\"\n"
	           "\tcp /usr/share/stamped.version \"$PKG_DESTDIR/usr/share/observed.version\"\n}\n");
	fclose(f);
	return 0;
}

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
	fprintf(f, "pkg_source=\"%s %s %s\"\n", test_http_src(tarball_path), test_http_src(extra1_path),
	        test_http_src(extra2_path));
	fprintf(f, "pkg_sha256=\"%s %s %s\"\n", tarball_sha256, extra1_sha256, extra2_sha256);
	fprintf(f, "pkg_depends=\"\"\n");
	fprintf(f, "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n");
	fprintf(f, "pkg_build() {\n\ttcc -o hello hello.c\n}\n\n");
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
/*
 * Returns the moment the state leaves fetching/building, so a generous
 * max_attempts NEVER slows a passing run -- it only sets how long a
 * genuine hang takes to report. Budgets were widened from 60 (18s
 * wall) after a real suite run failed with "greeter never left
 * fetching/building" on a machine measured 4-5x slower than idle at
 * that moment: greeter takes ~4s idle, so 18s had no margin under
 * load, and the failure cascaded into six more misleading assertions
 * downstream (this helper leaves out_state untouched on a non-200, so
 * later failures printed a stale 'building' from an earlier package).
 * That cascade cost a multi-hour investigation that concluded the code
 * was innocent -- the budget was the defect.
 */
/* #171: read a boolean field, false when absent or not a bool. */
static int json_bool_field(const struct json_value *obj, const char *key)
{
	const struct json_value *v = json_object_get(obj, key);

	return v != NULL && v->type == JSON_BOOL && v->u.boolean;
}

/*
 * Issue #407: PkgEntry.build_container must be non-null EXACTLY while a
 * build is in flight, and the gate is the whole correctness of the
 * field. Nothing clears the daemon's own build_container_name when a
 * build ends -- only a later job claiming the same chain slot does --
 * so a dropped state gate does not fail loudly. It reports a
 * torn-down container for an installed package, and after slot reuse
 * reports a container running an unrelated build. Both read as
 * plausible.
 *
 * Checked inside poll_pkg_state() rather than as a case of its own,
 * because that helper already observes every state transition of every
 * install this file performs -- so the gate is asserted dozens of
 * times across real builds for free, in both directions, which is
 * worth more than one dedicated test of a single package.
 *
 * NOTE: test_pkg is NOT in the Makefile's SELFTESTS list, so nothing
 * here runs in a release gate (#224 -- the suite's build container
 * cannot create containers). The live half of this field was verified
 * by reading GET /v1/pkg/{name} mid-build on 192.168.15.95; this
 * assertion is the regression net for anyone editing the serializer.
 */
static int g_build_container_gate_fails;

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
		/* Compare via out_state (a stable, owned copy) after freeing
		 * r -- state itself points into r.json's tree and would be a
		 * dangling pointer the instant cix_response_free() runs. */
		snprintf(out_state, out_state_size, "%s", state);
		{
			/* #407's gate, both directions. Read before the free,
			 * for the same dangling-pointer reason as above. */
			const struct json_value *bc =
			        json_object_get(r.json, "build_container");

			if (bc == NULL) {
				fprintf(stderr, "FAIL: %s has no build_container field\n", name);
				g_build_container_gate_fails++;
			} else if (strcmp(out_state, "building") == 0) {
				if (bc->type != JSON_STRING ||
				    strncmp(bc->u.string, "__pkgbuild-", 11) != 0) {
					fprintf(stderr,
					        "FAIL: %s is building but build_container is not a "
					        "__pkgbuild-* name\n", name);
					g_build_container_gate_fails++;
				}
			} else if (bc->type != JSON_NULL) {
				fprintf(stderr,
				        "FAIL: %s is '%s' but build_container is non-null -- the "
				        "state gate is gone\n", name, out_state);
				g_build_container_gate_fails++;
			}
		}
		/* The daemon's reason for a failed build, so a report can say
		 * why and not only that (cix-tests v2.57.247-1, 2026-09-23). */
		if (strcmp(out_state, "failed") == 0) {
			const char *err = json_str_field(r.json, "error");

			fprintf(stderr, "    %s failed: %s\n", name, err != NULL ? err : "(no error field)");

			/* And WHY, which the error field never says: it carries an
			 * exit status and nothing more (cix#516). */
			test_print_build_log(c, name, 40);
		}
		cix_response_free(&r);
		if (strcmp(out_state, "fetching") != 0 && strcmp(out_state, "building") != 0)
			return 0;
		usleep(300000);
	}
	return -1;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	struct cix_response r;
	char scratch_dir[] = "/tmp/cix_test_pkg_XXXXXX";
	char tarball_path[512], sha256[128];
	char bad_sha256[128];
	char state[32];
	char sandbox_version_before[128] = { 0 };

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	snprintf(g_pkg_state_dir, sizeof(g_pkg_state_dir), "%s/rebuildable/pkg", g_data_dir);
	snprintf(g_pkgbuild_rootfs, sizeof(g_pkgbuild_rootfs), "%s/rebuildable/images/pkgbuild", g_data_dir);
	snprintf(g_images_base_dir, sizeof(g_images_base_dir), "%s/rebuildable/images/base", g_data_dir);
	snprintf(g_images_router_dir, sizeof(g_images_router_dir), "%s/rebuildable/images/router", g_data_dir);

	reset_pkg_state();
	run_cmd("mkdir -p '%s/recipes'", g_pkg_state_dir);

	/*
	 * ADR-0209: the build floor, seeded before the daemon starts.
	 * Real, recipe-built package artifacts go into this daemon's own
	 * cache, and the real recipes that approve them go beside them --
	 * so the installs below are cache hits needing no build
	 * environment, which is the only honest way to have one at all now
	 * that there is no fungible sandbox. Verified against each
	 * recipe's own pkg_artifact_sha256 on the way in; if the artifacts
	 * are absent this fails here, loudly, rather than the suite
	 * mysteriously failing later.
	 */
	if (test_image_fixture_seed_floor_packages(g_data_dir, "build-inputs/floor-artifacts") != 0) {
		fprintf(stderr,
		        "FAIL: could not seed the build floor -- fetch the real package artifacts into "
		        "build-inputs/floor-artifacts first (see ADR-0209); they are never fabricated\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

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
	/* Issue #57: a recipe that actually PRINTS, so the persisted build
	 * log has content to assert on -- greeter's own build is silent on
	 * success, and "the file exists" would pass against a log that
	 * never recorded a byte. Same layout write_recipe() uses
	 * (recipes/<name>/<version>/build.sh), only the body differs. */
	{
		char dir[300];
		char path[400];
		FILE *f;

		snprintf(dir, sizeof(dir), "%s/recipes/chatty", g_pkg_state_dir);
		mkdir(dir, 0755);
		snprintf(dir, sizeof(dir), "%s/recipes/chatty/1.0", g_pkg_state_dir);
		mkdir(dir, 0755);
		snprintf(path, sizeof(path), "%s/build.sh", dir);
		f = fopen(path, "w");
		if (f != NULL) {
			fprintf(f, "pkg_name=chatty\npkg_version=1.0\n");
			fprintf(f, "pkg_source=%s\n", test_http_src(tarball_path));
			fprintf(f, "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n", sha256);
			fprintf(f, "pkg_build() {\n\techo BUILD_LOG_MARKER_ONE\n\ttcc -o hello hello.c\n"
			           "\techo BUILD_LOG_MARKER_TWO\n}\n\n");
			fprintf(f, "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n"
			           "\tcp hello \"$PKG_DESTDIR/usr/bin/chatty\"\n}\n");
			fclose(f);
		}
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0)
		return 1;

	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	/*
	 * Published after the daemon is up, not before it: a CPDL recipe
	 * needs its identity derived, and POST /v1/pkg/recipes is what
	 * does that (cix#516). These five plus badsum used to be written
	 * straight to disk here, which a shell recipe allowed and a PBS
	 * one does not.
	 */
	if (write_recipe(&client, "greeter", "1.0", tarball_path, sha256, "") != 0 ||
	    write_recipe(&client, "concurrent", "1.0", tarball_path, sha256, "") != 0 ||
	    write_recipe(&client, "overflow", "1.0", tarball_path, sha256, "") != 0 ||
	    write_recipe(&client, "slowhold", "1.0", tarball_path, sha256, "") != 0 ||
	    write_recipe(&client, "hbconcurrent", "1.0", tarball_path, sha256, "") != 0) {
		fprintf(stderr, "FAIL: could not write recipes\n");
		return 1;
	}
	snprintf(bad_sha256, sizeof(bad_sha256),
	         "0000000000000000000000000000000000000000000000000000000000000000");
	bad_sha256[64] = '\0';
	if (write_recipe(&client, "badsum", "1.0", tarball_path, bad_sha256, "") != 0) {
		fprintf(stderr, "FAIL: could not write badsum recipe\n");
		return 1;
	}

	/* ADR-0157 Phase 3: the real, unmodified default before this test
	 * touches it at all. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/pkg-build-config", NULL, &r) != 0 ||
	    r.status != 200 || json_as_number(json_object_get(r.json, "max_concurrent_jobs")) != 10) {
		fprintf(stderr, "FAIL: GET pkg-build-config expected 200 max_concurrent_jobs=10, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * 1. The build floor (ADR-0209). This used to be
	 * `POST /v1/pkg/bootstrap`, which copied the build host's whole
	 * /usr into an image -- the mechanism that put rustup, chromium
	 * and qemu inside cix-builder (issue #168). It is gone, and so is
	 * every other route by which a build could inherit something it
	 * did not declare.
	 *
	 * What replaces it is what a fresh host actually does: real,
	 * recipe-built package artifacts are already in this daemon's own
	 * cache (seeded by test_data_dir_create()), so installing them is
	 * a cache hit that needs no build environment at all. Every later
	 * build composes from tools it declares, and those declarations
	 * resolve against these.
	 */
	{
		char fstate[64];

		/*
		 * glibc is deliberately NOT in that list: the daemon installs
		 * it into the default image itself, at startup, and this
		 * asserts that it did (#189).
		 *
		 * A fresh install materializes "base" with the ordinary
		 * baseline, which since ADR-0216 carries no runtime borrowed
		 * from the build host -- so the one image a fresh box has
		 * could not run a container at all. The daemon now installs a
		 * C library through the ordinary pipeline whenever the default
		 * image lacks one and the artifact is already cached, which is
		 * exactly the situation here: the floor is seeded before the
		 * daemon starts.
		 *
		 * Asserted through the package manifest, not by looking for
		 * the file: the whole point of #189 over a copy is that the
		 * image ends up with a real, versioned, upgradable entry.
		 */
		{
			const char *ver = NULL;

			/* poll_pkg_state() leaves this untouched when it never
			 * saw a state at all, and a diagnostic that prints an
			 * uninitialized buffer is worse than one that says
			 * nothing -- caught by reintroducing the bug this
			 * assertion is for. */
			fstate[0] = '\0';
			if (poll_pkg_state(&client, "glibc", fstate, sizeof(fstate), 300) != 0 ||
			    strcmp(fstate, "installed") != 0) {
				fprintf(stderr,
				        "FAIL: #189 the daemon did not install a C library into the default "
				        "image on its own (state=%s)\n",
				        fstate[0] != '\0' ? fstate : "never reported");
				ok = 0;
			} else {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/glibc", NULL, &r) == 0 &&
				    r.status == 200)
					ver = json_str_field(r.json, "version");
				if (ver == NULL || ver[0] == '\0') {
					fprintf(stderr,
					        "FAIL: #189 the default image's C library has no recorded "
					        "version -- it arrived as a copy, not a package\n");
					ok = 0;
				}
				cix_response_free(&r);
			}
		}

		/* test_floor_install_all() reports each failure where it happens. */
		if (test_floor_install_all(&client) != 0)
			ok = 0;
	}

	/* ADR-0157 Phase 3 raised the real default ceiling to 10 -- lowered
	 * here to a small, deterministic 2 so every "N chains busy" boundary
	 * check below (originally written against Phase 2's placeholder
	 * default) stays meaningful without needing 10+ concurrent installs
	 * to actually exhaust it. The config endpoint's own behavior
	 * (validation, and the ceiling genuinely taking effect) is proven
	 * separately, later in this test, by deliberately varying it. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":2}", &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: PUT pkg-build-config max_concurrent_jobs=2 (test setup), got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 2. install for an unknown recipe -> 400 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"nosuchpackage\"}", &r) !=
	        0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: install unknown recipe expected 400, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 3. real install: greeter -> 202, fetching */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"greeter\"}", &r) != 0 ||
	    r.status != 202 || !str_eq(json_str_field(r.json, "state"), "fetching")) {
		fprintf(stderr, "FAIL: POST install greeter, status=%d, state=%s\n", r.status,
		        json_str_field(r.json, "state") ? json_str_field(r.json, "state") : "(null)");
		ok = 0;
	}
	cix_response_free(&r);

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
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"concurrent\"}", &r) !=
	        0 ||
	    r.status != 202 || !str_eq(json_str_field(r.json, "state"), "fetching")) {
		fprintf(stderr, "FAIL: POST install concurrent (2nd chain) status=%d, state=%s\n", r.status,
		        json_str_field(r.json, "state") ? json_str_field(r.json, "state") : "(null)");
		ok = 0;
	}
	cix_response_free(&r);

	/* 4b. a THIRD distinct install while both chain slots are occupied
	 * -> 409 -- proves both slots are genuinely in use (not just that
	 * "concurrent" above got lucky some other way), and that the
	 * PKG_MAX_CONCURRENT_JOBS ceiling is still real and enforced. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"overflow\"}", &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: overflow install with both chains busy expected 409, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * Issue #40: the shared build sandbox is a real, versioned image
	 * now, so an ordinary install has to produce a NEW version of it.
	 *
	 * Worth asserting precisely because the obvious implementation is
	 * silently wrong: an image's version identity is a hash of its
	 * MANIFEST (ADR-0108), and folding a package into the sandbox adds
	 * no manifest entry -- so the hash would be byte-identical, the
	 * staging copy would be discarded as "already produced", and the
	 * merge would vanish with every call reporting success. That is
	 * ADR-0155's own same-manifest trap reached from the other
	 * direction. A test that only checked "the install succeeded"
	 * would have passed throughout.
	 */
	{
		char before[128] = { 0 };

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/images/cix-builder", NULL, &r) == 0 &&
		    r.status == 200 && json_str_field(r.json, "current_version") != NULL)
			snprintf(before, sizeof(before), "%s", json_str_field(r.json, "current_version"));
		cix_response_free(&r);
		snprintf(sandbox_version_before, sizeof(sandbox_version_before), "%s", before);
	}

	/* 5. poll until BOTH greeter and concurrent finish; confirm each
	 * actually installed and its binary genuinely runs from the base
	 * image -- the real payoff, not just files with the right names. */
	if (poll_pkg_state(&client, "greeter", state, sizeof(state), 200) != 0) {
		fprintf(stderr, "FAIL: greeter never left fetching/building\n");
		ok = 0;
	} else if (strcmp(state, "installed") != 0) {
		fprintf(stderr, "FAIL: greeter ended in state '%s', not installed\n", state);
		ok = 0;
	} else {
		/*
		 * Issue #168 / ADR-0209, inverted from what #40 asserted here.
		 *
		 * This used to require that every successful install grew the
		 * `cix-builder` image, because every install folded into it as
		 * the shared build sandbox. That fold is gone: build
		 * environments are composed from a recipe's declared tools, so
		 * an install has no business touching an unrelated image at
		 * all. Left as-is, this check would have passed only while the
		 * bug it now guards against was present.
		 *
		 * So the assertion is the opposite one, and it is worth having:
		 * installing a package must NOT move cix-builder. If this ever
		 * fails, accumulation has come back and an image is once again
		 * growing content its manifest never declared.
		 */
		if (sandbox_version_before[0] != '\0') {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/images/cix-builder", NULL, &r) == 0 &&
			    r.status == 200 && json_str_field(r.json, "current_version") != NULL &&
			    strcmp(sandbox_version_before, json_str_field(r.json, "current_version")) != 0) {
				fprintf(stderr,
				        "FAIL: #168 an install moved the cix-builder image -- nothing should "
				        "fold into a shared sandbox any more (ADR-0209)\n");
				ok = 0;
			}
			cix_response_free(&r);
		}
	}
	/*
	 * ADR-0272: a completed install leaves a run behind.
	 *
	 * The contract of GET /pipeline/runs is gated in test_stallwatch,
	 * which is in SELFTESTS; this is the half that cannot be asserted
	 * there because it needs a package to have actually been built. A
	 * run is closed at the FINAL outcome, so a greeter that reports
	 * installed and leaves no successful run means the close is hooked
	 * to the provisional assignment instead.
	 */
	if (strcmp(state, "installed") == 0) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pipeline/runs?name=greeter", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/pipeline/runs?name=greeter, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *runs = json_object_get(r.json, "runs");
			int saw_ok = 0;
			size_t k;

			if (runs != NULL && runs->type == JSON_ARRAY) {
				for (k = 0; k < runs->u.array.count; k++) {
					const char *st = json_str_field(runs->u.array.items[k], "status");
					const char *nm = json_str_field(runs->u.array.items[k], "name");

					if (st != NULL && strcmp(st, "ok") == 0 && nm != NULL &&
					    strcmp(nm, "greeter") == 0)
						saw_ok = 1;
				}
			}
			if (!saw_ok) {
				fprintf(stderr,
				        "FAIL: greeter installed but left no successful run in the store\n");
				ok = 0;
			}
		}
		cix_response_free(&r);
	}

	if (strcmp(state, "installed") == 0) {
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

	/*
	 * Issue #101: a package that could not be FETCHED and one that
	 * failed to BUILD both used to read "failed", with only prose
	 * telling them apart -- and they call for opposite responses: retry
	 * the first, fix the second. The kind is a field now.
	 */
	{
		char path[PATH_MAX];
		FILE *f;

		/* Unreachable by construction: 192.0.2.0/24 is TEST-NET-1,
		 * reserved for documentation and routed nowhere. */
		snprintf(path, sizeof(path), "%s/recipes/unreachable", g_pkg_state_dir);
		mkdir(path, 0755);
		snprintf(path, sizeof(path), "%s/recipes/unreachable/1.0", g_pkg_state_dir);
		mkdir(path, 0755);
		snprintf(path, sizeof(path), "%s/recipes/unreachable/1.0/build.sh", g_pkg_state_dir);
		f = fopen(path, "w");
		if (f != NULL) {
			fprintf(f, "pkg_name=unreachable\npkg_version=1.0\n");
			fprintf(f, "pkg_source=https://192.0.2.1/nothing.tar.gz\n");
			fprintf(f, "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n", sha256);
			fprintf(f, "pkg_build() {\n\ttrue\n}\n\npkg_install() {\n\ttrue\n}\n");
			fclose(f);
		}

		/* Builds fine, fails in its own build step. */
		snprintf(path, sizeof(path), "%s/recipes/badbuild", g_pkg_state_dir);
		mkdir(path, 0755);
		snprintf(path, sizeof(path), "%s/recipes/badbuild/1.0", g_pkg_state_dir);
		mkdir(path, 0755);
		snprintf(path, sizeof(path), "%s/recipes/badbuild/1.0/build.sh", g_pkg_state_dir);
		f = fopen(path, "w");
		if (f != NULL) {
			fprintf(f, "pkg_name=badbuild\npkg_version=1.0\n");
			fprintf(f, "pkg_source=%s\n", test_http_src(tarball_path));
			fprintf(f, "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n", sha256);
			fprintf(f, "pkg_build() {\n\texit 7\n}\n\npkg_install() {\n\ttrue\n}\n");
			fclose(f);
		}

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"unreachable\"}", &r);
		cix_response_free(&r);
		if (poll_pkg_state(&client, "unreachable", state, sizeof(state), 90) != 0 ||
		    strcmp(state, "failed") != 0) {
			fprintf(stderr, "FAIL: #101 unreachable ended in state '%s', expected failed\n", state);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/unreachable", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #101 GET unreachable, status=%d\n", r.status);
			ok = 0;
		} else {
			const char *kind = json_str_field(r.json, "stage");

			if (kind == NULL || strcmp(kind, "fetch") != 0) {
				fprintf(stderr, "FAIL: #101 a source that could not be reached reported stage "
				                "'%s', expected fetch\n",
				        kind != NULL ? kind : "(null)");
				ok = 0;
			}
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"badbuild\"}", &r);
		cix_response_free(&r);
		if (poll_pkg_state(&client, "badbuild", state, sizeof(state), 90) != 0 ||
		    strcmp(state, "failed") != 0) {
			fprintf(stderr, "FAIL: #101 badbuild ended in state '%s', expected failed\n", state);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/badbuild", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #101 GET badbuild, status=%d\n", r.status);
			ok = 0;
		} else {
			const char *kind = json_str_field(r.json, "stage");

			if (kind == NULL || strcmp(kind, "build") != 0) {
				fprintf(stderr,
				        "FAIL: #101 a recipe whose build exited 7 reported stage '%s', expected "
				        "build\n",
				        kind != NULL ? kind : "(null)");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/*
		 * Issue #302: a build that exits 0 while the shell reported
		 * a missing command must be REFUSED, not installed.
		 *
		 * This is the silent half of the class and the reason the
		 * check exists at all. gettext's environment was missing
		 * find, cmp and xargs; none of the three changed an exit
		 * status, and libtool quietly produced a static archive
		 * without the convenience-archive objects while configure
		 * quietly answered two feature probes from a tool that was
		 * not there.
		 *
		 * The recipe below reproduces exactly that shape: a real
		 * command that genuinely is not there, followed by a
		 * successful exit. Deliberately a real missing command
		 * rather than an echo of the phrase -- an echo would prove
		 * the scanner reads text, not that it catches the thing
		 * that actually happens.
		 */
		snprintf(path, sizeof(path), "%s/recipes/silenttool", g_pkg_state_dir);
		mkdir(path, 0755);
		snprintf(path, sizeof(path), "%s/recipes/silenttool/1.0", g_pkg_state_dir);
		mkdir(path, 0755);
		snprintf(path, sizeof(path), "%s/recipes/silenttool/1.0/build.sh", g_pkg_state_dir);
		f = fopen(path, "w");
		if (f != NULL) {
			fprintf(f, "pkg_name=silenttool\npkg_version=1.0\n");
			fprintf(f, "pkg_source=%s\n", test_http_src(tarball_path));
			fprintf(f, "pkg_sha256=%s\npkg_depends=\"\"\n"
			           "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n",
			        sha256);
			fprintf(f, "pkg_build() {\n"
			           "\tcix_no_such_tool_302 || true\n"
			           "\ttrue\n"
			           "}\n\n"
			           "pkg_install() {\n\ttrue\n}\n");
			fclose(f);
		}

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"silenttool\"}", &r);
		cix_response_free(&r);
		if (poll_pkg_state(&client, "silenttool", state, sizeof(state), 90) != 0 ||
		    strcmp(state, "failed") != 0) {
			fprintf(stderr,
			        "FAIL: #302 a build that exited 0 with a missing command ended in state "
			        "'%s', expected failed\n",
			        state);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/silenttool", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #302 GET silenttool, status=%d\n", r.status);
			ok = 0;
		} else {
			const char *kind = json_str_field(r.json, "stage");
			const char *err = json_str_field(r.json, "error");

			if (kind == NULL || strcmp(kind, "build") != 0) {
				fprintf(stderr, "FAIL: #302 missing-tool failure reported stage '%s', "
				                "expected build\n",
				        kind != NULL ? kind : "(null)");
				ok = 0;
			}
			/* The message must NAME the tool. A failure that says
			 * only "build failed" puts the reader back where
			 * gettext's eight revisions started. */
			if (err == NULL || strstr(err, "cix_no_such_tool_302") == NULL) {
				fprintf(stderr,
				        "FAIL: #302 the failure does not name the missing tool: '%s'\n",
				        err != NULL ? err : "(null)");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/* And a healthy package says nothing at all -- absence of a
		 * failure is not a kind of failure. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/greeter", NULL, &r) == 0 &&
		    r.status == 200) {
			const struct json_value *k = json_object_get(r.json, "stage");

			if (k == NULL || k->type != JSON_NULL) {
				fprintf(stderr, "FAIL: #101 an installed package reported a pipeline stage\n");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/* Leave nothing failed behind for later assertions to trip on. */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/pkg/unreachable", NULL, &r);
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/pkg/badbuild", NULL, &r);
		cix_response_free(&r);
	}

	/*
	 * Issue #64: which version an omitted version resolves to is a
	 * per-package policy, not one fixed rule.
	 *
	 * Set up deliberately so that HIGHEST and NEWEST disagree -- 2.0 is
	 * published first, then 1.5 -- because that is the real case this
	 * exists for: the Part 201 toolchain work published gcc 4.7.4 and
	 * 6.4.0 *after* 16.2.0, so for gcc the highest version and the
	 * newest published are different recipes and neither is what an
	 * operator walking a bootstrap chain wants.
	 *
	 * Asserted through what actually gets INSTALLED, not through a
	 * resolver's own opinion of itself.
	 */
	{
		char installed[64];

		if (write_recipe(&client, "policypkg", "2.0", tarball_path, sha256, "") != 0) {
			fprintf(stderr, "FAIL: #64 could not write policypkg 2.0\n");
			ok = 0;
		}
		sleep(1); /* distinct mtimes: "published later" has to be real */
		if (write_recipe(&client, "policypkg", "1.5", tarball_path, sha256, "") != 0) {
			fprintf(stderr, "FAIL: #64 could not write policypkg 1.5\n");
			ok = 0;
		}

		/* Default: highest wins, which is 2.0 even though 1.5 is newer. */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"policypkg\"}", &r);
		cix_response_free(&r);
		if (poll_pkg_state(&client, "policypkg", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: #64 policypkg default install ended '%s'\n", state);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		installed[0] = '\0';
		if (cix_client_request(&client, "GET", "/v1/pkg/policypkg", NULL, &r) == 0 && r.status == 200 &&
		    json_str_field(r.json, "version") != NULL)
			snprintf(installed, sizeof(installed), "%s", json_str_field(r.json, "version"));
		cix_response_free(&r);
		if (strcmp(installed, "2.0-1") != 0) {
			fprintf(stderr, "FAIL: #64 default policy installed '%s', expected 2.0-1 (highest)\n",
			        installed);
			ok = 0;
		}

		/* newest: the later-published 1.5 wins over the higher 2.0. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/pkg/policies/policypkg",
		                       "{\"policy\":\"newest\"}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #64 set newest, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/pkg/policypkg", NULL, &r);
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"policypkg\"}", &r);
		cix_response_free(&r);
		if (poll_pkg_state(&client, "policypkg", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: #64 policypkg newest install ended '%s'\n", state);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		installed[0] = '\0';
		if (cix_client_request(&client, "GET", "/v1/pkg/policypkg", NULL, &r) == 0 && r.status == 200 &&
		    json_str_field(r.json, "version") != NULL)
			snprintf(installed, sizeof(installed), "%s", json_str_field(r.json, "version"));
		cix_response_free(&r);
		if (strcmp(installed, "1.5-1") != 0) {
			fprintf(stderr,
			        "FAIL: #64 newest policy installed '%s', expected 1.5-1 (published later)\n",
			        installed);
			ok = 0;
		}

		/* pinned: held at 2.0-1 even with 1.5-1 newer and 3.0-1 published
		 * after the pin -- a pin that drifts is not a pin. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/pkg/policies/policypkg",
		                       "{\"policy\":\"pinned\",\"version\":\"2.0-1\"}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #64 set pinned, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		if (write_recipe(&client, "policypkg", "3.0", tarball_path, sha256, "") != 0) {
			fprintf(stderr, "FAIL: #64 could not write policypkg 3.0\n");
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/pkg/policypkg", NULL, &r);
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"policypkg\"}", &r);
		cix_response_free(&r);
		if (poll_pkg_state(&client, "policypkg", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: #64 policypkg pinned install ended '%s'\n", state);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		installed[0] = '\0';
		if (cix_client_request(&client, "GET", "/v1/pkg/policypkg", NULL, &r) == 0 && r.status == 200 &&
		    json_str_field(r.json, "version") != NULL)
			snprintf(installed, sizeof(installed), "%s", json_str_field(r.json, "version"));
		cix_response_free(&r);
		if (strcmp(installed, "2.0-1") != 0) {
			fprintf(stderr, "FAIL: #64 pinned policy installed '%s', expected the held 2.0-1\n",
			        installed);
			ok = 0;
		}

		/* A pin with no version is refused: it would claim to hold
		 * something while meaning "highest". */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/pkg/policies/policypkg", "{\"policy\":\"pinned\"}",
		                       &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: #64 pin without a version should 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* Leave nothing drifted behind: policypkg is installed at the
		 * held 2.0 with a 3.0 published, which is a real update
		 * candidate and would make a later "nothing to update"
		 * assertion fail for a reason that has nothing to do with it. */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/pkg/policypkg", NULL, &r);
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/pkg/policies/policypkg", NULL, &r);
		cix_response_free(&r);
	}

	/*
	 * Issue #57: every build's COMPLETE output is teed to a file and
	 * survives the build. Until now the log store kept a ~4KB tail and
	 * `pkg build-log` was live-only, so a finished build's real output
	 * survived nowhere -- which cost two multi-hour round trips on gcc:
	 * once when a verbose configure swamped the tail and hid the error,
	 * once when the workaround (redirecting inside the container)
	 * silenced the live stream and a healthy build was killed by hand
	 * as "hung".
	 *
	 * greeter has just built, so its log must exist and must contain
	 * output the 4KB tail could not be relied on to hold.
	 */
	{
		char logfile[256] = "";

		/* Build the chatty package first, so there is output to find. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"chatty\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: #57 install chatty, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		if (poll_pkg_state(&client, "chatty", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: #57 chatty ended in state '%s'\n", state);
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/build-logs", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #57 GET build-logs, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *logs = json_object_get(r.json, "logs");
			size_t li;

			if (logs == NULL || logs->type != JSON_ARRAY || logs->u.array.count == 0) {
				fprintf(stderr, "FAIL: #57 no build log recorded for a build that just ran\n");
				ok = 0;
			} else {
				for (li = 0; li < logs->u.array.count; li++) {
					const char *f = json_str_field(logs->u.array.items[li], "file");

					if (f != NULL && strncmp(f, "chatty-", 7) == 0) {
						snprintf(logfile, sizeof(logfile), "%s", f);
						break;
					}
				}
				if (logfile[0] == '\0') {
					fprintf(stderr, "FAIL: #57 no chatty build log among %d\n",
					        (int)logs->u.array.count);
					ok = 0;
				}
			}
		}
		cix_response_free(&r);

		if (logfile[0] != '\0') {
			char path[512];

			snprintf(path, sizeof(path), "/v1/pkg/build-logs/%s", logfile);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", path, NULL, &r) != 0 || r.status != 200) {
				fprintf(stderr, "FAIL: #57 reading %s, status=%d\n", path, r.status);
				ok = 0;
			} else if (memmem(r.body, r.body_len, "BUILD_LOG_MARKER_ONE",
			                   strlen("BUILD_LOG_MARKER_ONE")) == NULL ||
			           memmem(r.body, r.body_len, "BUILD_LOG_MARKER_TWO",
			                   strlen("BUILD_LOG_MARKER_TWO")) == NULL) {
				/* Both markers: the FIRST proves output from before the
				 * compile survived, which is exactly what the 4KB tail
				 * could not promise and what cost a real debugging round
				 * trip on gcc. */
				fprintf(stderr, "FAIL: #57 build log is missing its own markers (%d bytes)\n",
				        (int)r.body_len);
				ok = 0;
			}
			cix_response_free(&r);
		}

		/* A filename is a path component straight out of an HTTP
		 * request and this opens a file with it, so traversal is
		 * refused rather than sanitised into something plausible. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/build-logs/../../../etc/passwd", NULL, &r) ==
		        0 &&
		    r.status == 200) {
			fprintf(stderr, "FAIL: #57 path traversal in a build-log name was served\n");
			ok = 0;
		}
		cix_response_free(&r);
	}

	/*
	 * Issue #85: the build budget is AGGREGATE, carried by one parent
	 * cgroup every build container is a leaf of -- not a per-container
	 * ceiling that silently multiplies by max_concurrent_jobs (default
	 * 10). That mistake took a real 2-CPU box off the network: four
	 * concurrent builds at 1.5 CPU each demanded 6 CPUs, starved cixd
	 * off the run queue, and a shell-less host has no other way in.
	 *
	 * Asserted by CHANGING the configured budget and running a build,
	 * then reading the parent back -- not merely by the parent existing.
	 * The directory survives a daemon restart and even a reboot-less
	 * revert of this whole feature, so "it is there" proves nothing;
	 * "it carries the number I just set" proves the aggregate cgroup is
	 * really being created and re-applied per build, which is exactly
	 * what stops a stale ceiling outliving a config change.
	 */
	{
		const char *parent = "/sys/fs/cgroup/cix-workload/cix-pkgbuild";
		const char *want_cpu = "70000 100000";
		const long long want_mem = 1610612736LL;
		char path[PATH_MAX];
		char restore[128];

		snprintf(restore, sizeof(restore), "{\"cpu_max\":\"150000 100000\",\"memory_max\":4294967296}");

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config",
		                       "{\"cpu_max\":\"70000 100000\",\"memory_max\":1610612736}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #85 could not set a distinctive build budget, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		if (write_recipe(&client, "budgeted", "1.0", tarball_path, sha256, "") != 0) {
			fprintf(stderr, "FAIL: #85 could not write budgeted recipe\n");
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"budgeted\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: #85 install budgeted, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		if (poll_pkg_state(&client, "budgeted", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: #85 budgeted ended in state '%s'\n", state);
			ok = 0;
		}

		snprintf(path, sizeof(path), "%s/cpu.max", parent);
		{
			FILE *f = fopen(path, "r");
			char got[64] = "";

			if (f == NULL) {
				fprintf(stderr,
				        "FAIL: #85 aggregate build cgroup %s missing -- builds are back to "
				        "per-container limits, which multiply by concurrency\n",
				        parent);
				ok = 0;
			} else {
				if (fgets(got, sizeof(got), f) != NULL)
					got[strcspn(got, "\n")] = '\0';
				fclose(f);
				if (strcmp(got, want_cpu) != 0) {
					fprintf(stderr,
					        "FAIL: #85 parent cpu.max='%s', configured '%s' -- the aggregate "
					        "ceiling is not being applied per build\n",
					        got, want_cpu);
					ok = 0;
				}
			}
		}

		snprintf(path, sizeof(path), "%s/memory.max", parent);
		{
			FILE *f = fopen(path, "r");
			long long got = -1;

			if (f != NULL) {
				if (fscanf(f, "%lld", &got) != 1)
					got = -1;
				fclose(f);
			}
			if (got != want_mem) {
				fprintf(stderr, "FAIL: #85 parent memory.max=%lld, configured %lld\n", got,
				        want_mem);
				ok = 0;
			}
		}

		/* A cgroup v2 parent has to delegate controllers to its subtree
		 * or its children get no accounting under it at all. */
		snprintf(path, sizeof(path), "%s/cgroup.subtree_control", parent);
		{
			FILE *f = fopen(path, "r");
			char got[128] = "";

			if (f != NULL) {
				if (fgets(got, sizeof(got), f) == NULL)
					got[0] = '\0';
				fclose(f);
			}
			if (strstr(got, "cpu") == NULL || strstr(got, "memory") == NULL) {
				fprintf(stderr, "FAIL: #85 parent subtree_control='%s' (want cpu and memory)\n",
				        got);
				ok = 0;
			}
		}

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "PUT", "/v1/system/pkg-build-config", restore, &r);
		cix_response_free(&r);
	}

	if (poll_pkg_state(&client, "concurrent", state, sizeof(state), 200) != 0) {
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

	/*
	 * Issue #109 (ADR-0199): a recipe that declares a build tool which
	 * is not available must FAIL, naming it -- never quietly fall back
	 * to the shared sandbox.
	 *
	 * That refusal is the whole property. A fallback would let the
	 * build succeed against something fuller than it declared, which is
	 * exactly the failure this replaces, and it would teach everyone
	 * that the declaration is decorative.
	 */
	if (write_builddeps_recipe("declaredmissing", "1.0", tarball_path, sha256,
	                            "nosuchbuildtool") != 0) {
		fprintf(stderr, "FAIL: could not write the declared-build-deps recipe\n");
		ok = 0;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"declaredmissing\"}",
	                       &r) != 0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: #109 install of a declared-tools recipe, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (poll_pkg_state(&client, "declaredmissing", state, sizeof(state), 200) != 0 ||
	    strcmp(state, "failed") != 0) {
		fprintf(stderr, "FAIL: #109 a recipe declaring an unavailable build tool ended '%s', "
		                "expected failed -- it must not fall back to the shared sandbox\n",
		        state);
		ok = 0;
	} else {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/declaredmissing", NULL, &r) != 0 ||
		    r.status != 200 || json_str_field(r.json, "error") == NULL ||
		    strstr(json_str_field(r.json, "error"), "nosuchbuildtool") == NULL) {
			fprintf(stderr, "FAIL: #109 the failure does not name the missing build tool: %s\n",
			        r.json != NULL && json_str_field(r.json, "error") != NULL
			            ? json_str_field(r.json, "error")
			            : "(none)");
			ok = 0;
		}
		cix_response_free(&r);
	}

	/*
	 * Issue #109, the other half: a recipe declaring a tool that IS
	 * available must actually compose an environment from it.
	 *
	 * "greeter" is a real package installed earlier in this test, so it
	 * has a real recorded file list -- which is precisely what a build
	 * environment is composed from. The build itself is still expected
	 * to fail (a composed environment holds the declared tools and
	 * nothing else, and greeter is not a C compiler) -- the point is
	 * WHICH failure: reaching a real build error proves the
	 * composition ran, where "could not compose" would mean it never
	 * got that far. Without this case only the refusal path was
	 * covered, and a composition that failed for every input would
	 * still have looked green.
	 */
	if (write_builddeps_recipe("declaredpresent", "1.0", tarball_path, sha256, "greeter") != 0) {
		fprintf(stderr, "FAIL: could not write the available-build-deps recipe\n");
		ok = 0;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install",
	                       "{\"name\":\"declaredpresent\"}", &r) != 0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: #109 install of an available-declared-tools recipe, status=%d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (poll_pkg_state(&client, "declaredpresent", state, sizeof(state), 600) != 0) {
		fprintf(stderr, "FAIL: #109 available-declared-tools install never settled (last state '%s')\n", state);
		ok = 0;
	} else {
		const char *err;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/declaredpresent", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #109 could not read back the available-declared-tools pkg\n");
			ok = 0;
		} else {
			err = json_str_field(r.json, "error");
			if (err != NULL && strstr(err, "compose") != NULL) {
				fprintf(stderr,
				        "FAIL: #109 a declared tool that IS installed still failed to compose "
				        "a build environment: %s\n",
				        err);
				ok = 0;
			}
		}
		cix_response_free(&r);
	}

	/*
	 * Issue #127: a build tool may be version-pinned as name@version.
	 * A pin to the version actually installed (greeter@1.0-1) must
	 * resolve and compose exactly like the bare-name case above; a pin
	 * to a version that is NOT installed (greeter@9.9) must fail with
	 * the same "not installed anywhere" refusal an entirely-unknown
	 * tool gets -- never silently ignore the pin and match the wrong
	 * version, which is what happened before (the whole "name@version"
	 * string was compared against bare package names and matched
	 * nothing, so even the correct pin failed).
	 */
	if (write_builddeps_recipe("pinnedgood", "1.0", tarball_path, sha256, "greeter@1.0-1") != 0 ||
	    write_builddeps_recipe("pinnedbad", "1.0", tarball_path, sha256, "greeter@9.9") != 0) {
		fprintf(stderr, "FAIL: could not write the version-pinned build-deps recipes\n");
		ok = 0;
	}
	/* good pin: composes (build then fails because greeter is not a
	 * compiler, exactly like declaredpresent -- the point is it got
	 * past composition). */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"pinnedgood\"}", &r) !=
	        0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: #127 install of a correctly-pinned recipe, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (poll_pkg_state(&client, "pinnedgood", state, sizeof(state), 600) != 0) {
		fprintf(stderr, "FAIL: #127 correctly-pinned install never settled (last '%s')\n", state);
		ok = 0;
	} else {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/pinnedgood", NULL, &r) == 0 &&
		    r.status == 200) {
			const char *err = json_str_field(r.json, "error");

			if (err != NULL && (strstr(err, "compose") != NULL ||
			                    strstr(err, "not installed anywhere") != NULL)) {
				fprintf(stderr, "FAIL: #127 greeter@1.0 (the installed version) did not "
				                "resolve: %s\n", err);
				ok = 0;
			}
		}
		cix_response_free(&r);
	}
	/* bad pin: must fail, naming the pinned tool -- the wrong version
	 * is not installed, so it is as unavailable as an unknown name. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"pinnedbad\"}", &r) !=
	        0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: #127 install of a wrong-version-pinned recipe, status=%d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (poll_pkg_state(&client, "pinnedbad", state, sizeof(state), 200) != 0 ||
	    strcmp(state, "failed") != 0) {
		fprintf(stderr, "FAIL: #127 a pin to an uninstalled version ended '%s', expected "
		                "failed\n", state);
		ok = 0;
	} else {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/pinnedbad", NULL, &r) == 0 &&
		    r.status == 200) {
			const char *err = json_str_field(r.json, "error");

			if (err == NULL || strstr(err, "greeter@9.9") == NULL) {
				fprintf(stderr, "FAIL: #127 the wrong-version-pin failure does not name "
				                "greeter@9.9: %s\n", err != NULL ? err : "(none)");
				ok = 0;
			}
		}
		cix_response_free(&r);
	}

	/*
	 * Issue #166: a package that honestly declares ITSELF as a build
	 * tool must still be upgradable in place.
	 *
	 * tcc compiles tcc; gcc bootstraps gcc. Both correctly name
	 * themselves in pkg_build_depends, and neither could ever be
	 * upgraded: start_fetch_for() sets the entry to FETCHING before
	 * composing anything, so the only entry matching the name was no
	 * longer INSTALLED and composition failed with "declared build
	 * tool ... is not installed anywhere" -- while GET /v1/pkg showed
	 * it installed throughout.
	 *
	 * The files are genuinely present the whole time. Image versions
	 * are immutable, so the image's current version still holds the
	 * old package until a new version is produced at the end, which is
	 * why start_fetch_for() deliberately leaves e->version/e->files
	 * alone through an in-place upgrade.
	 *
	 * Install 1.0 with the ordinary build floor so it genuinely
	 * reaches INSTALLED -- an in-place upgrade is only an in-place
	 * upgrade from a real prior install -- then upgrade to 2.0, which
	 * adds ITSELF to that same floor. The proof is that the upgrade
	 * does not come back with the composition refusal.
	 */
	if (write_builddeps_recipe("selfdep", "1.0", tarball_path, sha256,
	                           "tcc linux-headers bash coreutils binutils") != 0) {
		fprintf(stderr, "FAIL: could not write the self-dependency recipe\n");
		ok = 0;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"selfdep\"}", &r) != 0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: #166 install of selfdep 1.0, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (poll_pkg_state(&client, "selfdep", state, sizeof(state), 600) != 0) {
		fprintf(stderr, "FAIL: #166 selfdep 1.0 never settled (last state '%s')\n", state);
		ok = 0;
	} else if (strcmp(state, "installed") != 0) {
		/*
		 * The upgrade below is only meaningful from a genuinely
		 * INSTALLED entry -- that is the whole precondition. Say so
		 * rather than letting the real assertion report something
		 * that is really about this step.
		 */
		fprintf(stderr, "FAIL: #166 precondition lost -- selfdep 1.0 ended '%s', not "
		                "'installed', so the self-declaring upgrade below would not be "
		                "exercising an in-place upgrade at all\n", state);
		ok = 0;
	} else {
		if (write_builddeps_recipe("selfdep", "2.0", tarball_path, sha256,
		                           "selfdep tcc linux-headers bash coreutils binutils") != 0) {
			fprintf(stderr, "FAIL: could not write the self-declaring upgrade recipe\n");
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"selfdep\",\"version\":\"2.0\",\"upgrade\":true}",
		                       &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: #166 self-declaring upgrade, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		if (poll_pkg_state(&client, "selfdep", state, sizeof(state), 600) != 0) {
			fprintf(stderr, "FAIL: #166 self-declaring upgrade never settled (last state '%s')\n",
			        state);
			ok = 0;
		} else {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/selfdep", NULL, &r) == 0 &&
			    r.status == 200) {
				const char *err = json_str_field(r.json, "error");

				if (err != NULL && strstr(err, "not installed anywhere") != NULL) {
					fprintf(stderr,
					        "FAIL: #166 a package declaring itself as a build tool cannot be "
					        "upgraded -- its own entry is mid-upgrade, but its files are still "
					        "in the image's current version the whole time: %s\n",
					        err);
					ok = 0;
				}
			}
			cix_response_free(&r);
		}
	}

	/*
	 * ...and the environment it composed must not be EMPTY. image_create()
	 * gives every new image a current version straight away, so an image
	 * that exists is not the same thing as an image that was filled --
	 * conflating the two shipped a build environment holding nothing at
	 * all, which then failed at execve() of a shell that was supposed to
	 * be in it. Asserting the version moved off the empty-manifest one is
	 * what tells those two states apart.
	 */
	{
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/images", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #109 could not list images to check the composed env\n");
			ok = 0;
		} else {
			/*
			 * ADR-0209, replacing what #109 asserted here.
			 *
			 * This used to hunt for a surviving "__buildenv-*" image as
			 * proof that composition had happened. A build environment
			 * is now destroyed the moment its build ends, so the thing
			 * it looked for is exactly what must NOT be there --
			 * leftovers are how a box accumulates images nobody can
			 * account for, which is the whole disease being cured.
			 *
			 * Composition is still proven, just not by debris: with no
			 * fallback environment anywhere, a build that succeeds
			 * could only have run in one composed from its declared
			 * tools. The successful installs above are that proof. What
			 * remains to check is that nothing was left behind.
			 */
			const struct json_value *arr = json_object_get(r.json, "images");
			size_t n = (arr != NULL && arr->type == JSON_ARRAY) ? arr->u.array.count : 0;
			size_t k;

			for (k = 0; k < n; k++) {
				const struct json_value *im = arr->u.array.items[k];
				const char *nm = im != NULL ? json_str_field(im, "name") : NULL;

				if (nm != NULL && strncmp(nm, "__buildenv-", 11) == 0) {
					fprintf(stderr,
					        "FAIL: #168 build environment %s outlived its build -- it must be "
					        "torn down when the build ends (ADR-0209)\n",
					        nm);
					ok = 0;
				}
			}
		}
		cix_response_free(&r);
	}

	/*
	 * A command that fails in the middle of pkg_install() must fail the
	 * package. Without `set -e` -- and with `&&` between pkg_build and
	 * pkg_install, which POSIX says suspends set -e inside them -- the
	 * function simply carries on and returns the last command's status.
	 * A real libcap was recorded as installed that way with four of its
	 * binaries missing, which nothing downstream could detect.
	 */
	if (write_midfail_recipe(&client, "midfail", "1.0", tarball_path, sha256) != 0) {
		fprintf(stderr, "FAIL: could not write the mid-failure recipe\n");
		ok = 0;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"midfail\"}", &r) !=
	        0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: POST install midfail, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	if (poll_pkg_state(&client, "midfail", state, sizeof(state), 200) != 0 ||
	    strcmp(state, "failed") != 0) {
		fprintf(stderr,
		        "FAIL: a recipe whose pkg_install() failed midway ended '%s', expected failed "
		        "-- a partially installed package must not be recorded as installed\n",
		        state);
		ok = 0;
	}

	/*
	 * A package file landing where a SYMLINK already exists must
	 * replace it, not write through it.
	 *
	 * copy_file_simple() opens the destination O_CREAT|O_TRUNC, which
	 * follows a symlink -- so installing over one used to write to the
	 * link's target instead. The visible failure was the lucky case (a
	 * dangling link, so the open failed and the merge stopped); a link
	 * pointing at a real file would have silently overwritten
	 * something unrelated and reported success. Real gcc hit this on a
	 * Debian-alternatives symlink left in a build sandbox.
	 *
	 * The staged symlink points at a decoy this test owns, so if the
	 * write goes through the link instead of replacing it, the decoy
	 * changes and that is detectable.
	 */
	{
		char link_path[PATH_MAX];
		char decoy_path[PATH_MAX];
		FILE *df;
		struct stat lst;

		snprintf(decoy_path, sizeof(decoy_path), "%s/decoy.txt", scratch_dir);
		df = fopen(decoy_path, "w");
		if (df != NULL) {
			fputs("untouched\n", df);
			fclose(df);
		}
		/* base image's own usr/bin, where greeter installs */
		snprintf(link_path, sizeof(link_path), "%s/rebuildable/images/base/%s/rootfs/usr/bin/relinked",
		         g_data_dir, "current");
		/* Resolve "current" the way the daemon does: ask for the image. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/images/base", NULL, &r) == 0 &&
		    r.status == 200 && json_str_field(r.json, "current_version") != NULL) {
			snprintf(link_path, sizeof(link_path),
			         "%s/rebuildable/images/base/%s/rootfs/usr/bin/relinked", g_data_dir,
			         json_str_field(r.json, "current_version"));
			unlink(link_path);
			if (symlink(decoy_path, link_path) != 0)
				fprintf(stderr, "WARN: could not stage the symlink fixture\n");
		}
		cix_response_free(&r);

		if (write_recipe(&client, "relinked", "1.0", tarball_path, sha256, NULL) != 0)
			ok = 0;
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"relinked\"}",
		                       &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install relinked, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		if (poll_pkg_state(&client, "relinked", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr,
			        "FAIL: installing over an existing symlink ended '%s', expected installed\n",
			        state);
			ok = 0;
		} else {
			char decoy_buf[64];
			FILE *cf = fopen(decoy_path, "r");

			decoy_buf[0] = '\0';
			if (cf != NULL) {
				if (fgets(decoy_buf, sizeof(decoy_buf), cf) == NULL)
					decoy_buf[0] = '\0';
				fclose(cf);
			}
			if (strncmp(decoy_buf, "untouched", 9) != 0) {
				fprintf(stderr,
				        "FAIL: installing over a symlink wrote THROUGH it -- the decoy the link "
				        "pointed at was overwritten\n");
				ok = 0;
			}
			/* An install produces a NEW image version, so the file
			 * landed in that one -- the path staged above belongs to
			 * the version that was current beforehand. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/images/base", NULL, &r) == 0 &&
			    r.status == 200 && json_str_field(r.json, "current_version") != NULL) {
				char new_path[PATH_MAX];

				snprintf(new_path, sizeof(new_path),
				         "%s/rebuildable/images/base/%s/rootfs/usr/bin/relinked", g_data_dir,
				         json_str_field(r.json, "current_version"));
				memset(&lst, 0, sizeof(lst));
				if (lstat(new_path, &lst) != 0) {
					fprintf(stderr, "FAIL: the package file is missing from the new image "
					                "version (%s)\n", new_path);
					ok = 0;
				} else if (S_ISLNK(lst.st_mode)) {
					fprintf(stderr,
					        "FAIL: the symlink survived; the package file did not replace it\n");
					ok = 0;
				}
			}
			cix_response_free(&r);
		}
	}

	/* 6. duplicate install of an already-installed package -> 409 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"greeter\"}", &r) != 0 ||
	    r.status != 409) {
		fprintf(stderr, "FAIL: duplicate install expected 409, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 7. checksum mismatch -> ends FAILED, never installed */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"badsum\"}", &r) != 0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: POST install badsum, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

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
		if (cix_client_request(&client, "DELETE", "/v1/pkg/badsum", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE badsum (failed state) expected 204, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/badsum", NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: GET badsum after delete expected 404, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
	if (cix_client_request(&client, "DELETE", "/v1/pkg/greeter", NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: DELETE greeter expected 204, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	{
		struct stat st;

		if (stat(base_path("/usr/bin/greeter"), &st) == 0) {
			fprintf(stderr, "FAIL: greeter binary still exists in base image after delete\n");
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pkg/greeter", NULL, &r) != 0 || r.status != 404) {
		fprintf(stderr, "FAIL: GET greeter after delete expected 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* 8b. same cleanup for "concurrent" (step 4's second chain) -- left
	 * installed until now so later steps don't have to account for its
	 * presence; nothing past this point depends on it. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "DELETE", "/v1/pkg/concurrent", NULL, &r) != 0 || r.status != 204) {
		fprintf(stderr, "FAIL: DELETE concurrent expected 204, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	{
		struct stat st;

		if (stat(base_path("/usr/bin/concurrent"), &st) == 0) {
			fprintf(stderr, "FAIL: concurrent binary still exists in base image after delete\n");
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pkg/concurrent", NULL, &r) != 0 || r.status != 404) {
		fprintf(stderr, "FAIL: GET concurrent after delete expected 404, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

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
		} else if (write_recipe(&client, "leaf", "1.0", leaf_tarball, leaf_sha, "") != 0 ||
		           write_recipe(&client, "top", "1.0", top_tarball, top_sha, "leaf") != 0) {
			fprintf(stderr, "FAIL: could not write leaf/top recipes\n");
			ok = 0;
		} else {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"top\"}", &r) !=
			        0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: POST install top, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

			if (poll_pkg_state(&client, "leaf", state, sizeof(state), 200) != 0 ||
			    strcmp(state, "installed") != 0) {
				fprintf(stderr, "FAIL: leaf (top's dependency) did not reach installed\n");
				ok = 0;
			}
			if (poll_pkg_state(&client, "top", state, sizeof(state), 200) != 0 ||
			    strcmp(state, "installed") != 0) {
				fprintf(stderr, "FAIL: top did not reach installed\n");
				ok = 0;
			}
		}
	}

	/* 10. circular dependency -> 400, nothing registered */
	if (write_recipe(&client, "circ1", "1.0", tarball_path, sha256, "circ2") != 0 ||
	    write_recipe(&client, "circ2", "1.0", tarball_path, sha256, "circ1") != 0) {
		fprintf(stderr, "FAIL: could not write circular recipes\n");
		ok = 0;
	} else {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"circ1\"}", &r) !=
		        0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: circular dependency install expected 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/circ1", NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: circ1 should never have been registered, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/* 11. a dependency with no matching recipe -> 400 */
	if (write_recipe(&client, "needsghost", "1.0", tarball_path, sha256, "ghost") != 0) {
		fprintf(stderr, "FAIL: could not write needsghost recipe\n");
		ok = 0;
	} else {
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"needsghost\"}", &r) !=
		        0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: missing dependency install expected 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
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
		} else if (write_recipe(&client, "leaf", "2.0", leaf2_tarball, leaf2_sha, "") != 0) {
			fprintf(stderr, "FAIL: could not write leaf 2.0 recipe\n");
			ok = 0;
		} else {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/leaf", NULL, &r) != 0 ||
			    r.status != 200 || !str_eq(json_str_field(r.json, "available_version"), "2.0-1")) {
				fprintf(stderr,
				        "FAIL: leaf should show available_version=2.0-1 before upgrading, got %s\n",
				        json_str_field(r.json, "available_version")
				            ? json_str_field(r.json, "available_version")
				            : "(null)");
				ok = 0;
			}
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"leaf\"}", &r) !=
			        0 ||
			    r.status != 409) {
				fprintf(stderr, "FAIL: re-install without upgrade expected 409, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install",
			                       "{\"name\":\"leaf\",\"upgrade\":true}", &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: upgrade install expected 202, got %d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

			if (poll_pkg_state(&client, "leaf", state, sizeof(state), 200) != 0 ||
			    strcmp(state, "installed") != 0) {
				fprintf(stderr, "FAIL: leaf upgrade did not reach installed\n");
				ok = 0;
			} else {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/leaf", NULL, &r) != 0 ||
				    !str_eq(json_str_field(r.json, "version"), "2.0-1") ||
				    json_str_field(r.json, "available_version") != NULL) {
					fprintf(stderr,
					        "FAIL: leaf after upgrade should be version=2.0-1, "
					        "available_version=null\n");
					ok = 0;
				}
				cix_response_free(&r);

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
		char rstate[64];

		/*
		 * The C library first, because this image is going to run
		 * something (#186). It used to arrive by itself: creation and
		 * every install copied a loader and libc off the build host
		 * into whatever image was being written. Now an image gets one
		 * the same way it gets anything else, so the guarantee below --
		 * that greeter can actually execve() out of this image -- is
		 * still asserted, but it is earned by a package rather than
		 * granted by a copy.
		 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"glibc\",\"image\":\"router\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install glibc@router, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		if (poll_pkg_state(&client, "glibc@router", rstate, sizeof(rstate), 300) != 0 ||
		    strcmp(rstate, "installed") != 0) {
			fprintf(stderr, "FAIL: glibc@router did not install (state=%s)\n", rstate);
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"greeter\",\"image\":\"router\"}", &r) != 0 ||
		    r.status != 202 || !str_eq(json_str_field(r.json, "image"), "router")) {
			fprintf(stderr, "FAIL: POST install greeter@router, status=%d, image=%s\n", r.status,
			        json_str_field(r.json, "image") ? json_str_field(r.json, "image") : "(null)");
			ok = 0;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client, "greeter@router", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: greeter@router did not reach installed\n");
			ok = 0;
		} else {
			struct stat st;

			if (stat(router_path("/usr/bin/greeter"), &st) != 0) {
				fprintf(stderr, "FAIL: greeter@router binary missing from router image\n");
				ok = 0;
			}
			/* The same guarantee this has always asserted -- an image
			 * carries the C runtime the binaries in it need to
			 * execve() -- reached by the right route (#186). It used
			 * to be a side effect of installing anything at all, since
			 * the baseline copied a loader and libc off the build host
			 * on every install. It is now what installing the glibc
			 * package above actually delivers, which is why that
			 * install is part of this step rather than assumed.
			 *
			 * libtinfo is deliberately NOT checked (ADR-0209): it is
			 * ncurses, a package this project builds itself, and
			 * copying the host's copy into every image was content
			 * arriving by mechanism rather than declaration. */
			if (stat(router_path("/lib64/ld-linux-x86-64.so.2"), &st) != 0 ||
			    stat(router_path("/lib/x86_64-linux-gnu/libc.so.6"), &st) != 0) {
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
		if (cix_client_request(&client, "GET", "/v1/pkg/greeter", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: GET greeter (base) expected 404, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* @-addressed GET reaches the router entry specifically */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/greeter@router", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "image"), "router") ||
		    !str_eq(json_str_field(r.json, "state"), "installed")) {
			fprintf(stderr, "FAIL: GET greeter@router expected 200 installed router, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* a fresh install of the SAME name back into the default image
		 * is independent -- greeter@router being installed must not
		 * make this a 409 duplicate */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"greeter\"}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr,
			        "FAIL: POST install greeter (base) while greeter@router installed "
			        "expected 202, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client, "greeter", state, sizeof(state), 200) != 0 ||
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
		if (cix_client_request(&client, "GET", "/v1/pkg", NULL, &r) != 0 || r.status != 200) {
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
		cix_response_free(&r);

		/* @-addressed DELETE removes only the router entry, leaving the
		 * independently-installed base entry untouched */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/pkg/greeter@router", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE greeter@router expected 204, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
		if (cix_client_request(&client, "POST", "/v1/pkg/update-all", "{}", &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "status"), "nothing to update")) {
			fprintf(stderr,
			        "FAIL: update-all with nothing drifted expected 200 \"nothing to update\", got %d %s\n",
			        r.status,
			        json_str_field(r.json, "status") ? json_str_field(r.json, "status") : "(null)");
			ok = 0;
		}
		cix_response_free(&r);

		{
			char top2_tarball[512], top2_sha[128];

			if (stage_fixture_tarball(scratch_dir, "top", "2.0", top2_tarball, sizeof(top2_tarball),
			                           top2_sha, sizeof(top2_sha)) != 0) {
				fprintf(stderr, "FAIL: could not stage top 2.0 fixture\n");
				ok = 0;
			} else if (write_recipe(&client, "top", "2.0", top2_tarball, top2_sha, "leaf") != 0) {
				fprintf(stderr, "FAIL: could not write top 2.0 recipe\n");
				ok = 0;
			} else {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "POST", "/v1/pkg/update-all", "{}", &r) != 0 ||
				    r.status != 202 || !str_eq(json_str_field(r.json, "name"), "top")) {
					fprintf(stderr,
					        "FAIL: update-all with top drifted expected 202 name=top, got %d name=%s\n",
					        r.status,
					        json_str_field(r.json, "name") ? json_str_field(r.json, "name") : "(null)");
					ok = 0;
				}
				cix_response_free(&r);

				if (poll_pkg_state(&client, "top", state, sizeof(state), 200) != 0 ||
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
				if (cix_client_request(&client, "POST", "/v1/pkg/update-all", "{}", &r) != 0 ||
				    r.status != 200 ||
				    !str_eq(json_str_field(r.json, "status"), "nothing to update")) {
					fprintf(stderr,
					        "FAIL: update-all after draining backlog expected 200 \"nothing to "
					        "update\", got %d\n",
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);
			}
		}
	}

	/*
	 * Issue #109: when the same package is installed at different
	 * versions in different images, the composed environment must take
	 * the NEWEST -- deterministically, not whichever the registry
	 * happened to list first.
	 *
	 * Not a theoretical tidiness. A real build failed exactly here:
	 * `libc-dev` resolved to 2.36, which does not stage libm.so, while
	 * 2.36-3, which does, sat installed in two other images. And plain
	 * string ordering is not enough either -- "2.36-3" sorts before
	 * "2.36" by strcmp, and "1.3.2-10" before "1.3.2-9".
	 *
	 * "stamped" installs a file naming the version that produced it,
	 * which is how this reads back which copy actually won.
	 */
	{
		char stamp_path[PATH_MAX];
		FILE *sf;
		char line[64];

		if (write_stamped_recipe(&client, "stamped", "1.9", tarball_path, sha256) != 0 ||
		    write_stamped_recipe(&client, "stamped", "1.10", tarball_path, sha256) != 0) {
			fprintf(stderr, "FAIL: could not write the stamped recipes\n");
			ok = 0;
		}
		/* 1.10 into one image, 1.9 into another -- 1.10 is newer, and
		 * is also the one a plain strcmp() would rank lower. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"stamped\",\"version\":\"1.10-1\","
		                       "\"image\":\"vnew\"}",
		                       &r) != 0 ||
		    r.status != 202)
			ok = 0;
		cix_response_free(&r);
		if (poll_pkg_state(&client, "stamped@vnew", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: #109 stamped 1.10 ended '%s'\n", state);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"stamped\",\"version\":\"1.9-1\","
		                       "\"image\":\"vold\"}",
		                       &r) != 0 ||
		    r.status != 202)
			ok = 0;
		cix_response_free(&r);
		if (poll_pkg_state(&client, "stamped@vold", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: #109 stamped 1.9 ended '%s'\n", state);
			ok = 0;
		}

		/*
		 * ADR-0209: the consuming recipe now REPORTS what it saw.
		 *
		 * This property -- that declaring a tool gets you the version
		 * you declared -- used to be checked by rummaging in a
		 * surviving __buildenv-* image afterwards. Build environments
		 * are destroyed with their builds now, so there is nothing to
		 * rummage in; and observing from inside the build is a better
		 * test anyway, because it proves what the build actually had
		 * rather than what was lying around when it finished.
		 */
		if (write_observing_recipe("usesstamped", "1.0", tarball_path, sha256,
		                            "stamped@1.9-1 tcc linux-headers bash coreutils") != 0)
			ok = 0;
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"usesstamped\"}", &r) != 0 ||
		    r.status != 202)
			ok = 0;
		cix_response_free(&r);
		if (poll_pkg_state(&client, "usesstamped", state, sizeof(state), 400) != 0) {
			fprintf(stderr, "FAIL: #109 usesstamped never settled\n");
			ok = 0;
		}

		/*
		 * Which version did the build actually run against? Asked of
		 * the build's own output, not of leftover images: usesstamped
		 * copied the stamp its declared tool left in the environment
		 * into its own package, so the installed file is a first-hand
		 * record. 1.9 is what it declared; 1.10 exists purely to prove
		 * "newest wins" is not what happens.
		 */
		{
			char observed[128] = { 0 };
			char bver[128];
			FILE *of;

			if (test_image_fixture_read_current_version(g_images_base_dir, bver, sizeof(bver)) != 0)
				bver[0] = '\0';
			snprintf(stamp_path, sizeof(stamp_path),
			         "%s/rebuildable/images/base/%s/rootfs/usr/share/observed.version", g_data_dir,
			         bver);
			of = fopen(stamp_path, "r");
			if (of == NULL) {
				fprintf(stderr,
				        "FAIL: #109 usesstamped did not record which stamped version its "
				        "environment held (%s)\n",
				        stamp_path);
				ok = 0;
			} else {
				if (fgets(observed, sizeof(observed), of) == NULL)
					observed[0] = '\0';
				fclose(of);
				observed[strcspn(observed, "\r\n")] = '\0';
				if (strcmp(observed, "1.9-1") != 0) {
					fprintf(stderr,
					        "FAIL: #109 the composed environment held stamped '%s', expected the "
					        "declared 1.9-1 -- a declaration must pin the version, not resolve to "
					        "the newest\n",
					        observed);
					ok = 0;
				}
			}
		}
		cix_response_free(&r);
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
			if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"multisrc\"}", &r) !=
			        0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: POST install multisrc, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

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
			if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"multisrcbad\"}",
			                       &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: POST install multisrcbad, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

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
		 * addressed via a URL with "?ref=deadbeef" appended; the loopback
		 * server (test_image_fixture.c) ignores the query string exactly
		 * as a real HTTP host would, so this
		 * exercises the real bug. */
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
				if (cix_client_request(&client, "POST", "/v1/pkg/install",
				                       "{\"name\":\"multisrcquery\"}", &r) != 0 ||
				    r.status != 202) {
					fprintf(stderr, "FAIL: POST install multisrcquery, status=%d\n", r.status);
					ok = 0;
				}
				cix_response_free(&r);

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
		         "pkg_name=apirecipe\npkg_version=1.0\npkg_source=%s\n"
		         "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
		         "pkg_build() {\n\ttcc -o hello hello.c\n}\n\n"
		         "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
		         "\"$PKG_DESTDIR/usr/bin/apirecipe\"\n}\n",
		         test_http_src(api_tarball), api_sha);

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
		if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: POST recipe with name/pkg_name= mismatch, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
		jw_free(&w);

		/*
		 * #494: two recipe versions that collide on one ARTIFACT
		 * name. The artifact server reads a missing release as
		 * release 1, so `X` and `X-1` are one object there and only
		 * the first to build can ever publish -- the second rebuilds
		 * from source forever and can never be approved.
		 *
		 * Both orders, because the check has to hold whichever is
		 * published first, and a 409 that is NOT the immutability
		 * 409: this version is not published, and saying it is sends
		 * the author looking for a recipe that does not exist.
		 */
		{
			char coll[2048];
			char coll_name[32];
			int i;
			static const char *const pairs[][2] = {
				{ "9.9.9", "9.9.9-1" },
				{ "8.8.8-1", "8.8.8" },
			};

			for (i = 0; i < 2; i++) {
				int first_status = 0, second_status = 0;
				int k;

				for (k = 0; k < 2; k++) {
					snprintf(coll, sizeof(coll),
					         "pkg_name=collide%d\npkg_version=%s\n"
					         "pkg_source=%s\npkg_sha256=%s\n"
					         "pkg_depends=\"\"\npkg_build_depends=\"\"\n"
					         "pkg_build() { :; }\n"
					         "pkg_install() { mkdir -p \"$PKG_DESTDIR/usr/bin\"; "
					         ": > \"$PKG_DESTDIR/usr/bin/collide\"; }\n",
					         i, pairs[i][k], api_tarball, api_sha);
					jw_init(&w);
					jw_obj_open(&w);
					jw_key(&w, "name");
					snprintf(coll_name, sizeof(coll_name), "collide%d", i);
					jw_str(&w, coll_name);
					jw_key(&w, "content");
					jw_str(&w, coll);
					jw_obj_close(&w);
					w.buf[w.len] = '\0';
					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0) {
						fprintf(stderr, "FAIL: #494 publish request failed\n");
						ok = 0;
					}
					if (k == 0)
						first_status = r.status;
					else
						second_status = r.status;
					cix_response_free(&r);
					jw_free(&w);
				}
				if (first_status != 204) {
					fprintf(stderr, "FAIL: #494 first publish (%s) status=%d, want 204\n",
					        pairs[i][0], first_status);
					ok = 0;
				}
				if (second_status != 409) {
					fprintf(stderr,
					        "FAIL: #494 publishing %s after %s status=%d, want 409 -- both "
					        "publish under the same artifact name\n",
					        pairs[i][1], pairs[i][0], second_status);
					ok = 0;
				}
			}
		}

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
		if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: POST malformed recipe, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
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
		if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: POST valid recipe via API, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		jw_free(&w);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"apirecipe\"}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install apirecipe (added via API), status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
			         "pkg_name=apirecipe\npkg_version=2.0\npkg_source=%s\n"
			         "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
			         "pkg_build() {\n\ttcc -o hello hello.c\n}\n\n"
			         "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
			         "\"$PKG_DESTDIR/usr/bin/apirecipe\"\n}\n",
			         test_http_src(api_tarball), api_sha);
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "apirecipe");
			jw_key(&w, "content");
			jw_str(&w, body2);
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
			    r.status != 204) {
				fprintf(stderr, "FAIL: upsert apirecipe to 2.0, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
			jw_free(&w);
		}
		/* ADR-0107: recipe versions are immutable and multi-version,
		 * not upsert-by-name -- the list must now show BOTH 1.0 and
		 * 2.0 as separate, independently-published entries for
		 * "apirecipe", neither one replacing the other. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/recipes", NULL, &r) != 0 || r.status != 200) {
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
		cix_response_free(&r);

		/* GET /v1/pkg/recipes/{name} (Phase 16, packages-tree UI): the
		 * raw .recipe text too, not just the list view's metadata --
		 * must reflect the 2.0 upsert above, byte for byte. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/recipes/apirecipe", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET recipe content, status=%d\n", r.status);
			ok = 0;
		} else if (!str_eq(json_str_field(r.json, "name"), "apirecipe") ||
		           !str_eq(json_str_field(r.json, "version"), "2.0") ||
		           !str_eq(json_str_field(r.json, "content"), body2)) {
			fprintf(stderr, "FAIL: GET recipe content mismatch after upsert\n");
			ok = 0;
		}
		cix_response_free(&r);

		/* An unknown recipe name -> 404, not a raw-id-style fallback
		 * (there is no such fallback for recipes -- a name either has
		 * a recipe on file or it doesn't). */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/recipes/never-added-recipe", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: GET unknown recipe content expected 404, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* DELETE removes it; a subsequent install attempt fails again */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/pkg/recipes/apirecipe", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE apirecipe recipe, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* GET after DELETE -> 404 too (recipe genuinely gone, not just
		 * uninstalled). */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/recipes/apirecipe", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: GET recipe content after delete expected 404, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"apirecipe2\"}", &r) !=
		        0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: install of a never-added name should 400, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* DELETE of something never added -> 404 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/pkg/recipes/apirecipe2", NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: DELETE of a never-added recipe should 404, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
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
		if (cix_client_request(&client, "POST", "/v1/images", "{\"name\":\"rollingtest\"}", &r) !=
		        0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST rollingtest image, status=%d\n", r.status);
			ok = 0;
			goto skip_rolling_rebuild;
		}
		cix_response_free(&r);

		if (stage_fixture_tarball(scratch_dir, "rollpkg", "1.0", tarball1, sizeof(tarball1), sha1,
		                           sizeof(sha1)) != 0 ||
		    write_recipe(&client, "rollpkg", "1.0", tarball1, sha1, NULL) != 0) {
			fprintf(stderr, "FAIL: could not stage/write rollpkg 1.0\n");
			ok = 0;
			goto skip_rolling_rebuild;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"rollpkg\",\"image\":\"rollingtest\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install rollpkg@rollingtest, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client, "rollpkg@rollingtest", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: rollpkg@rollingtest (1.0) did not reach installed\n");
			ok = 0;
			goto skip_rolling_rebuild;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/images/rollingtest/manifest",
		                       "{\"package\":\"rollpkg\",\"mode\":\"rolling\",\"version\":\"1.0\"}",
		                       &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: POST rollingtest manifest (rollpkg rolling@1.0), status=%d\n",
			        r.status);
			ok = 0;
			goto skip_rolling_rebuild;
		}
		cix_response_free(&r);

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
			         "pkg_name=rollpkg\npkg_version=2.0\npkg_source=%s\n"
			         "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
			         "pkg_build() {\n\ttcc -o hello hello.c\n}\n\n"
			         "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
			         "\"$PKG_DESTDIR/usr/bin/rollpkg\"\n}\n",
			         test_http_src(tarball2), sha2);
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "rollpkg");
			jw_key(&w, "content");
			jw_str(&w, body2);
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
			    r.status != 204) {
				fprintf(stderr, "FAIL: publish rollpkg 2.0, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
			jw_free(&w);

			/* No install/upgrade request follows -- the daemon's own
			 * rolling-rebuild trigger must do this by itself. */
			state[0] = '\0';
			for (i = 0; i < 60; i++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/rollpkg@rollingtest", NULL, &r) ==
				        0 &&
				    r.status == 200) {
					const char *st = json_str_field(r.json, "state");
					const char *ver = json_str_field(r.json, "version");

					if (st != NULL)
						snprintf(state, sizeof(state), "%s", st);
					if (st != NULL && strcmp(st, "installed") == 0 && ver != NULL &&
					    strcmp(ver, "2.0") == 0) {
						cix_response_free(&r);
						break;
					}
				}
				cix_response_free(&r);
				usleep(300000);
			}
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/rollpkg@rollingtest", NULL, &r) != 0 ||
			    r.status != 200 || !str_eq(json_str_field(r.json, "state"), "installed") ||
			    !str_eq(json_str_field(r.json, "version"), "2.0")) {
				fprintf(stderr,
				        "FAIL: rollpkg@rollingtest was not auto-rebuilt to 2.0 (last state "
				        "seen: %s)\n",
				        state);
				ok = 0;
			}
			cix_response_free(&r);

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
		if (cix_client_request(&client, "POST", "/v1/images", "{\"name\":\"pintest\"}", &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST pintest image, status=%d\n", r.status);
			ok = 0;
			goto skip_pin_isolation;
		}
		cix_response_free(&r);

		if (stage_fixture_tarball(scratch_dir, "pinpkg", "1.0", tarball1, sizeof(tarball1), sha1,
		                           sizeof(sha1)) != 0 ||
		    write_recipe(&client, "pinpkg", "1.0", tarball1, sha1, NULL) != 0) {
			fprintf(stderr, "FAIL: could not stage/write pinpkg 1.0\n");
			ok = 0;
			goto skip_pin_isolation;
		}

		/* A container is created from this image further down, so it
		 * needs a C library to execve pinpkg at all (#186). That used
		 * to be a side effect of any install; it is a package now, and
		 * POST /v1/containers refuses an image without one rather than
		 * letting it fail later as exit 127. */
		{
			char pstate[64];

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install",
			                       "{\"name\":\"glibc\",\"image\":\"pintest\"}", &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: POST install glibc@pintest, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
			if (poll_pkg_state(&client, "glibc@pintest", pstate, sizeof(pstate), 300) != 0 ||
			    strcmp(pstate, "installed") != 0) {
				fprintf(stderr, "FAIL: glibc@pintest did not install (state=%s)\n", pstate);
				ok = 0;
			}
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"pinpkg\",\"image\":\"pintest\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install pinpkg@pintest, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client, "pinpkg@pintest", state, sizeof(state), 200) != 0 ||
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
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"pintest-old\",\"image\":\"pintest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/usr/bin/pinpkg\"]}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST pintest-old, status=%d\n", r.status);
			ok = 0;
			goto skip_pin_isolation;
		}
		cix_response_free(&r);
		usleep(500000);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/containers/pintest-old", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "image_version"), v1_version)) {
			fprintf(stderr,
			        "FAIL: pintest-old should be pinned to %s, image_version=%s\n", v1_version,
			        json_str_field(r.json, "image_version") ? json_str_field(r.json, "image_version")
			                                                 : "(null)");
			ok = 0;
		}
		cix_response_free(&r);

		/* Now upgrade pintest to 2.0 -- produces a NEW immutable
		 * current_version; pintest-old's own overlay lowerdir must stay
		 * exactly what it was pinned to above. */
		if (stage_fixture_tarball(scratch_dir, "pinpkg", "2.0", tarball2, sizeof(tarball2), sha2,
		                           sizeof(sha2)) != 0 ||
		    write_recipe(&client, "pinpkg", "2.0", tarball2, sha2, NULL) != 0) {
			fprintf(stderr, "FAIL: could not stage/write pinpkg 2.0\n");
			ok = 0;
			goto skip_pin_isolation;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"pinpkg\",\"image\":\"pintest\",\"upgrade\":true}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST upgrade pinpkg@pintest, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client, "pinpkg@pintest", state, sizeof(state), 200) != 0 ||
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
		if (cix_client_request(&client, "GET", "/v1/containers/pintest-old", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "image_version"), v1_version)) {
			fprintf(stderr,
			        "FAIL: pintest-old's image_version changed after pintest's upgrade "
			        "(was %s, now %s) -- pinning broke\n",
			        v1_version,
			        json_str_field(r.json, "image_version") ? json_str_field(r.json, "image_version")
			                                                 : "(null)");
			ok = 0;
		}
		cix_response_free(&r);

		/* The real proof: pintest-old's own /usr/bin/pinpkg must still
		 * be the OLD binary (embeds "hello from pinpkg v1.0" in its own
		 * rodata, see stage_fixture_tarball()) -- read via GET
		 * .../files, which for an exited container falls back to
		 * e->image_version's own immutable rootfs
		 * (handle_container_file_read(), ADR-0107/0108), never
		 * pintest's current (now 2.0) rootfs. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET",
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
		cix_response_free(&r);

		/* A FRESH container created now, against the very same image
		 * name, must pin to the NEW version and see the NEW binary --
		 * completing the two-sided proof. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/containers",
		                       "{\"name\":\"pintest-new\",\"image\":\"pintest\","
		                       "\"services\":[{\"name\":\"main\",\"on_exit\":\"fail-container\",\"cmd\":[\"/usr/bin/pinpkg\"]}]}",
		                       &r) != 0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST pintest-new, status=%d\n", r.status);
			ok = 0;
			goto skip_pin_isolation;
		}
		cix_response_free(&r);
		usleep(500000);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/containers/pintest-new", NULL, &r) != 0 ||
		    r.status != 200 || !str_eq(json_str_field(r.json, "image_version"), v2_version)) {
			fprintf(stderr, "FAIL: pintest-new should be pinned to %s, image_version=%s\n",
			        v2_version,
			        json_str_field(r.json, "image_version") ? json_str_field(r.json, "image_version")
			                                                 : "(null)");
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET",
		                       "/v1/containers/pintest-new/files?path=%2Fusr%2Fbin%2Fpinpkg", NULL,
		                       &r) != 0 ||
		    r.status != 200 || r.body == NULL ||
		    memmem(r.body, r.body_len, "hello from pinpkg v2.0", strlen("hello from pinpkg v2.0")) ==
		        NULL) {
			fprintf(stderr, "FAIL: pintest-new's /usr/bin/pinpkg does not reflect v2.0\n");
			ok = 0;
		}
		cix_response_free(&r);
	}
skip_pin_isolation:

	/* 17. hostbuild (ADR-0056): a second mode of the same pipeline that
	 * harvests pkg_install()'s output into ARTIFACTS_DIR/<name>/ instead
	 * of merging it into any image's rootfs, using a named image's own
	 * rootfs as the build container's lowerdir instead of the shared
	 * toolchain sandbox. Proves: a real artifact lands on disk at the
	 * documented path; PKG_ERR_BUSY is enforced against hostbuild the
	 * same way it already is between two ordinary installs (step 4);
	 * a hostbuild recipe with a non-empty pkg_depends is accepted and
	 * carries the declaration onto its entry without resolving it
	 * (#465 -- resolving means "install the closure into an image",
	 * and a hostbuild has no image to merge into). Not a kernel build
	 * (far too slow for this suite) -- the same trivial
	 * gcc-a-hello-world fixture every other step here already uses,
	 * just routed through the hostbuild entry point instead of an
	 * ordinary install.
	 */
	{
		char hb_recipe_path[PATH_MAX];
		char hb_artifact_file[PATH_MAX];
		char hb_state[32];
		FILE *f;
		int i;
		struct stat st;

		/*
		 * No build image is staged: ADR-0304 (#482) retired the field,
		 * and a hostbuild now composes its build container from the
		 * recipe's own pkg_build_depends like every other build. The
		 * fixture recipes below declare the ADR-0209 test floor's own
		 * seeded set (tcc/linux-headers/bash/coreutils), which is what
		 * makes them composable here.
		 *
		 * What used to be here: a hand-written manifest.json plus a
		 * versioned rootfs for an "hbimage", because a hostbuild's
		 * lowerdir resolved through image_current_version()/
		 * image_version_rootfs_path() and staging one through a real
		 * `pkg install` would have been far too slow for this suite.
		 * That whole mechanism is gone.
		 */

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
		/*
		 * This build sleeps for the same reason slowhold's and
		 * hbconcurrent's do -- see write_recipe(), which explains the
		 * race in full.
		 *
		 * That fix was applied to the packages occupying the SECOND
		 * chain slot and stopped there. The hostbuild ceiling check
		 * needs BOTH slots provably busy, and hbtest is the occupant
		 * of the first one. Its build was `tcc -o hello hello.c`,
		 * milliseconds, while the probe that follows first waits up
		 * to five seconds for hbconcurrent to be seen holding the
		 * other slot. hbtest routinely finished inside that wait, so
		 * by the time the third request landed a slot really was
		 * free and 202 was the CORRECT answer -- the test failed
		 * while the daemon was right, roughly half of all runs.
		 *
		 * Confirmed pre-existing rather than assumed: reproduced at
		 * the commit before POST /v1/pkg/cancel was added (2 of 4
		 * runs) as well as at HEAD (2 of 3), which is what ruled that
		 * change out as the cause.
		 *
		 * The precondition guard below detects a lost window and says
		 * so, which is worth keeping, but detecting a lost
		 * precondition is not the same as not losing it -- the same
		 * sentence write_recipe() already had to write once.
		 *
		 * coreutils is in this recipe's own declared build tools, so
		 * `sleep` is genuinely present rather than assumed.
		 */
		fprintf(f, "pkg_name=hbtest\npkg_version=1.0\npkg_source=%s\n"
		           "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
		           "pkg_build() {\n\tsleep 5\n\ttcc -o hello hello.c\n}\n\n"
		           "pkg_install() {\n\tcp hello \"$PKG_DESTDIR/hello\"\n}\n",
		        test_http_src(tarball_path), sha256);
		fclose(f);

		/* start it -> 202, fetching */
		/*
		 * Issue #200: configure artifact publishing BEFORE the build,
		 * because the tarball a hostbuild must leave behind is built at
		 * build completion.
		 *
		 * It is only built when publishing is configured --
		 * pkg_artifact_publish_resolve() refuses outright with no
		 * base_url, and tarring a whole installed tree with nowhere to
		 * send it would be waste. So what is under test is "when
		 * publishing is configured, a hostbuild produces something
		 * publishable", and the configuration is part of the test
		 * rather than an assumption about the daemon's defaults.
		 *
		 * The URL is deliberately unreachable: the upload is not what
		 * broke and is not what this asserts. The push fails against
		 * it, harmlessly, after the tarball exists.
		 */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/pkg/artifact-config",
		                       "{\"base_url\":\"http://127.0.0.1:9/artifacts\","
		                       "\"push_enabled\":true}",
		                       &r) != 0 ||
		    (r.status != 200 && r.status != 204)) {
			fprintf(stderr, "FAIL: #200 could not configure artifact push, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/hostbuild",
		                       "{\"name\":\"hbtest\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST hostbuild hbtest, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* ADR-0157 Phase 2: while it's in flight, a concurrent
		 * *ordinary* install now genuinely fits in the second chain
		 * slot -> 202, mirroring step 4's own top-level proof in the
		 * other direction (a hostbuild and an ordinary install sharing
		 * the two chain slots, not two ordinary installs). A fresh,
		 * never-yet-installed name ("hbconcurrent") -- unlike badsum,
		 * this one has a real, valid tarball/checksum, so it takes a
		 * real build to run rather than failing fast on a checksum
		 * mismatch during the fetch step alone; the overflow check
		 * right below needs both chains to still be provably busy by
		 * the time it fires, which a same-tick fetch failure could
		 * otherwise race past (confirmed live: badsum's own checksum
		 * failure was fast enough to free its chain slot before the
		 * overflow request even landed).
		 *
		 * A valid tarball is necessary but was NOT sufficient, and
		 * this comment used to claim it was ("takes a genuine gcc
		 * build to finish"). These fixture recipes compile one
		 * five-line hello.c with tcc; that is fast enough to lose the
		 * race outright about one run in three. hbconcurrent now
		 * holds its slot deliberately -- see write_recipe(). */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"hbconcurrent\"}", &r) !=
		        0 ||
		    r.status != 202 || !str_eq(json_str_field(r.json, "state"), "fetching")) {
			fprintf(stderr,
			        "FAIL: ordinary install alongside hostbuild (2nd chain) status=%d, state=%s\n",
			        r.status, json_str_field(r.json, "state") ? json_str_field(r.json, "state") : "(null)");
			ok = 0;
		}
		cix_response_free(&r);

		/*
		 * Wait for the precondition rather than hoping for it -- the
		 * same guard #192 added to the ordinary-install ceiling check
		 * above, which was never applied here. hbconcurrent holding
		 * its slot is what makes the 409 below mean "the ceiling is
		 * enforced" instead of "the box happened to be slow". When it
		 * is lost, this says so in those words, rather than letting a
		 * correct 202 be reported as a ceiling fault.
		 */
		{
			char hst[64];
			int hheld = 0;
			int hw;

			for (hw = 0; hw < 100; hw++) {
				hst[0] = '\0';
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/hbconcurrent", NULL, &r) == 0 &&
				    r.status == 200) {
					const char *st = json_str_field(r.json, "state");

					if (st != NULL)
						snprintf(hst, sizeof(hst), "%s", st);
				}
				cix_response_free(&r);
				if (strcmp(hst, "fetching") == 0 || strcmp(hst, "building") == 0) {
					hheld = 1;
					break;
				}
				if (strcmp(hst, "installed") == 0 || strcmp(hst, "failed") == 0)
					break; /* terminal -- the window is gone */
				usleep(50000);
			}
			if (!hheld) {
				fprintf(stderr,
				        "FAIL: hostbuild ceiling precondition lost -- hbconcurrent reached "
				        "'%s' before the ceiling could be probed, so the 409 below would be "
				        "asserting timing, not the ceiling\n",
				        hst[0] != '\0' ? hst : "(unknown)");
				ok = 0;
			}
		}

		/*
		 * The same check for the OTHER occupant. hbconcurrent being
		 * seen in flight proves one slot is held; the 409 below is
		 * only about the ceiling if the hostbuild still holds the
		 * other one. Asserting that here means a future regression
		 * reports the precondition it actually lost, instead of
		 * reporting a ceiling fault that never happened.
		 */
		{
			char bst[64];

			bst[0] = '\0';
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/hbtest", NULL, &r) == 0 &&
			    r.status == 200) {
				const char *st = json_str_field(r.json, "state");

				if (st != NULL)
					snprintf(bst, sizeof(bst), "%s", st);
			}
			cix_response_free(&r);
			if (strcmp(bst, "fetching") != 0 && strcmp(bst, "building") != 0) {
				fprintf(stderr,
				        "FAIL: hostbuild ceiling precondition lost -- hbtest reached '%s' "
				        "before the ceiling could be probed, so the 409 below would be "
				        "asserting timing, not the ceiling\n",
				        bst[0] != '\0' ? bst : "(unknown)");
				ok = 0;
			}
		}

		/* a THIRD job attempt now that both chain slots are genuinely
		 * occupied (hbtest's hostbuild + hbconcurrent's fetch/build)
		 * -> 409 -- "overflow" is the same never-yet-installed recipe
		 * step 4b already proved gets rejected the same way at the
		 * top level. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"overflow\"}", &r) !=
		        0 ||
		    r.status != 409) {
			fprintf(stderr,
			        "FAIL: overflow install with both chains busy (hostbuild case) expected 409, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
			if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/hbtest", NULL, &r) != 0 ||
			    r.status != 200) {
				cix_response_free(&r);
				break;
			}
			state = json_str_field(r.json, "state");
			if (state == NULL) {
				cix_response_free(&r);
				break;
			}
			snprintf(hb_state, sizeof(hb_state), "%s", state);
			cix_response_free(&r);
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

		/*
		 * Issue #200: a hostbuild must end up with a real tarball in
		 * the local cache, because that is the only thing the push
		 * worker can upload.
		 *
		 * A hostbuild's artifact is a DIRECTORY this host assembled,
		 * and the post-build path used to enqueue a push without ever
		 * building a tarball from it -- so the pusher looked in the
		 * cache, found nothing, and skipped, every single time. The
		 * artifact cache's newest `cix` sat at v2.2.0-rc30 while the
		 * box that built it ran rc35, and `kernel` and `isotools` were
		 * in the same state. Ordinary packages were unaffected only
		 * because their branch saves to the cache before enqueuing.
		 *
		 * Asserting the tarball rather than the upload deliberately:
		 * there is no artifact server here, and the upload is not what
		 * broke. The missing tarball is.
		 *
		 * Polled, because the tarball is produced by an asynchronous
		 * export that starts when the build completes -- so the state
		 * reaching "installed" above does not mean it exists yet.
		 */
		{
			char hb_tarball[PATH_MAX];
			struct stat hb_st;
			int hb_i;

			snprintf(hb_tarball, sizeof(hb_tarball), "%s/cache/hbtest-1.0.tar.gz",
			         g_pkg_state_dir);
			for (hb_i = 0; hb_i < 200; hb_i++) {
				if (stat(hb_tarball, &hb_st) == 0 && hb_st.st_size > 0)
					break;
				usleep(100000);
			}
			if (stat(hb_tarball, &hb_st) != 0 || hb_st.st_size <= 0) {
				fprintf(stderr,
				        "FAIL: #200 a completed hostbuild left no tarball at %s, so nothing "
				        "could ever be published from it -- this is how cix stayed five "
				        "releases behind in the artifact cache\n",
				        hb_tarball);
				ok = 0;
			}

			/*
			 * Issue #171: and the package view must SAY so. The gap
			 * #200 fixed hid for five releases because nothing in
			 * GET /v1/pkg/<name> distinguished "published" from
			 * "exists only on this disk" -- it was found by comparing
			 * two systems by hand. artifact_cached answers the half a
			 * host can know for free.
			 */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/hbtest", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: #171 could not read back hbtest, status=%d\n", r.status);
				ok = 0;
			} else if (!json_bool_field(r.json, "artifact_cached")) {
				fprintf(stderr,
				        "FAIL: #171 hbtest's artifact is in the local cache but the package "
				        "view does not report artifact_cached -- the field exists so this "
				        "state stops being invisible\n");
				ok = 0;
			}
			cix_response_free(&r);
		}

		/* hbconcurrent's own chain (started alongside hbtest's
		 * hostbuild above) must also have finished by now -- drained
		 * here, before the hbdepstest busy-check further below, which
		 * depends on BOTH chain slots genuinely being free again. */
		if (poll_pkg_state(&client, "hbconcurrent", state, sizeof(state), 200) != 0) {
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
		if (cix_client_request(&client, "DELETE", "/v1/pkg/hbconcurrent", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE hbconcurrent (2nd chain) expected 204, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* the response itself must say is_hostbuild=true and give the
		 * real artifact_path -- not just that the job finished. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/hbtest", NULL, &r) != 0 ||
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
		cix_response_free(&r);

		/*
		 * Rebuilding must not grow the file list (#185). A hostbuild
		 * MERGES into its artifact directory rather than replacing it,
		 * and this branch was appending to e->files each round without
		 * ever forgetting the previous one -- so the real `cix`
		 * package reported its twelve files five times over, once per
		 * rebuild since the entry was created, and would have kept
		 * growing. The ordinary install path has always reset the list
		 * first; this one was added later, for reporting alone, and
		 * never picked that up.
		 *
		 * Asserts the count is UNCHANGED, not merely non-zero: the
		 * broken version reported a perfectly non-empty list too.
		 */
		{
			int files_before = -1, files_after = -1;
			const struct json_value *fv;

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/hbtest", NULL, &r) == 0 &&
			    r.status == 200) {
				fv = json_object_get(r.json, "files");
				if (fv != NULL && fv->type == JSON_ARRAY)
					files_before = (int)fv->u.array.count;
			}
			cix_response_free(&r);

			/*
			 * A NEW version, because rebuilding the same one is
			 * refused outright (PKG_ERR_DUPLICATE) -- which is
			 * exactly the shape the real growth took: `cix` went
			 * rc24, rc25, rc26, rc27, rc28 through one entry, and
			 * the list grew by twelve at each step.
			 */
			{
				char v11_dir[PATH_MAX];
				char v11_path[PATH_MAX];
				FILE *vf;
				int started = 0;

				snprintf(v11_dir, sizeof(v11_dir), "%s/recipes/hbtest/1.1",
				         g_pkg_state_dir);
				mkdir(v11_dir, 0755);
				snprintf(v11_path, sizeof(v11_path), "%s/build.sh", v11_dir);
				vf = fopen(v11_path, "w");
				if (vf == NULL) {
					fprintf(stderr, "FAIL: could not write hbtest 1.1 recipe\n");
					ok = 0;
				} else {
					fprintf(vf,
					        "pkg_name=hbtest\npkg_version=1.1\npkg_source=%s\n"
					        "pkg_sha256=%s\npkg_depends=\"\"\n"
					        "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
					        "pkg_build() {\n\ttcc -o hello hello.c\n}\n\n"
					        "pkg_install() {\n\tcp hello \"$PKG_DESTDIR/hello\"\n}\n",
					        test_http_src(tarball_path), sha256);
					fclose(vf);
				}

				/* 409 can also mean an earlier job is still
				 * draining -- retry rather than race it. */
				for (i = 0; ok && i < 100; i++) {
					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "POST", "/v1/pkg/hostbuild",
					                        "{\"name\":\"hbtest\","
					                        "\"version\":\"1.1\",\"upgrade\":true}",
					                        &r) == 0 &&
					    r.status == 202) {
						cix_response_free(&r);
						started = 1;
						break;
					}
					cix_response_free(&r);
					usleep(300000);
				}
				if (ok && !started) {
					fprintf(stderr, "FAIL: hbtest 1.1 hostbuild never started\n");
					ok = 0;
				}
			}

			hb_state[0] = '\0';
			for (i = 0; i < 200; i++) {
				const char *st2;

				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/hbtest", NULL,
				                        &r) != 0 ||
				    r.status != 200) {
					cix_response_free(&r);
					break;
				}
				st2 = json_str_field(r.json, "state");
				if (st2 != NULL)
					snprintf(hb_state, sizeof(hb_state), "%s", st2);
				fv = json_object_get(r.json, "files");
				if (fv != NULL && fv->type == JSON_ARRAY)
					files_after = (int)fv->u.array.count;
				cix_response_free(&r);
				if (st2 == NULL ||
				    (strcmp(hb_state, "fetching") != 0 && strcmp(hb_state, "building") != 0))
					break;
				usleep(300000);
			}
			if (strcmp(hb_state, "installed") != 0) {
				fprintf(stderr, "FAIL: hbtest second hostbuild ended in '%s'\n", hb_state);
				ok = 0;
			} else if (files_before <= 0 || files_after != files_before) {
				fprintf(stderr,
				        "FAIL: rebuilding a hostbuild grew its file list: %d -> %d\n",
				        files_before, files_after);
				ok = 0;
			}
		}

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

		/*
		 * Issue #165: uninstalling a hostbuild must take its bytes
		 * with it.
		 *
		 * Deleting the package used to clear the record and leave the
		 * whole artifact directory on disk. That is not untidy, it is
		 * dangerous: these paths are consumed BY PATH, not by package
		 * -- an ISO build reads <artifacts>/kernel/bzImage directly --
		 * so an uninstalled hostbuild stayed fully deployable and
		 * bootable while GET /v1/pkg showed nothing to explain where
		 * the bytes came from.
		 *
		 * Asserted on the FILE, not on the API's own answer. The
		 * record disappearing was never the bug; the record
		 * disappearing while the bytes stayed was.
		 */
		{
			char hb_dir[PATH_MAX];
			char *slash;

			snprintf(hb_dir, sizeof(hb_dir), "%s", hb_artifact_file);
			slash = strrchr(hb_dir, '/');
			if (slash != NULL)
				*slash = '\0';

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "DELETE", "/v1/pkg/hbtest@__hostbuild", NULL, &r) != 0 ||
			    (r.status != 200 && r.status != 204)) {
				fprintf(stderr, "FAIL: DELETE hbtest@__hostbuild, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

			if (stat(hb_artifact_file, &st) == 0) {
				fprintf(stderr,
				        "FAIL: uninstalled hostbuild left its artifact on disk at '%s' -- still "
				        "deployable with no package to explain it (#165)\n",
				        hb_artifact_file);
				ok = 0;
			}
			if (stat(hb_dir, &st) == 0) {
				fprintf(stderr,
				        "FAIL: uninstalled hostbuild left its artifact directory '%s' behind "
				        "(#165)\n",
				        hb_dir);
				ok = 0;
			}
			/* And the record really is gone -- so this proves the two
			 * agree, rather than only that one of them changed. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/hbtest", NULL, &r) == 0 &&
			    r.status == 200) {
				fprintf(stderr, "FAIL: hbtest still present after DELETE\n");
				ok = 0;
			}
			cix_response_free(&r);
		}

		/*
		 * A hostbuild recipe declaring a non-empty pkg_depends is
		 * ACCEPTED, and the declaration is carried onto the entry
		 * without being resolved (#465). This step asserted a 400
		 * until then, which is what made cix's own recipe
		 * unsatisfiable in both directions at once.
		 *
		 * The dependency named here is deliberately a name no recipe
		 * in this fixture set has: anything on the hostbuild path
		 * that tried to RESOLVE it could only fail the job ("no such
		 * recipe"). So reaching `installed` proves it was not
		 * resolved, the recorded depends field proves it was not
		 * silently discarded either, and a 404 for the name itself
		 * proves nothing installed it -- one fixture covering all three
		 * parts of "carried, never resolved, never installed".
		 *
		 * The prior job is done by now (not busy), so this genuinely
		 * exercises the depends path rather than PKG_ERR_BUSY.
		 */
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
		fprintf(f, "pkg_name=hbdepstest\npkg_version=1.0\npkg_source=%s\n"
		           "pkg_sha256=%s\npkg_depends=\"nosuchdep\"\n"
		           "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
		           "pkg_build() {\n\ttcc -o hello hello.c\n}\n\n"
		           "pkg_install() {\n\tcp hello \"$PKG_DESTDIR/hello\"\n}\n",
		        test_http_src(tarball_path), sha256);
		fclose(f);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/hostbuild",
		                       "{\"name\":\"hbdepstest\"}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr,
			        "FAIL: hostbuild with non-empty pkg_depends expected 202, got %d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		hb_state[0] = '\0';
		for (i = 0; i < 100; i++) {
			const char *state;

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/hbdepstest", NULL, &r) != 0 ||
			    r.status != 200) {
				cix_response_free(&r);
				break;
			}
			state = json_str_field(r.json, "state");
			if (state == NULL) {
				cix_response_free(&r);
				break;
			}
			snprintf(hb_state, sizeof(hb_state), "%s", state);
			cix_response_free(&r);
			if (strcmp(hb_state, "fetching") != 0 && strcmp(hb_state, "building") != 0)
				break;
			usleep(300000);
		}
		if (strcmp(hb_state, "installed") != 0) {
			fprintf(stderr,
			        "FAIL: hbdepstest ended in state '%s', expected installed -- a declared "
			        "pkg_depends must not be resolved on the hostbuild path\n",
			        hb_state);
			ok = 0;
		} else {
			const char *dep;

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/hostbuild/hbdepstest", NULL, &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: could not re-read hbdepstest, status=%d\n", r.status);
				ok = 0;
			} else {
				dep = json_str_field(r.json, "depends");
				if (dep == NULL || strcmp(dep, "nosuchdep") != 0) {
					fprintf(stderr,
					        "FAIL: hbdepstest recorded depends=\"%s\", expected \"nosuchdep\" "
					        "-- the declaration is carried, not discarded\n",
					        dep != NULL ? dep : "(null)");
					ok = 0;
				}
			}
			cix_response_free(&r);

			/* and nothing installed it */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/nosuchdep", NULL, &r) == 0 &&
			    r.status == 200) {
				fprintf(stderr,
				        "FAIL: nosuchdep is installed -- a hostbuild resolved a declared "
				        "dependency it must only have recorded\n");
				ok = 0;
			}
			cix_response_free(&r);
		}

		/* There is no unknown-build_image case to assert any more:
		 * ADR-0304 (#482) retired the field, so a hostbuild composes
		 * its environment from pkg_build_depends and names no image.
		 * What used to be checked here -- a 404 for an image that does
		 * not exist -- has no request that can produce it. The
		 * equivalent failure is now a declared build tool that is
		 * installed nowhere, which the ADR-0199 composition path
		 * already refuses with the tool named. */
	}
skip_hostbuild:

	/* ADR-0157 Phase 3: PUT /v1/system/pkg-build-config validation --
	 * still at the ceiling=2 this test set right after startup. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":0}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: PUT pkg-build-config max_concurrent_jobs=0 expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":11}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: PUT pkg-build-config max_concurrent_jobs=11 expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* Lowering the ceiling to 1 and confirming chain_alloc() actually
	 * honors it (not just the compile-time PKG_MAX_CONCURRENT_JOBS
	 * array bound) is the real proof this config is load-bearing, not
	 * just a number that gets echoed back. "overflow"/"hbconcurrent"
	 * are both still fresh, never-installed recipes at this point. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":1}", &r) != 0 ||
	    r.status != 200 || json_as_number(json_object_get(r.json, "max_concurrent_jobs")) != 1) {
		fprintf(stderr, "FAIL: PUT pkg-build-config max_concurrent_jobs=1, got %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"slowhold\"}", &r) != 0 ||
	    r.status != 202) {
		fprintf(stderr, "FAIL: POST install slowhold (ceiling=1) status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * The 409 asserted below is only true while THIS job still holds
	 * the single slot, and nothing here made that true -- it was left
	 * to whether the box was slow enough (#192). It failed roughly one
	 * run in eight, as "expected 409, got 202" plus a downstream wake,
	 * and neither message named timing, so it read like a real fault in
	 * the ceiling logic rather than a test that got unlucky.
	 *
	 * So wait for the precondition instead of hoping for it, and fail
	 * with a message that says which thing went wrong if it is lost.
	 */
	{
		char ost[64];
		int held = 0;
		int w;

		for (w = 0; w < 100; w++) {
			ost[0] = '\0';
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/slowhold", NULL, &r) == 0 &&
			    r.status == 200) {
				const char *st = json_str_field(r.json, "state");

				if (st != NULL)
					snprintf(ost, sizeof(ost), "%s", st);
			}
			cix_response_free(&r);
			if (strcmp(ost, "fetching") == 0 || strcmp(ost, "building") == 0) {
				held = 1;
				break;
			}
			if (ost[0] != '\0' && strcmp(ost, "installed") != 0 && strcmp(ost, "failed") != 0) {
				usleep(50000);
				continue;
			}
			if (strcmp(ost, "installed") == 0 || strcmp(ost, "failed") == 0)
				break; /* already terminal -- the window is gone */
			usleep(50000);
		}
		if (!held) {
			fprintf(stderr,
			        "FAIL: #192 test precondition lost -- slowhold reached '%s' before the "
			        "build ceiling could be probed, so the 409 below would be asserting "
			        "timing, not the ceiling\n",
			        ost[0] != '\0' ? ost : "no state");
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"hbconcurrent\"}", &r) !=
	        0 ||
	    r.status != 409) {
		fprintf(stderr,
		        "FAIL: POST install hbconcurrent while ceiling=1 already busy expected 409, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	if (poll_pkg_state(&client, "slowhold", state, sizeof(state), 200) != 0 ||
	    strcmp(state, "installed") != 0) {
		fprintf(stderr, "FAIL: slowhold (ceiling=1) ended in state '%s', expected installed\n",
		        state);
		ok = 0;
	}

	/* Restored to the real default before the second install below --
	 * proves raising the ceiling back up re-admits genuine concurrency
	 * immediately, not just that lowering it worked. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config",
	                       "{\"max_concurrent_jobs\":10}", &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: PUT pkg-build-config restore max_concurrent_jobs=10, got %d\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/*
	 * Let the ceiling=1 job finish before asking for another (#192).
	 *
	 * The wait added above makes "slowhold" genuinely still hold the
	 * slot when the 409 is asserted -- which is the point -- so it is
	 * still holding it here too. Without draining it first, this step
	 * asserts a 202 while the pipeline is legitimately busy and gets a
	 * 409, which is the first race's echo rather than a fault in
	 * raising the ceiling. Same defect as the one being fixed, one step
	 * later: a timing assumption left implicit.
	 */
	{
		char dst[64];

		dst[0] = '\0';
		if (poll_pkg_state(&client, "slowhold", dst, sizeof(dst), 300) != 0) {
			fprintf(stderr,
			        "FAIL: #192 slowhold never reached a terminal state (last='%s'), so the "
			        "ceiling-restore check below would be asserting timing\n",
			        dst[0] != '\0' ? dst : "no state");
			ok = 0;
		}
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"hbconcurrent\"}", &r) !=
	        0 ||
	    r.status != 202) {
		fprintf(stderr,
		        "FAIL: POST install hbconcurrent after restoring ceiling=10 status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	if (poll_pkg_state(&client, "hbconcurrent", state, sizeof(state), 200) != 0 ||
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
		fprintf(f, "pkg_name=keepfail\npkg_version=1.0\npkg_source=%s\n"
		           "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
		           "pkg_build() {\n\texit 1\n}\n\n"
		           "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n}\n",
		        test_http_src(tarball_path), sha256);
		fclose(f);

		/* 18a. WITHOUT keep_on_failure: unchanged pre-existing
		 * behavior -- the failed build container is gone immediately,
		 * kept_build_container stays null. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"keepfail\"}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install keepfail (no keep_on_failure) status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client, "keepfail", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "failed") != 0) {
			fprintf(stderr, "FAIL: keepfail (no keep_on_failure) ended in state '%s'\n", state);
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/keepfail", NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET pkg/keepfail (no keep_on_failure) status=%d\n", r.status);
			ok = 0;
		} else if (json_object_get(r.json, "kept_build_container")->type != JSON_NULL) {
			fprintf(stderr,
			        "FAIL: keepfail (no keep_on_failure) has a non-null kept_build_container\n");
			ok = 0;
		}
		cix_response_free(&r);

		/* 18b. WITH keep_on_failure: the build container survives,
		 * readable, until an explicit DELETE. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"keepfail\",\"upgrade\":true,\"keep_on_failure\":true}",
		                       &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install keepfail (keep_on_failure) status=%d\n", r.status);
			ok = 0;
			goto skip_keep_on_failure;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client, "keepfail", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "failed") != 0) {
			fprintf(stderr, "FAIL: keepfail (keep_on_failure) ended in state '%s'\n", state);
			ok = 0;
			goto skip_keep_on_failure;
		}

		memset(&r, 0, sizeof(r));
		kept_name[0] = '\0';
		if (cix_client_request(&client, "GET", "/v1/pkg/keepfail", NULL, &r) != 0 || r.status != 200) {
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
		cix_response_free(&r);

		if (kept_name[0] == '\0')
			goto skip_keep_on_failure;

		/* the preserved container is a completely ordinary, addressable
		 * exited container -- GET still finds it... */
		snprintf(kf_container_path, sizeof(kf_container_path), "/v1/containers/%s", kept_name);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 200) {
			fprintf(stderr, "FAIL: GET %s (preserved build container) status=%d\n",
			        kf_container_path, r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* ...and ADR-0055's GET .../files reads real content back out
		 * of it -- proof the overlay genuinely survived, not just the
		 * registry entry (the actual point of this whole mechanism:
		 * pulling a failed build's own output/state back out for real
		 * debugging). */
		snprintf(kf_container_path, sizeof(kf_container_path),
		         "/v1/containers/%s/files?path=%%2Fbuild%%2Fsrc%%2Fhello.c", kept_name);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 200 ||
		    r.body == NULL || r.body_len == 0) {
			fprintf(stderr, "FAIL: GET %s status=%d body_len=%zu\n", kf_container_path, r.status,
			        r.body_len);
			ok = 0;
		}
		cix_response_free(&r);

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
		if (cix_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 200 ||
		    r.body == NULL || r.body_len == 0) {
			fprintf(stderr,
			        "FAIL: GET %s (build container lowerdir read) status=%d body_len=%zu\n",
			        kf_container_path, r.status, r.body_len);
			ok = 0;
		}
		cix_response_free(&r);

		/*
		 * Negative (the host-leak half): a marker file this test writes
		 * into its own scratch directory exists on the daemon's host and
		 * in no image, so it must 404 -- a 200 here means the endpoint
		 * served a host file through a per-container path.
		 *
		 * This used /etc/os-release, on the premise that it exists on the
		 * host but not in the build tree. The daemon now writes a Cix
		 * os-release into every image it seeds (pkg_seed_image_baseline(),
		 * ADR-0274), so that path answered 200 from the container's own
		 * tree (cix-tests on 192.168.15.95, 2026-09-23) and could no
		 * longer tell a leak from a correct read.
		 */
		{
			char marker[600], enc[1800];
			const char *p;
			size_t o = 0;
			FILE *mf;

			snprintf(marker, sizeof(marker), "%s/host-only-marker", scratch_dir);
			mf = fopen(marker, "w");
			if (mf != NULL) {
				fputs("host only\n", mf);
				fclose(mf);
			}
			for (p = marker; *p != '\0' && o + 4 < sizeof(enc); p++) {
				if (*p == '/') {
					memcpy(enc + o, "%2F", 3);
					o += 3;
				} else {
					enc[o++] = *p;
				}
			}
			enc[o] = '\0';
			snprintf(kf_container_path, sizeof(kf_container_path),
			         "/v1/containers/%s/files?path=%s", kept_name, enc);
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr,
			        "FAIL: GET %s expected 404 -- a host file must never be readable through a "
			        "container's own files endpoint, status=%d\n",
			        kf_container_path, r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* explicit cleanup -- the completely ordinary DELETE path,
		 * no new mechanism. */
		snprintf(kf_container_path, sizeof(kf_container_path), "/v1/containers/%s", kept_name);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", kf_container_path, NULL, &r) != 0 || r.status != 204) {
			fprintf(stderr, "FAIL: DELETE %s (preserved build container) status=%d\n",
			        kf_container_path, r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", kf_container_path, NULL, &r) != 0 || r.status != 404) {
			fprintf(stderr, "FAIL: GET %s after DELETE expected 404, got %d\n", kf_container_path,
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
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
		fprintf(f, "pkg_name=resumeme\npkg_version=1.0\npkg_source=%s\n"
		           "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
		           "pkg_build() {\n\ttouch /build/src/.resumed_marker\n\texit 1\n}\n\n"
		           "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n}\n",
		        test_http_src(tarball_path), sha256);
		fclose(f);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"resumeme\",\"keep_on_failure\":true}", &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install resumeme (v1.0) status=%d\n", r.status);
			ok = 0;
			goto skip_resume;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client, "resumeme", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "failed") != 0) {
			fprintf(stderr, "FAIL: resumeme (v1.0) ended in state '%s'\n", state);
			ok = 0;
			goto skip_resume;
		}

		memset(&r, 0, sizeof(r));
		kept_name[0] = '\0';
		if (cix_client_request(&client, "GET", "/v1/pkg/resumeme", NULL, &r) != 0 ||
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
		cix_response_free(&r);

		if (kept_name[0] == '\0')
			goto skip_resume;

		/* the marker really is there -- proof the container's own
		 * overlay genuinely has real build state, not just an empty
		 * preserved shell. */
		snprintf(rs_container_path, sizeof(rs_container_path),
		         "/v1/containers/%s/files?path=%%2Fbuild%%2Fsrc%%2F.resumed_marker", kept_name);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", rs_container_path, NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET %s (marker before resume) status=%d\n", rs_container_path,
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

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
		fprintf(f, "pkg_name=resumeme\npkg_version=1.1\npkg_source=%s\n"
		           "pkg_sha256=%s\npkg_depends=\"\"\npkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
		           "pkg_build() {\n\t[ -f /build/src/.resumed_marker ] || exit 1\n}\n\n"
		           "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/share/resumeme\"\n"
		           "\techo resumed > \"$PKG_DESTDIR/usr/share/resumeme/stamp\"\n}\n",
		        test_http_src(tarball_path), sha256);
		fclose(f);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/resume", "{\"name\":\"resumeme\"}", &r) !=
		        0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST resume resumeme status=%d body=%s\n", r.status,
			        r.body != NULL ? r.body : "(null)");
			ok = 0;
			goto skip_resume;
		}
		cix_response_free(&r);

		if (poll_pkg_state(&client, "resumeme", state, sizeof(state), 200) != 0 ||
		    strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: resumeme (resumed) ended in state '%s'\n", state);
			ok = 0;
			goto skip_resume;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/resumeme", NULL, &r) != 0 ||
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
		cix_response_free(&r);

		/* the exact same registry slot/container name was reused, not a
		 * fresh one -- and a genuinely successful build (resumed or
		 * not) always gets torn down automatically afterward, same as
		 * any other pkgbuild container's clean exit. */
		snprintf(rs_container_path, sizeof(rs_container_path), "/v1/containers/%s", kept_name);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", rs_container_path, NULL, &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: GET %s (resumed container after success) expected 404, got %d\n",
			        rs_container_path, r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}
skip_resume:

	/* cleanup */
	run_cmd("rm -rf '%s'", scratch_dir);

	/*
	 * A build that goes quiet must be REPORTED, with what its processes
	 * are blocked on. Nothing acted on last_output_seconds_ago before
	 * this: a stalled build sat in "building" looking exactly like a
	 * working one, and the only thing that ever noticed was a person
	 * wondering why the machine was idle.
	 */
	{
		int saw_stall = 0;
		int attempt;
		char stall_tarball[600];
		char stall_sha[80];

		/* Stage this block's own source rather than reusing the one
		 * from the top of the test: by the time this runs, the cache
		 * and scratch tests have been and gone, and that tarball is no
		 * longer on disk. A fixture that quietly disappears makes this
		 * look like a detector that does not fire. */
		if (stage_fixture_tarball(scratch_dir, "stallsrc", "1.0", stall_tarball,
		                          sizeof(stall_tarball), stall_sha, sizeof(stall_sha)) != 0) {
			fprintf(stderr, "FAIL: could not stage the stall fixture tarball\n");
			ok = 0;
		}
		if (write_stalling_recipe(&client, "stallpkg", "1.0", stall_tarball, stall_sha) != 0) {
			fprintf(stderr, "FAIL: could not write the stalling recipe\n");
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"stallpkg\"}",
		                       &r) != 0 ||
		    r.status != 202) {
			fprintf(stderr, "FAIL: POST install stallpkg, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		for (attempt = 0; attempt < 120 && !saw_stall; attempt++) {
			usleep(500000);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET",
			                       "/v1/system/logs?source=cixd&tail=200", NULL, &r) == 0 &&
			    r.status == 200 && r.body != NULL &&
			    strstr(r.body, "no build output for") != NULL &&
			    strstr(r.body, "stallpkg") != NULL)
				saw_stall = 1;
			cix_response_free(&r);
		}
		if (!saw_stall) {
			fprintf(stderr,
			        "FAIL: a build that produced no output was never reported as stalled\n");
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/stallpkg", NULL, &r) == 0)
				fprintf(stderr, "  stallpkg: status=%d %s\n", r.status,
				        r.body != NULL ? r.body : "(no body)");
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/system/logs?source=cixd&tail=12",
			                       NULL, &r) == 0 && r.body != NULL)
				fprintf(stderr, "  last logs: %.1200s\n", r.body);
			cix_response_free(&r);
			ok = 0;
		}
	}

	/*
	 * Issue #175: removing a package must not delete files another
	 * installed package also owns.
	 *
	 * Two packages sharing a path is ordinary -- glibc and
	 * linux-headers both own parts of usr/include -- and whichever
	 * installed last is what is on disk. Deleting one used to unlink
	 * every path in its manifest regardless, so the survivor was left
	 * reported as installed, with a recorded manifest, and its files
	 * gone. Nothing failed at the time; the damage surfaced later and
	 * somewhere else entirely, as a build environment that could not
	 * be composed.
	 *
	 * The shared file must still be there after deleting the package
	 * that happens to have written it, because the other one claims it
	 * too.
	 */
	{
		const char *shared_rel = "usr/share/shared175/common.txt";
		char sha[65];
		char tarball[PATH_MAX];
		int stage_ok;

		stage_ok = stage_fixture_tarball(scratch_dir, "shareda", "1.0", tarball,
		                                 sizeof(tarball), sha, sizeof(sha)) == 0;
		if (stage_ok &&
		    (write_shared_path_recipe(&client, "shareda", "1.0", tarball, sha, shared_rel) != 0 ||
		     write_shared_path_recipe(&client, "sharedb", "1.0", tarball, sha, shared_rel) != 0)) {
			fprintf(stderr, "FAIL: could not write the shared-path recipes\n");
			ok = 0;
			stage_ok = 0;
		}
		if (stage_ok) {
			static const char *const both[] = { "shareda", "sharedb", NULL };
			int i, installed = 1;

			for (i = 0; both[i] != NULL && installed; i++) {
				char body[128], st[64];

				snprintf(body, sizeof(body), "{\"name\":\"%s\"}", both[i]);
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "POST", "/v1/pkg/install", body, &r) != 0 ||
				    (r.status != 202 && r.status != 200)) {
					fprintf(stderr, "FAIL: #175 install %s status=%d\n", both[i],
					        r.status);
					ok = 0;
					installed = 0;
				}
				cix_response_free(&r);
				if (installed &&
				    (poll_pkg_state(&client, both[i], st, sizeof(st), 240) != 0 ||
				     !str_eq(st, "installed"))) {
					fprintf(stderr, "FAIL: #175 %s did not install (state=%s)\n",
					        both[i], st);
					ok = 0;
					installed = 0;
				}
			}

			if (installed && access(base_path("/usr/share/shared175/common.txt"), F_OK) != 0) {
				fprintf(stderr, "FAIL: #175 shared file absent before the delete\n");
				ok = 0;
				installed = 0;
			}

			if (installed) {
				memset(&r, 0, sizeof(r));
				cix_client_request(&client, "DELETE", "/v1/pkg/shareda", NULL, &r);
				if (r.status != 204 && r.status != 200) {
					fprintf(stderr, "FAIL: #175 deleting shareda status=%d\n",
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);

				/* The survivor's own file, and the shared one it
				 * also claims, must both still be present. */
				if (access(base_path("/usr/bin/sharedb"), F_OK) != 0) {
					fprintf(stderr,
					        "FAIL: #175 deleting shareda removed sharedb's own file\n");
					ok = 0;
				}
				if (access(base_path("/usr/share/shared175/common.txt"), F_OK) != 0) {
					fprintf(stderr,
					        "FAIL: #175 deleting shareda deleted a path sharedb "
					        "also owns -- silent cross-package deletion\n");
					ok = 0;
				}
				/* And its own, unshared file must be gone -- the fix
				 * must not turn delete into a no-op. */
				if (access(base_path("/usr/bin/shareda"), F_OK) == 0) {
					fprintf(stderr, "FAIL: #175 shareda's own file survived the delete\n");
					ok = 0;
				}
			}
		}
	}

	/*
	 * #186: a build environment must carry the C library this platform
	 * built, and carry it INTACT.
	 *
	 * Composition copies each declared tool's files in turn, in sorted
	 * name order, so a tool sorting after "glibc" that ships a path
	 * glibc also owns silently wins it. glibc's objects share a
	 * private, version-locked interface -- an environment holding
	 * halves of two C libraries cannot exec, and would fail later with
	 * a bare ENOENT naming nothing.
	 *
	 * So this stages exactly that collision on purpose: "zzlibc" ships
	 * its own lib/x86_64-linux-gnu/libc.so.6, is declared as a build
	 * tool, and sorts last. Composition must REFUSE, and say which file
	 * was overwritten. Asserting the message, not merely the failure --
	 * a build can fail for a hundred reasons and only one of them is
	 * this one.
	 *
	 * Measured with the gate removed, which is the whole argument for
	 * having it: the build still fails, but as "build failed (exit
	 * 127)" -- the opaque broken-loader exit that names no file, no
	 * package, and no cause. That is what an operator would have had
	 * to diagnose. With the gate, composition stops and says which
	 * file is not the copy glibc installed.
	 */
	{
		char zz_dir[300];
		char zz_path[400];
		char state[64];
		char zz_scratch[] = "/tmp/cix_test_186_XXXXXX";
		char zz_tarball[512];
		char zz_sha[128];
		FILE *zf;
		int ok186 = 1;

		/* Its own scratch: the suite's shared one is removed well
		 * before this point, and a recipe still has to fetch and
		 * verify a real source like any other. */
		if (mkdtemp(zz_scratch) == NULL ||
		    stage_fixture_tarball(zz_scratch, "zzlibc", "1.0", zz_tarball, sizeof(zz_tarball),
		                           zz_sha, sizeof(zz_sha)) != 0) {
			fprintf(stderr, "FAIL: #186 could not stage a source tarball\n");
			ok = 0;
			ok186 = 0;
		}

		snprintf(zz_dir, sizeof(zz_dir), "%s/recipes/zzlibc", g_pkg_state_dir);
		mkdir(zz_dir, 0755);
		snprintf(zz_dir, sizeof(zz_dir), "%s/recipes/zzlibc/1.0", g_pkg_state_dir);
		mkdir(zz_dir, 0755);
		snprintf(zz_path, sizeof(zz_path), "%s/build.sh", zz_dir);
		zf = ok186 ? fopen(zz_path, "w") : NULL;
		if (zf == NULL && ok186) {
			fprintf(stderr, "FAIL: #186 could not write zzlibc recipe\n");
			ok = 0;
			ok186 = 0;
		} else {
			fprintf(zf,
			        "pkg_name=zzlibc\npkg_version=1.0\npkg_source=%s\n"
			        "pkg_sha256=%s\npkg_depends=\"\"\n"
			        "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
			        "pkg_build() {\n\ttcc -o hello hello.c\n}\n\n"
			        "pkg_install() {\n"
			        "\tmkdir -p \"$PKG_DESTDIR/lib/x86_64-linux-gnu\"\n"
			        "\techo not-a-real-libc > "
			        "\"$PKG_DESTDIR/lib/x86_64-linux-gnu/libc.so.6\"\n}\n",
			        test_http_src(zz_tarball), zz_sha);
			fclose(zf);
		}

		if (ok186) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install",
			                        "{\"name\":\"zzlibc\",\"image\":\"libcollide\"}", &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: #186 install zzlibc, status=%d\n", r.status);
				ok = 0;
				ok186 = 0;
			}
			cix_response_free(&r);
		}
		if (ok186 && (poll_pkg_state(&client, "zzlibc@libcollide", state, sizeof(state), 300) != 0 ||
		              strcmp(state, "installed") != 0)) {
			{
				const char *em = NULL;

				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/zzlibc@libcollide", NULL, &r) == 0 &&
				    r.status == 200)
					em = json_str_field(r.json, "error");
				fprintf(stderr, "FAIL: #186 zzlibc did not install (state=%s): %s\n", state,
				        em != NULL ? em : "(no error)");
				cix_response_free(&r);
			}
			ok = 0;
			ok186 = 0;
		}

		/* Into an image of its own, never "base": this package exists
		 * to ship a deliberately broken libc, and every later step in
		 * this suite builds against base. A declared tool is resolved
		 * from whichever image holds it, so isolating it costs
		 * nothing. */
		/* Now a package declaring it, so composition must copy zzlibc
		 * after glibc and land on the same path. */
		if (ok186) {
			snprintf(zz_dir, sizeof(zz_dir), "%s/recipes/collide", g_pkg_state_dir);
			mkdir(zz_dir, 0755);
			snprintf(zz_dir, sizeof(zz_dir), "%s/recipes/collide/1.0", g_pkg_state_dir);
			mkdir(zz_dir, 0755);
			snprintf(zz_path, sizeof(zz_path), "%s/build.sh", zz_dir);
			zf = fopen(zz_path, "w");
			if (zf == NULL) {
				fprintf(stderr, "FAIL: #186 could not write collide recipe\n");
				ok = 0;
				ok186 = 0;
			} else {
				fprintf(zf,
				        "pkg_name=collide\npkg_version=1.0\npkg_source=%s\n"
				        "pkg_sha256=%s\npkg_depends=\"\"\n"
				        "pkg_build_depends=\"tcc linux-headers bash coreutils binutils zzlibc\"\n\n"
				        "pkg_build() {\n\ttcc -o hello hello.c\n}\n\n"
				        "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n"
				        "\tcp hello \"$PKG_DESTDIR/usr/bin/collide\"\n}\n",
				        test_http_src(zz_tarball), zz_sha);
				fclose(zf);
			}
		}

		if (ok186) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install",
			                        "{\"name\":\"collide\",\"image\":\"base\"}", &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: #186 install collide, status=%d\n", r.status);
				ok = 0;
				ok186 = 0;
			}
			cix_response_free(&r);
		}
		if (ok186) {
			/*
			 * It must SUCCEED, and that is the assertion (#187).
			 *
			 * This block used to require a refusal, because a tool
			 * sorting after "glibc" genuinely won any path they both
			 * claimed and the only defence was to notice afterwards.
			 * The C library is now copied last, so a colliding
			 * package cannot win -- glibc overwrites it -- and the
			 * collision is prevented rather than detected.
			 *
			 * A successful build IS the proof: zzlibc's libc.so.6 is
			 * the text "not-a-real-libc", so if it were the one that
			 * survived, nothing in the environment could exec and the
			 * build could not finish. The gate remains as a tripwire
			 * against a future reordering, and is proven by removing
			 * the ordering rather than by a permanent failing case.
			 */
			if (poll_pkg_state(&client, "collide", state, sizeof(state), 300) != 0 ||
			    strcmp(state, "installed") != 0) {
				fprintf(stderr,
				        "FAIL: #187 a colliding libc was not overridden by glibc "
				        "(collide state=%s, expected installed)\n",
				        state);
				ok = 0;
			}
		}
	}

	/*
	 * Issue #213: an in-flight build can be cancelled, and the outcome
	 * says so.
	 *
	 * Three things are asserted, and the middle one is the point:
	 *   - cancelling a package that does not exist is 404
	 *   - cancelling one with no build in flight is 409, NOT a silent
	 *     success -- a cancel racing a build that just finished must
	 *     not be able to mark a completed install as cancelled
	 *   - cancelling a real running build stops it and records
	 *     stage "build" with status "cancelled", not a plain build failure
	 *
	 * The last matters because the container is SIGKILLed: without the
	 * cancel flag being consulted, the outcome would be reported as
	 * "killed by signal 9" -- true, useless, and identical to every
	 * other way a build can die.
	 */
	{
		char cdir[PATH_MAX], cpath[PATH_MAX];
		char c_scratch[] = "/tmp/cix_test_cancel_XXXXXX";
		char c_tarball[512], c_sha[128];
		FILE *cf;
		int okc = 1;
		int i;

		/* Its own source, staged like every other recipe's: a cancel
		 * test still has to get as far as a real build. */
		if (mkdtemp(c_scratch) == NULL ||
		    stage_fixture_tarball(c_scratch, "sleeper", "1.0", c_tarball, sizeof(c_tarball),
		                           c_sha, sizeof(c_sha)) != 0) {
			fprintf(stderr, "FAIL: #213 could not stage a source tarball\n");
			ok = 0;
			okc = 0;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/cancel",
		                        "{\"name\":\"no-such-package-at-all\"}", &r) != 0 ||
		    r.status != 404) {
			fprintf(stderr, "FAIL: #213 cancel unknown package, status=%d (want 404)\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* A build that would never finish on its own. */
		snprintf(cdir, sizeof(cdir), "%s/recipes/sleeper", g_pkg_state_dir);
		mkdir(cdir, 0755);
		snprintf(cdir, sizeof(cdir), "%s/recipes/sleeper/1.0", g_pkg_state_dir);
		mkdir(cdir, 0755);
		snprintf(cpath, sizeof(cpath), "%s/build.sh", cdir);
		cf = okc ? fopen(cpath, "w") : NULL;
		if (cf == NULL && okc) {
			fprintf(stderr, "FAIL: #213 could not write sleeper recipe\n");
			ok = 0;
			okc = 0;
		} else if (cf != NULL) {
			fprintf(cf,
			        "pkg_name=sleeper\npkg_version=1.0\npkg_source=%s\n"
			        "pkg_sha256=%s\npkg_depends=\"\"\n"
			        "pkg_build_depends=\"tcc linux-headers bash coreutils binutils\"\n\n"
			        "pkg_build() {\n\tsleep 600\n}\n\n"
			        "pkg_install() {\n\ttrue\n}\n",
			        test_http_src(c_tarball), c_sha);
			fclose(cf);
		}

		if (okc) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install",
			                        "{\"name\":\"sleeper\",\"image\":\"base\"}", &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: #213 install sleeper, status=%d\n", r.status);
				ok = 0;
				okc = 0;
			}
			cix_response_free(&r);
		}

		/* Wait until it is genuinely building, not merely fetching. */
		if (okc) {
			int building = 0;

			for (i = 0; i < 120 && !building; i++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/sleeper", NULL, &r) == 0 &&
				    r.status == 200 && r.body != NULL &&
				    strstr(r.body, "\"state\":\"building\"") != NULL)
					building = 1;
				cix_response_free(&r);
				if (!building)
					usleep(250000);
			}
			if (!building) {
				fprintf(stderr, "FAIL: #213 sleeper never reached building\n");
				ok = 0;
				okc = 0;
			}
		}

		if (okc) {
			/* What the daemon thinks is running, before and after --
			 * the decisive datum if the cancel does not take: a
			 * container still present means the kill did not happen,
			 * an absent one means the kill happened and the
			 * completion path did not notice. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers", NULL, &r) == 0)
				fprintf(stderr, "#213 containers BEFORE cancel: %s\n",
				        r.body != NULL ? r.body : "(none)");
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/cancel",
			                        "{\"name\":\"sleeper\",\"image\":\"base\"}", &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: #213 cancel running build, status=%d\n", r.status);
				ok = 0;
				okc = 0;
			}
			cix_response_free(&r);
		}

		if (okc) {
			int cancelled = 0;

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/containers", NULL, &r) == 0)
				fprintf(stderr, "#213 containers AFTER cancel: %s\n",
				        r.body != NULL ? r.body : "(none)");
			cix_response_free(&r);

			for (i = 0; i < 120 && !cancelled; i++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/sleeper", NULL, &r) == 0 &&
				    r.status == 200 && r.body != NULL &&
				    strstr(r.body, "\"state\":\"failed\"") != NULL &&
				    strstr(r.body, "\"status\":\"cancelled\"") != NULL)
					cancelled = 1;
				cix_response_free(&r);
				if (!cancelled)
					usleep(250000);
			}
			if (!cancelled) {
				/*
				 * Say what it DID become. A check that only
				 * reports "not what I wanted" leaves the reader
				 * guessing, which is how the same failure gets
				 * diagnosed twice.
				 */
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/sleeper", NULL, &r) == 0)
					fprintf(stderr,
					        "FAIL: #213 cancelled build did not end failed/cancelled; "
					        "entry is now: %s\n",
					        r.body != NULL ? r.body : "(no body)");
				else
					fprintf(stderr,
					        "FAIL: #213 cancelled build did not end failed/cancelled, "
					        "and the entry could not be read back\n");
				cix_response_free(&r);
				ok = 0;
			}

			/*
			 * Now that it is genuinely not building, a second
			 * cancel must be REFUSED. This is the race the 409
			 * exists for: a cancel arriving after the build has
			 * finished must not be able to restate the outcome.
			 */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/cancel",
			                        "{\"name\":\"sleeper\",\"image\":\"base\"}", &r) != 0 ||
			    r.status != 409) {
				fprintf(stderr,
				        "FAIL: #213 second cancel, status=%d (want 409)\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
		}
	}

	/* #407: every build_container gate violation poll_pkg_state() saw,
	 * across every install above. Reported as one line rather than
	 * folded into ok at the point of failure, so the count is visible
	 * -- a gate that broke for one package and not another says
	 * something different from one that broke for all of them. */
	if (g_build_container_gate_fails != 0) {
		fprintf(stderr, "FAIL: %d build_container state-gate violation(s)\n",
		        g_build_container_gate_fails);
		ok = 0;
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	test_data_dir_cleanup(g_data_dir);
	printf(ok ? "PKG RESULT: PASS\n" : "PKG RESULT: FAIL\n");
	return ok ? 0 : 1;
}
