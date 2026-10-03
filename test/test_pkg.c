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
#include "base64.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include "json.h"
#include "test_image_fixture.h"
#include "test_floor.h"
#include "test_pgp_fixture.h"

#include <errno.h>
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

/*
 * #500: POST /v1/pkg/install refuses an image that does not exist, so a
 * case installing into its own image creates it first, the one way an
 * image is made. Returns 0 on 201, and reports the status otherwise.
 */
static int create_image(const struct cix_client *c, const char *name)
{
	struct cix_response r;
	char body[128];
	int rc;

	snprintf(body, sizeof(body), "{\"name\":\"%s\"}", name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "POST", "/v1/images", body, &r) != 0) {
		fprintf(stderr, "FAIL: POST /v1/images %s: no response\n", name);
		return -1;
	}
	rc = r.status == 201 ? 0 : -1;
	if (rc != 0)
		fprintf(stderr, "FAIL: POST /v1/images %s, status=%d %.200s\n", name, r.status,
		        r.body != NULL ? r.body : "");
	cix_response_free(&r);
	return rc;
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

/* A fresh Ed25519 private key, PEM, for the catalogue-signing case (ADR-0324). */
static char *gen_ed25519_pem(void)
{
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
	EVP_PKEY *key = NULL;
	BIO *bio = NULL;
	char *data = NULL, *out = NULL;
	long n;

	if (ctx == NULL || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &key) <= 0)
		goto done;
	bio = BIO_new(BIO_s_mem());
	if (bio == NULL || PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL) != 1)
		goto done;
	n = BIO_get_mem_data(bio, &data);
	if (n > 0)
		out = strndup(data, (size_t)n);
done:
	BIO_free(bio);
	EVP_PKEY_free(key);
	EVP_PKEY_CTX_free(ctx);
	return out;
}

static int str_eq(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* Reads a whole file into a malloc'd, NUL-terminated buffer. 0 or -1. */
static int slurp_file(const char *path, char **out, size_t *out_len)
{
	FILE *fp = fopen(path, "rb");
	long size;
	char *buf;

	*out = NULL;
	*out_len = 0;
	if (fp == NULL)
		return -1;
	if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}
	buf = malloc((size_t)size + 1);
	if (buf == NULL || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	buf[size] = '\0';
	*out = buf;
	*out_len = (size_t)size;
	return 0;
}

/*
 * ADR-0324: starts a recipe sync and waits for it to finish. The final
 * GET /v1/pkg/sync is left in r for the caller to read and free; state
 * is its "state", or why there is none.
 */
static int sync_and_wait(const struct cix_client *c, char *state, size_t state_size,
                         struct cix_response *r)
{
	int i;

	memset(r, 0, sizeof(*r));
	if (cix_client_request(c, "POST", "/v1/pkg/sync", NULL, r) != 0 || r->status != 202) {
		snprintf(state, state_size, "start answered %d", r->status);
		cix_response_free(r);
		memset(r, 0, sizeof(*r));
		return -1;
	}
	cix_response_free(r);
	for (i = 0; i < 240; i++) {
		memset(r, 0, sizeof(*r));
		if (cix_client_request(c, "GET", "/v1/pkg/sync", NULL, r) == 0 && r->json != NULL &&
		    json_str_field(r->json, "state") != NULL &&
		    strcmp(json_str_field(r->json, "state"), "running") != 0) {
			snprintf(state, state_size, "%s", json_str_field(r->json, "state"));
			return 0;
		}
		cix_response_free(r);
		usleep(500000);
	}
	memset(r, 0, sizeof(*r));
	snprintf(state, state_size, "%s", "timeout");
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
	if (test_pkg_config_seed_cleared(g_pkg_state_dir) != 0)
		fprintf(stderr, "reset: could not re-seed the cleared pkg config in %s\n", g_pkg_state_dir);
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
 * multisrc scenario below (ADR-0036). Under CPDL a non-archive
 * source lands at ${src}/<source-name>/<basename>, never extracted. */
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
	jw_str(&w, "cbs");
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
/*
 * The build tools nearly every fixture here declares: the compiler,
 * and the four packages a `tcc -o hello hello.c` plus a copy actually
 * needs in a composed build container. `bash` is not optional and is
 * not a dependency of what the phase invokes -- without it the build
 * container's PID 1 cannot start the phase at all and the failure
 * presents as a timeout (cbs#224).
 */
#define CPDL_STD_TOOLS                                                                             \
	"            compiler \"tcc\"\n"                                                               \
	"            tool \"linux-headers\"\n"                                                         \
	"            tool \"bash\"\n"                                                                  \
	"            tool \"coreutils\"\n"                                                             \
	"            tool \"binutils\"\n"

/*
 * The CPDL text alone, without publishing it.
 *
 * Split out of publish_cpdl_recipe() for the recipe-API block, which
 * is the one caller that needs the document rather than the effect:
 * it posts the same content under a deliberately wrong name, posts it
 * again under the right one, and later compares `GET
 * /v1/pkg/recipes/{name}`'s `content` field against it byte for byte.
 * A helper that only ever published could not express any of those.
 *
 * `decls` goes at package level, before the first phase, where CPDL
 * puts declarations such as `privileged file` (#552). NULL for none.
 */
static void cpdl_recipe_text_decl(char *out, size_t out_size, const char *name,
                                  const char *version, const char *source_url,
                                  const char *sha256, const char *extra_sources,
                                  const char *tools, const char *runtime, const char *decls,
                                  const char *build_body, const char *install_body)
{
	snprintf(out, out_size,
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
	         "%s"
	         "    build {\n"
	         "%s"
	         "    }\n"
	         "\n"
	         "    install {\n"
	         "%s"
	         "    }\n"
	         "}\n",
	         name, version, name, source_url, sha256,
	         extra_sources != NULL ? extra_sources : "", tools,
	         runtime != NULL ? runtime : "", decls != NULL ? decls : "", build_body,
	         install_body);
}

/*
 * A minimal CPDL recipe declaring an upstream block (ADR-0323). Its own
 * template rather than cpdl_recipe_text_decl()'s `decls` slot, because
 * cbs ranks `upstream` with `sources` -- before `requires` -- and
 * refuses it anywhere later with CPDL-E3003 "package declaration
 * appears out of order" (validate.c package_item_rank(), v0.1.102;
 * measured as 0.2.57-446's selftest failure).
 */
static void cpdl_upstream_recipe_text(char *out, size_t out_size, const char *name,
                                      const char *source_url, const char *sha256,
                                      const char *upstream)
{
	snprintf(out, out_size,
	         "package \"%s\" {\n"
	         "    version \"1.0\"\n"
	         "    release 1\n"
	         "    format \"cixpkg\"\n"
	         "\n"
	         "%s"
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
	         "            tool \"bash\"\n"
	         "        }\n"
	         "    }\n"
	         "\n"
	         "    build {\n"
	         "        run \"true\" {\n"
	         "        }\n"
	         "    }\n"
	         "\n"
	         "    install {\n"
	         "        mkdir \"${dest}/usr/share/%s\" parents\n"
	         "    }\n"
	         "}\n",
	         name, upstream, name, source_url, sha256, name);
}

static void cpdl_recipe_text(char *out, size_t out_size, const char *name, const char *version,
                              const char *source_url, const char *sha256,
                              const char *extra_sources, const char *tools, const char *runtime,
                              const char *build_body, const char *install_body)
{
	cpdl_recipe_text_decl(out, out_size, name, version, source_url, sha256, extra_sources, tools,
	                      runtime, NULL, build_body, install_body);
}

/*
 * The same document, written into the recipe store instead of posted.
 *
 * Only for a fixture created BEFORE start_daemon(). A `.cbs` dropped
 * in while the daemon is running has no derived identity and every
 * install of it is refused 400 (ADR-0305) -- but one present at
 * startup is picked up by the daemon's own sweep, which derives the
 * identity exactly as a publish would. `test_pkg_concurrent_stress`
 * has relied on that for a while; `chatty` is the only fixture in
 * this file that needs it, because its whole purpose is to have
 * produced build-log output before the first install runs.
 *
 * Anything created after the daemon is up goes through
 * publish_cpdl_recipe() instead, and the difference is not stylistic.
 */
static int write_cpdl_recipe_file(const char *name, const char *version,
                                   const char *tarball_path, const char *sha256,
                                   const char *tools, const char *build_body,
                                   const char *install_body)
{
	char dir[PATH_MAX];
	char path[PATH_MAX];
	char content[4096];
	FILE *f;

	cpdl_recipe_text(content, sizeof(content), name, version, test_http_src(tarball_path), sha256, NULL, tools,
	                 NULL, build_body, install_body);
	snprintf(dir, sizeof(dir), "%s/recipes/%s", g_pkg_state_dir, name);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/recipes/%s/%s", g_pkg_state_dir, name, version);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/build.cbs", dir);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	if (fputs(content, f) == EOF) {
		fclose(f);
		return -1;
	}
	return fclose(f) == 0 ? 0 : -1;
}

/* POST one already-rendered CPDL document. The publish helpers differ
 * only in how they name the fetch, so the request itself is written
 * once. `source` names the recipe source it belongs to (ADR-0324), or
 * NULL to let the host decide. */
static char *cpdl_publish_body(const char *name, const char *content, const char *source)
{
	struct json_writer w;
	char *body;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_key(&w, "format");
	jw_str(&w, "cbs");
	if (source != NULL) {
		jw_key(&w, "source");
		jw_str(&w, source);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	body = strdup(w.buf);
	jw_free(&w);
	return body;
}

static int publish_cpdl_content_from(const struct cix_client *c, const char *name,
                                      const char *version, const char *content,
                                      const char *source)
{
	char *body = cpdl_publish_body(name, content, source);
	struct cix_response r;
	int ok;

	if (body == NULL)
		return -1;
	memset(&r, 0, sizeof(r));
	ok = (cix_client_request(c, "POST", "/v1/pkg/recipes", body, &r) == 0 && r.status == 204);
	if (!ok)
		fprintf(stderr, "      POST /v1/pkg/recipes %s@%s: status=%d %.200s\n", name, version,
		        r.status, r.body != NULL ? r.body : "");
	cix_response_free(&r);
	free(body);
	return ok ? 0 : -1;
}

static int publish_cpdl_content(const struct cix_client *c, const char *name, const char *version,
                                 const char *content)
{
	return publish_cpdl_content_from(c, name, version, content, NULL);
}

static int publish_cpdl_recipe(const struct cix_client *c, const char *name, const char *version,
                                const char *tarball_path, const char *sha256,
                                const char *extra_sources, const char *tools,
                                const char *runtime, const char *build_body,
                                const char *install_body)
{
	char content[4096];

	cpdl_recipe_text(content, sizeof(content), name, version, test_http_src(tarball_path), sha256,
	                 extra_sources, tools, runtime, build_body, install_body);
	return publish_cpdl_content(c, name, version, content);
}

/*
 * Publish a recipe whose source is somewhere other than the fixture
 * HTTP server.
 *
 * One fixture needs this and it is not an edge case: `unreachable`
 * points at 192.0.2.1, TEST-NET-1, reserved for documentation and
 * routed nowhere, because its whole subject is what a fetch failure
 * reports. A helper that can only name a file the test is serving
 * cannot express "a source that does not answer".
 */
static int publish_cpdl_recipe_url(const struct cix_client *c, const char *name,
                                    const char *version, const char *source_url,
                                    const char *sha256, const char *tools,
                                    const char *build_body, const char *install_body)
{
	char content[4096];

	cpdl_recipe_text(content, sizeof(content), name, version, source_url, sha256, NULL, tools,
	                 NULL, build_body, install_body);
	return publish_cpdl_content(c, name, version, content);
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
 * A recipe that installs one setuid file, declared as CPDL requires
 * (#552): cbs refuses an undeclared setuid entry, and the installer
 * keeps a declared one.
 */
static int write_suid_recipe(const struct cix_client *c, const char *name, const char *version,
                             const char *tarball_path, const char *sha256)
{
	char srcdir[160];
	char build_body[512], install_body[768], decls[256];
	char content[4096];

	fixture_srcdir(tarball_path, srcdir, sizeof(srcdir));
	snprintf(build_body, sizeof(build_body),
	         "        cd \"${src}/%s/%s\" {\n"
	         "            run \"tcc\" {\n"
	         "                \"-o\" \"hello\" \"hello.c\"\n"
	         "            }\n"
	         "        }\n",
	         name, srcdir);
	snprintf(install_body, sizeof(install_body),
	         "        mkdir \"${dest}/usr/libexec/%s\" parents chmod 0755\n"
	         "        copy \"${src}/%s/%s/hello\" to \"${dest}/usr/libexec/%s/helper\"\n"
	         "        chmod 04711 \"${dest}/usr/libexec/%s/helper\"\n",
	         name, name, srcdir, name, name);
	snprintf(decls, sizeof(decls),
	         "    privileged file \"${dest}/usr/libexec/%s/helper\" mode 04711\n\n", name);
	cpdl_recipe_text_decl(content, sizeof(content), name, version, test_http_src(tarball_path),
	                      sha256, NULL,
	                      "            compiler \"tcc\"\n"
	                      "            tool \"linux-headers\"\n"
	                      "            tool \"bash\"\n"
	                      "            tool \"coreutils\"\n"
	                      "            tool \"binutils\"\n",
	                      NULL, decls, build_body, install_body);
	return publish_cpdl_content(c, name, version, content);
}

/*
 * An ordinary hello recipe that declares how much memory its whole
 * build needs (CPDL `resources { memory }`, cbs v0.1.100, cix#558).
 * memory is the declared value as written in the recipe, e.g. "3GiB".
 */
static int write_memory_recipe(const struct cix_client *c, const char *name, const char *version,
                               const char *tarball_path, const char *sha256, const char *memory)
{
	char srcdir[160];
	char build_body[512], install_body[512], decls[128];
	char content[4096];

	fixture_srcdir(tarball_path, srcdir, sizeof(srcdir));
	snprintf(build_body, sizeof(build_body),
	         "        cd \"${src}/%s/%s\" {\n"
	         "            run \"tcc\" {\n"
	         "                \"-o\" \"hello\" \"hello.c\"\n"
	         "            }\n"
	         "        }\n",
	         name, srcdir);
	snprintf(install_body, sizeof(install_body),
	         "        mkdir \"${dest}/usr/bin\" parents chmod 0755\n"
	         "        copy \"${src}/%s/%s/hello\" to \"${dest}/usr/bin/%s\"\n",
	         name, srcdir, name);
	snprintf(decls, sizeof(decls), "    resources {\n        memory \"%s\"\n    }\n\n", memory);
	cpdl_recipe_text_decl(content, sizeof(content), name, version, test_http_src(tarball_path),
	                      sha256, NULL,
	                      "            compiler \"tcc\"\n"
	                      "            tool \"linux-headers\"\n"
	                      "            tool \"bash\"\n"
	                      "            tool \"coreutils\"\n"
	                      "            tool \"binutils\"\n",
	                      NULL, decls, build_body, install_body);
	return publish_cpdl_content(c, name, version, content);
}

/*
 * A recipe that installs one file of its own AND one path another
 * fixture installs too -- the collision #553 refuses. It is not normal
 * any more: glibc and linux-headers, the example this comment used to
 * give, measure no shared path at all on 192.168.15.95 (2026-09-30).
 */
static int write_shared_path_recipe(const struct cix_client *c, const char *name,
                                    const char *version, const char *tarball_path,
                                    const char *sha256, const char *shared_rel,
                                    const char *decls)
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
	/* One file of its own and one another fixture also installs (#553).
	 * `mkdir` takes the shared file's parent because
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

	{
		char content[4096];

		/* decls: package-level declarations, e.g. `replaces` (#553). */
		cpdl_recipe_text_decl(content, sizeof(content), name, version,
		                      test_http_src(tarball_path), sha256, "",
		                      "            compiler \"tcc\"\n"
		                      "            tool \"linux-headers\"\n"
		                      "            tool \"bash\"\n"
		                      "            tool \"coreutils\"\n"
		                      "            tool \"binutils\"\n",
		                      "", decls, build_body, install_body);
		return publish_cpdl_content(c, name, version, content);
	}
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
/*
 * A space-separated build-tool list, as CPDL `tool` declarations.
 *
 * The callers' strings are unchanged by the move to CPDL, because
 * the pinned spelling is the same one they already used: the CPDL
 * spec's own example is `tool "tcc@0.9.27-7"` and upstream's
 * `tests/fixtures/valid/complete.cbs` carries `tool "ninja@1.12.1-2"`
 * (read from cbs v0.1.63 by probe-cbs-toolpin@1-1, rather than
 * guessed). So "greeter@1.0-1" travels through as-is.
 *
 * Everything is emitted as `tool`, including the compiler. That is
 * what the shell form's flat pkg_build_depends meant, and the spec
 * pins tcc as a tool in the very line quoted above, so the two
 * spellings do not differ in what they declare.
 */
static void cpdl_tools_from_list(char *out, size_t out_size, const char *list)
{
	size_t len = 0;
	const char *p = list;

	out[0] = '\0';
	while (*p != '\0') {
		const char *start;

		while (*p == ' ')
			p++;
		start = p;
		while (*p != '\0' && *p != ' ')
			p++;
		if (p == start)
			break;
		len += (size_t)snprintf(out + len, out_size - len, "            tool \"%.*s\"\n",
		                        (int)(p - start), start);
		if (len >= out_size)
			return;
	}
}

/*
 * Issue #109: a recipe that DECLARES its build tools. The build
 * container is then composed from exactly those packages and nothing
 * else, so what this build ran against is a property of the recipe
 * rather than of whatever the shared sandbox happens to hold.
 */
static int write_builddeps_recipe(const struct cix_client *c, const char *name,
                                   const char *version, const char *tarball_path,
                                   const char *sha256, const char *build_depends)
{
	char tools[1024];
	char srcdir[160];
	char build_body[512], install_body[768];

	/*
	 * This writer takes its declaration from the caller and adds
	 * nothing -- that is its whole purpose (proving a recipe's
	 * declared tools are honoured, including a deliberately
	 * unavailable one, and a pin to a version that is not installed).
	 *
	 * The shell form had a sharper reason for the same rule: two
	 * pkg_build_depends lines meant the parser read the first and
	 * silently ignored the caller's, so a test that should fail
	 * passed instead, which is exactly what happened once. CPDL
	 * cannot hide a declaration that way -- every `tool` line is
	 * additive and visible -- but the rule stands regardless, because
	 * a fixture whose declaration is partly written by its helper is
	 * no longer testing what its call site says it is.
	 *
	 * The consequence is that a caller whose build must actually RUN
	 * has to name the whole floor itself, `bash` included: without
	 * bash the build container's PID 1 cannot start the phase at all
	 * and the failure presents as a timeout (cbs#224, measured here
	 * 2026-09-26). `selfdep` and the observing fixture already did
	 * this; `declaredpresent` and `pinnedgood` now do too.
	 */
	cpdl_tools_from_list(tools, sizeof(tools), build_depends);
	fixture_srcdir(tarball_path, srcdir, sizeof(srcdir));
	snprintf(build_body, sizeof(build_body),
	         "        cd \"${src}/%s/%s\" {\n"
	         "            run \"tcc\" {\n"
	         "                \"-o\" \"hello\" \"hello.c\"\n"
	         "            }\n"
	         "        }\n",
	         name, srcdir);
	snprintf(install_body, sizeof(install_body),
	         "        mkdir \"${dest}/usr/bin\" chmod 0755\n"
	         "        copy \"${src}/%s/%s/hello\" to \"${dest}/usr/bin/%s\"\n",
	         name, srcdir, name);
	return publish_cpdl_recipe(c, name, version, tarball_path, sha256, NULL, tools, NULL,
	                            build_body, install_body);
}

/* Multi-source recipe (ADR-0036): source 0 is the usual fixture
 * tarball; sources 1/2 are plain files, named extra1 and extra2, that
 * land at ${src}/extraN/extraN.txt. The install phase
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
static int write_observing_recipe(const struct cix_client *c, const char *name,
                                   const char *version, const char *tarball_path,
                                   const char *sha256, const char *build_depends)
{
	char tools[1024];

	cpdl_tools_from_list(tools, sizeof(tools), build_depends);
	/*
	 * `run "cp"` rather than CPDL's own `copy`, and that is the one
	 * substantive difference from the shell form.
	 *
	 * The file being read is `/usr/share/stamped.version`, which the
	 * declared build tool left in the composed environment -- it is
	 * not under ${src}, ${build} or ${dest}, and CPDL's filesystem
	 * operations are confined to those roots (its own suite calls the
	 * property "confinement"). coreutils' cp is an ordinary program
	 * and is declared by every caller of this helper, so the copy
	 * goes through a `run`, which is unremarkable in an install phase
	 * -- 262 of them across the corpus.
	 *
	 * Reading a path outside the recipe's roots is the whole point of
	 * this fixture rather than an accident of it: the installed
	 * package has to record which version of its declared tool the
	 * build genuinely ran against, observable afterwards when the
	 * build environment no longer exists (ADR-0209 tears it down).
	 */
	return publish_cpdl_recipe(c, name, version, tarball_path, sha256, NULL, tools, NULL,
	                            "        run \"true\" {\n"
	                            "        }\n",
	                            "        mkdir \"${dest}/usr/share\" chmod 0755\n"
	                            "        run \"cp\" {\n"
	                            "            \"/usr/share/stamped.version\"\n"
	                            "            \"${dest}/usr/share/observed.version\"\n"
	                            "        }\n");
}

static int write_multisrc_recipe(const struct cix_client *c, const char *name,
                                  const char *version, const char *tarball_path,
                                  const char *tarball_sha256, const char *extra1_path,
                                  const char *extra1_sha256, const char *extra2_path,
                                  const char *extra2_sha256)
{
	char srcdir[160];
	char extra_sources[768];
	char build_body[512], install_body[768];

	/*
	 * Three sources (ADR-0036): the fixture tarball, and two plain
	 * files. The shell form declared them as one space-separated
	 * pkg_source and they landed at /build/extra/extraN.txt; CPDL
	 * names each one, and a non-archive source arrives at
	 * ${src}/<source-name>/<basename> rather than being unpacked.
	 */
	snprintf(extra_sources, sizeof(extra_sources),
	         "        extra \"extra1\" {\n"
	         "            url \"%s\"\n"
	         "            sha256 \"%s\"\n"
	         "        }\n"
	         "        extra \"extra2\" {\n"
	         "            url \"%s\"\n"
	         "            sha256 \"%s\"\n"
	         "        }\n",
	         test_http_src(extra1_path), extra1_sha256, test_http_src(extra2_path),
	         extra2_sha256);

	fixture_srcdir(tarball_path, srcdir, sizeof(srcdir));
	snprintf(build_body, sizeof(build_body),
	         "        cd \"${src}/%s/%s\" {\n"
	         "            run \"tcc\" {\n"
	         "                \"-o\" \"hello\" \"hello.c\"\n"
	         "            }\n"
	         "        }\n",
	         name, srcdir);
	/* extra1.txt is copied into the staged tree on purpose, so the
	 * caller can read its real byte content afterwards -- proof the
	 * file was genuinely there during the build, not merely that the
	 * job succeeded. */
	snprintf(install_body, sizeof(install_body),
	         "        mkdir \"${dest}/usr/bin\" chmod 0755\n"
	         /* usr/share first, each level named: parents are created by
	          * default again since cbs v0.1.70 (cix-build-system#246), but
	          * that default has changed twice, so nothing here leans on it.
	          * The directory is the package's own name, because two
	          * fixtures installing one path into base is the collision #553
	          * refuses. */
	         "        mkdir \"${dest}/usr/share\" chmod 0755\n"
	         "        mkdir \"${dest}/usr/share/%s\" chmod 0755\n"
	         "        copy \"${src}/%s/%s/hello\" to \"${dest}/usr/bin/%s\"\n"
	         /* materialize, not copy: an `extra` source is NOT placed
	          * under ${src} -- only `main` is. Measured by
	          * probe-plainsrc@1-1 on 192.168.15.95, 2026-09-26, whose
	          * ${src} held the main source and nothing else. The copy
	          * this replaces failed naming its DESTINATION, which had
	          * just been created; the bogus path was the source. */
	         "        materialize $source.extra1 to \"${dest}/usr/share/%s/extra1.txt\"\n",
	         name, name, srcdir, name, name);

	return publish_cpdl_recipe(c, name, version, tarball_path, tarball_sha256, extra_sources,
	                            "            compiler \"tcc\"\n"
	                            "            tool \"linux-headers\"\n"
	                            "            tool \"bash\"\n"
	                            "            tool \"coreutils\"\n"
	                            "            tool \"binutils\"\n",
	                            "", build_body, install_body);
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

/*
 * ADR-0315: the public catalogue and cache are what a host starts
 * with, and an operator can clear either for good.
 *
 * Runs against its own data directory, before anything else, because
 * every other daemon in this test starts from the cleared config
 * test_data_dir_create() seeds -- so this is the one place a daemon
 * sees no saved config at all. Nothing here syncs or installs, so the
 * defaults are read and never contacted.
 */
static int read_small_file(const char *path, char *out, size_t out_size)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (f == NULL)
		return -1;
	n = fread(out, 1, out_size - 1, f);
	out[n] = '\0';
	fclose(f);
	return 0;
}

static int check_pkg_config(const struct cix_client *c, const char *want_repo_url,
                            const char *want_kind, const char *want_ref,
                            const char *want_artifact_url, const char *when)
{
	struct cix_response r;
	int ok = 1;

	memset(&r, 0, sizeof(r));
	/* ADR-0324: the single repo config became a list of sources; the
	 * ADR-0315 default is its one entry, and a cleared host has none. */
	if (cix_client_request(c, "GET", "/v1/pkg/sources", NULL, &r) != 0 || r.status != 200 ||
	    r.json == NULL) {
		fprintf(stderr, "FAIL: sources %s: status=%d\n", when, r.status);
		ok = 0;
	} else {
		const struct json_value *arr = json_object_get(r.json, "sources");
		size_t want = want_repo_url[0] == '\0' ? 0 : 1;

		if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count != want) {
			fprintf(stderr, "FAIL: sources %s: want %zu source(s), got %s\n", when, want,
			        r.body != NULL ? r.body : "(none)");
			ok = 0;
		} else if (want == 1) {
			const struct json_value *s = arr->u.array.items[0];
			const struct json_value *tk = json_object_get(s, "trust_keys");
			const struct json_value *wr = json_object_get(s, "write");

			if (!str_eq(json_str_field(s, "url"), want_repo_url) ||
			    !str_eq(json_str_field(s, "kind"), want_kind) ||
			    !str_eq(json_str_field(s, "ref"), want_ref) ||
			    !str_eq(json_str_field(s, "name"), "cix-public") || tk == NULL ||
			    tk->type != JSON_BOOL || !tk->u.boolean || wr == NULL ||
			    wr->type != JSON_BOOL || wr->u.boolean) {
				fprintf(stderr,
				        "FAIL: sources %s: want cix-public %s %s %s, read, trusted for keys; "
				        "got %s\n",
				        when, want_repo_url, want_kind, want_ref, r.body);
				ok = 0;
			}
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	/* ADR-0324 step B: likewise the repositories -- the public cache,
	 * pull only, or nothing once cleared. */
	if (cix_client_request(c, "GET", "/v1/pkg/repositories", NULL, &r) != 0 || r.status != 200 ||
	    r.json == NULL) {
		fprintf(stderr, "FAIL: repositories %s: status=%d\n", when, r.status);
		ok = 0;
	} else {
		const struct json_value *arr = json_object_get(r.json, "repositories");
		size_t want = want_artifact_url[0] == '\0' ? 0 : 1;

		if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count != want) {
			fprintf(stderr, "FAIL: repositories %s: want %zu repository, got %s\n", when, want,
			        r.body != NULL ? r.body : "(none)");
			ok = 0;
		} else if (want == 1) {
			const struct json_value *rep = arr->u.array.items[0];
			const struct json_value *push = json_object_get(rep, "push");

			if (!str_eq(json_str_field(rep, "url"), want_artifact_url) ||
			    !str_eq(json_str_field(rep, "name"), "cix-public") || push == NULL ||
			    push->type != JSON_BOOL || push->u.boolean) {
				fprintf(stderr, "FAIL: repositories %s: want cix-public at %s, pull only: %s\n",
				        when, want_artifact_url, r.body != NULL ? r.body : "");
				ok = 0;
			}
		}
	}
	cix_response_free(&r);
	return ok;
}

static int test_pkg_config_defaults(void)
{
	char saved_data_dir[PATH_MAX];
	char dir[PATH_MAX], path[PATH_MAX], content[1024];
	struct cix_client c;
	struct cix_response r;
	pid_t pid;
	int ok = 1;

	snprintf(saved_data_dir, sizeof(saved_data_dir), "%s", g_data_dir);
	if (test_data_dir_create(dir, sizeof(dir)) != 0) {
		fprintf(stderr, "FAIL: defaults: could not create a data dir\n");
		return 0;
	}
	/* Undo the harness's cleared seed: this daemon must see no file. */
	snprintf(path, sizeof(path), "%s/rebuildable/pkg/sources.json", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/rebuildable/pkg/repositories.json", dir);
	unlink(path);
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir);

	cix_client_init(&c, "127.0.0.1", TEST_PORT);
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		fprintf(stderr, "FAIL: defaults: daemon never accepted connections\n");
		ok = 0;
		goto out;
	}
	ok &= check_pkg_config(&c, "https://github.com/The-Cix-Project/cix-recipes", "github", "main",
	                       "https://cache.cix.world", "with no saved config");

	/* Clear both, as an operator would. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&c, "DELETE", "/v1/pkg/sources/cix-public", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: defaults: removing the default source returned %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&c, "DELETE", "/v1/pkg/repositories/cix-public", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: defaults: removing the default repository returned %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	/* The clear is on disk, as an explicit empty value ... */
	snprintf(path, sizeof(path), "%s/rebuildable/pkg/sources.json", dir);
	if (read_small_file(path, content, sizeof(content)) != 0 ||
	    strstr(content, "\"sources\":[]") == NULL) {
		fprintf(stderr, "FAIL: defaults: the emptied source list was not persisted\n");
		ok = 0;
	}
	snprintf(path, sizeof(path), "%s/rebuildable/pkg/repositories.json", dir);
	if (read_small_file(path, content, sizeof(content)) != 0 ||
	    strstr(content, "\"repositories\":[]") == NULL) {
		fprintf(stderr, "FAIL: defaults: the emptied repository list was not persisted\n");
		ok = 0;
	}

	/* ... and a restart does not bring the defaults back. */
	if (stop_daemon(pid) != 0) {
		fprintf(stderr, "FAIL: defaults: daemon did not stop cleanly\n");
		ok = 0;
	}
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		fprintf(stderr, "FAIL: defaults: daemon did not come back after restart\n");
		ok = 0;
		goto out;
	}
	ok &= check_pkg_config(&c, "", "github", "main", "", "after clearing and restarting");

out:
	if (pid > 0 && stop_daemon(pid) != 0) {
		fprintf(stderr, "FAIL: defaults: daemon did not stop cleanly\n");
		ok = 0;
	}
	test_data_dir_cleanup(dir);
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", saved_data_dir);
	return ok;
}

/*
 * ADR-0316: a host that has never saved a schedule file gets a
 * six-hourly recipe-sync, and an operator who deletes it keeps it
 * deleted.
 *
 * The data dir keeps its emptied source list, so the sync the new job
 * fires at startup fails with "no recipe source is configured" and reaches no
 * network; only schedules.json is removed.
 */
static int get_schedule(const struct cix_client *c, const char *name, struct cix_response *r)
{
	char path[128];

	snprintf(path, sizeof(path), "/v1/schedules/%s", name);
	memset(r, 0, sizeof(*r));
	return cix_client_request(c, "GET", path, NULL, r);
}

static int test_schedule_defaults(void)
{
	char saved_data_dir[PATH_MAX];
	char dir[PATH_MAX], path[PATH_MAX];
	struct cix_client c;
	struct cix_response r;
	const struct json_value *every;
	pid_t pid;
	int ok = 1;

	snprintf(saved_data_dir, sizeof(saved_data_dir), "%s", g_data_dir);
	if (test_data_dir_create(dir, sizeof(dir)) != 0) {
		fprintf(stderr, "FAIL: schedule defaults: could not create a data dir\n");
		return 0;
	}
	snprintf(path, sizeof(path), "%s/state/schedules.json", dir);
	unlink(path);
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir);

	cix_client_init(&c, "127.0.0.1", TEST_PORT);
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		fprintf(stderr, "FAIL: schedule defaults: daemon never accepted connections\n");
		ok = 0;
		goto out;
	}

	if (get_schedule(&c, "recipe-sync", &r) != 0 || r.status != 200 ||
	    !str_eq(json_str_field(r.json, "action"), "pkg.sync")) {
		fprintf(stderr, "FAIL: schedule defaults: no pkg.sync recipe-sync on a fresh host (%d)\n",
		        r.status);
		ok = 0;
	} else {
		every = json_object_get(json_object_get(r.json, "schedule"), "every");
		if (json_as_number(json_object_get(every, "seconds")) != 21600 ||
		    json_object_get(r.json, "enabled") == NULL ||
		    json_object_get(r.json, "enabled")->type != JSON_BOOL ||
		    !json_object_get(r.json, "enabled")->u.boolean) {
			fprintf(stderr, "FAIL: schedule defaults: recipe-sync is not an enabled "
			                "every-21600-seconds job\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* ADR-0327: and the nightly host roll, at the owner's 03:00, for 3h. */
	if (get_schedule(&c, "host-roll", &r) != 0 || r.status != 200 ||
	    !str_eq(json_str_field(r.json, "action"), "system.roll") ||
	    !str_eq(json_str_field(json_object_get(json_object_get(r.json, "schedule"), "daily"), "at"),
	            "03:00") ||
	    json_as_number(json_object_get(r.json, "window_minutes")) != 180) {
		fprintf(stderr, "FAIL: schedule defaults: no nightly 03:00 system.roll host-roll with a "
		                "3h window on a fresh host: %s\n",
		        r.body != NULL ? r.body : "(no body)");
		ok = 0;
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&c, "DELETE", "/v1/schedules/recipe-sync", NULL, &r) != 0 ||
	    r.status != 204) {
		fprintf(stderr, "FAIL: schedule defaults: deleting recipe-sync returned %d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	if (stop_daemon(pid) != 0) {
		fprintf(stderr, "FAIL: schedule defaults: daemon did not stop cleanly\n");
		ok = 0;
	}
	pid = start_daemon();
	if (pid < 0 || wait_for_daemon(&c, 50) != 0) {
		fprintf(stderr, "FAIL: schedule defaults: daemon did not come back after restart\n");
		ok = 0;
		goto out;
	}
	if (get_schedule(&c, "recipe-sync", &r) != 0 || r.status != 404) {
		fprintf(stderr, "FAIL: schedule defaults: a deleted recipe-sync came back after a "
		                "restart (%d)\n",
		        r.status);
		ok = 0;
	}
	cix_response_free(&r);

out:
	if (pid > 0 && stop_daemon(pid) != 0) {
		fprintf(stderr, "FAIL: schedule defaults: daemon did not stop cleanly\n");
		ok = 0;
	}
	test_data_dir_cleanup(dir);
	snprintf(g_data_dir, sizeof(g_data_dir), "%s", saved_data_dir);
	return ok;
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

	if (!test_pkg_config_defaults())
		ok = 0;
	if (!test_schedule_defaults())
		ok = 0;

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
	 * never recorded a byte.
	 *
	 * Written into the store rather than published, because it has to
	 * exist before the daemon starts -- see write_cpdl_recipe_file()
	 * for why that is the one case where dropping the file in is
	 * correct rather than a mistake. */
	{
		char srcdir[160];
		char build_body[640], install_body[512];

		fixture_srcdir(tarball_path, srcdir, sizeof(srcdir));
		/* The two markers bracket the compile, so an assertion that
		 * finds both has proof the log captured the whole phase and
		 * not just its first line. */
		snprintf(build_body, sizeof(build_body),
		         "        cd \"${src}/chatty/%s\" {\n"
		         "            run \"echo\" {\n"
		         "                \"BUILD_LOG_MARKER_ONE\"\n"
		         "            }\n"
		         "            run \"tcc\" {\n"
		         "                \"-o\" \"hello\" \"hello.c\"\n"
		         "            }\n"
		         "            run \"echo\" {\n"
		         "                \"BUILD_LOG_MARKER_TWO\"\n"
		         "            }\n"
		         "        }\n",
		         srcdir);
		snprintf(install_body, sizeof(install_body),
		         "        mkdir \"${dest}/usr/bin\" chmod 0755\n"
		         "        copy \"${src}/chatty/%s/hello\" to \"${dest}/usr/bin/chatty\"\n",
		         srcdir);
		if (write_cpdl_recipe_file("chatty", "1.0", tarball_path, sha256, CPDL_STD_TOOLS,
		                            build_body, install_body) != 0) {
			fprintf(stderr, "FAIL: could not write the chatty recipe\n");
			ok = 0;
		}
	}

	/*
	 * ADR-0323 rung 2: a kernel.org release list, seeded where the
	 * daemon reads its cached copy at boot, so the kernel.org kind
	 * resolves stable to 7.2.8 here without the network (tests never
	 * reach the defaults' network). The discovery run below asks for
	 * no refresh, so nothing replaces it.
	 */
	{
		char rel_path[PATH_MAX];
		FILE *rf;

		snprintf(rel_path, sizeof(rel_path), "%s/state/kernel_releases.json", g_data_dir);
		rf = fopen(rel_path, "w");
		if (rf == NULL) {
			fprintf(stderr, "FAIL: could not seed %s\n", rel_path);
			ok = 0;
		} else {
			fputs("{\"releases\":["
			      "{\"moniker\":\"mainline\",\"version\":\"7.3\",\"source\":\"https://k/a\"},"
			      "{\"moniker\":\"stable\",\"version\":\"7.2.8\",\"source\":\"https://k/b\"}"
			      "]}\n",
			      rf);
			fclose(rf);
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
	 * straight to disk here, which a shell recipe allowed and a CBS
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

	/*
	 * 2b. ADR-0309 clause 4: publishing a NEW shell revision is
	 * refused, and an ALREADY-PUBLISHED one still answers duplicate.
	 *
	 * Both halves matter and the second is the one that can silently
	 * break. `recipe-sync` re-offers every file in the corpus every
	 * six hours and relies on duplicates being cheap -- its last run
	 * on 192.168.15.95 counted `added=28 skipped=379`. If the refusal
	 * were placed before the immutability check rather than after it,
	 * each of those 379 skips would become an error, every window,
	 * and the only symptom would be a number in `pkg sync-status`
	 * that nobody reads. So this asserts 400 for the new one and 409
	 * for the stored one, which pins the check's position rather than
	 * merely its existence.
	 *
	 * Note the format field is omitted deliberately on the first
	 * call: an absent format means shell (main.c's own comment says
	 * so, for clients predating CBS), so this is the exact shape a
	 * legacy publisher sends.
	 */
	{
		static const char shell_body[] = "pkg_name=oldshell\n"
		                                 "pkg_version=1.0\n"
		                                 "pkg_source=https://192.0.2.1/x.tar.gz\n"
		                                 "pkg_sha256="
		                                 "0000000000000000000000000000000000000000000000000000"
		                                 "000000000000\n"
		                                 "pkg_build() {\n\ttrue\n}\n"
		                                 "pkg_install() {\n\ttrue\n}\n";
		static const char stored_body[] = "pkg_name=storedshell\n"
		                                  "pkg_version=1.0\n"
		                                  "pkg_source=https://192.0.2.1/x.tar.gz\n"
		                                  "pkg_sha256="
		                                  "0000000000000000000000000000000000000000000000000000"
		                                  "000000000000\n"
		                                  "pkg_build() {\n\ttrue\n}\n"
		                                  "pkg_install() {\n\ttrue\n}\n";
		char body[2048];
		char path[PATH_MAX];
		struct json_writer sw;

		jw_init(&sw);
		jw_obj_open(&sw);
		jw_key(&sw, "name");
		jw_str(&sw, "oldshell");
		jw_key(&sw, "content");
		jw_str(&sw, shell_body);
		jw_obj_close(&sw);
		sw.buf[sw.len] = '\0';
		snprintf(body, sizeof(body), "%s", sw.buf);
		jw_free(&sw);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/recipes", body, &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr,
			        "FAIL: publishing a new shell recipe expected 400 (ADR-0309 clause 4), "
			        "got %d %.200s\n",
			        r.status, r.body != NULL ? r.body : "");
			ok = 0;
		}
		cix_response_free(&r);

		/* It must not have landed: a refused publish leaves nothing. */
		snprintf(path, sizeof(path), "%s/recipes/oldshell/1.0/build.sh", g_pkg_state_dir);
		if (access(path, F_OK) == 0) {
			fprintf(stderr, "FAIL: refused shell recipe was stored anyway at %s\n", path);
			ok = 0;
		}

		/*
		 * Now the same content for a version that IS in the store,
		 * put there the way every shell fixture in this file does it
		 * -- straight into the recipe directory, never through the
		 * publish endpoint. Re-offering it must be a duplicate, not
		 * the clause 4 refusal.
		 */
		if (test_seed_shell_recipe(g_pkg_state_dir, "storedshell", "1.0", stored_body) != 0) {
			fprintf(stderr, "FAIL: could not seed the storedshell recipe\n");
			ok = 0;
		} else {
			jw_init(&sw);
			jw_obj_open(&sw);
			jw_key(&sw, "name");
			jw_str(&sw, "storedshell");
			jw_key(&sw, "content");
			jw_str(&sw, stored_body);
			jw_obj_close(&sw);
			sw.buf[sw.len] = '\0';
			snprintf(body, sizeof(body), "%s", sw.buf);
			jw_free(&sw);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipes", body, &r) != 0 ||
			    r.status != 409) {
				fprintf(stderr,
				        "FAIL: re-publishing a STORED shell recipe expected 409 duplicate "
				        "(the clause 4 refusal must sit after the immutability check, or "
				        "recipe-sync turns 379 skips into 379 errors), got %d %.200s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);
		}
	}

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
					    strcmp(nm, "greeter") == 0) {
						const struct json_value *run = runs->u.array.items[k];
						const struct json_value *sg = json_object_get(run, "stages");
						static const char *const want[] = { "fetch", "build", "install" };
						long total = 0;
						size_t s;

						saw_ok = 1;
						/*
						 * #424: a built-from-source run passes through
						 * fetch, build and install, in that order, and
						 * each stage ends where the next began -- so
						 * their seconds add up to the run's duration
						 * exactly, not approximately.
						 */
						if (sg == NULL || sg->type != JSON_ARRAY || sg->u.array.count != 3) {
							fprintf(stderr, "FAIL: #424 greeter's run has no fetch/build/install "
							                "stages\n");
							ok = 0;
							continue;
						}
						for (s = 0; s < 3; s++) {
							const char *sn = json_str_field(sg->u.array.items[s], "stage");

							if (sn == NULL || strcmp(sn, want[s]) != 0) {
								fprintf(stderr, "FAIL: #424 stage %zu is %s, expected %s\n", s,
								        sn != NULL ? sn : "(none)", want[s]);
								ok = 0;
							}
							total += (long)json_as_number(
							    json_object_get(sg->u.array.items[s], "seconds"));
						}
						if (total !=
						    (long)json_as_number(json_object_get(run, "duration_seconds"))) {
							fprintf(stderr, "FAIL: #424 stage seconds sum to %ld, run took %ld\n",
							        total,
							        (long)json_as_number(json_object_get(run, "duration_seconds")));
							ok = 0;
						}
						/*
						 * #550: the listing names this run's log as
						 * greeter's, from the run record -- not from
						 * the filename -- and as finished.
						 */
						{
							const char *logf = json_str_field(run, "log");
							struct cix_response lr;
							int found = 0;

							memset(&lr, 0, sizeof(lr));
							if (logf != NULL && logf[0] != '\0' &&
							    cix_client_request(&client, "GET", "/v1/pkg/build-logs", NULL,
							                       &lr) == 0 &&
							    lr.status == 200) {
								const struct json_value *ll = json_object_get(lr.json, "logs");
								size_t q;

								for (q = 0; ll != NULL && ll->type == JSON_ARRAY &&
								            q < ll->u.array.count;
								     q++) {
									const struct json_value *le = ll->u.array.items[q];
									const char *lf = json_str_field(le, "file");
									const char *ln = json_str_field(le, "name");
									const char *li = json_str_field(le, "image");
									const struct json_value *fl =
									    json_object_get(le, "in_flight");

									if (lf == NULL || strcmp(lf, logf) != 0)
										continue;
									found = ln != NULL && strcmp(ln, "greeter") == 0 &&
									        li != NULL && strcmp(li, "base") == 0 &&
									        fl != NULL && fl->type == JSON_BOOL &&
									        !fl->u.boolean;
								}
							}
							if (!found) {
								fprintf(stderr, "FAIL: #550 build-logs does not list %s as "
								                "greeter@base, finished\n",
								        logf != NULL ? logf : "(no log)");
								ok = 0;
							}
							cix_response_free(&lr);
						}
					}
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
		/* Unreachable by construction: 192.0.2.0/24 is TEST-NET-1,
		 * reserved for documentation and routed nowhere. The URL is
		 * the fixture, so this one names its own source rather than
		 * pointing at the test's HTTP server. */
		if (publish_cpdl_recipe_url(&client, "unreachable", "1.0",
		                             "https://192.0.2.1/nothing.tar.gz", sha256, CPDL_STD_TOOLS,
		                             "        run \"true\" {\n        }\n",
		                             "        mkdir \"${dest}/usr/share\" chmod 0755\n") != 0) {
			fprintf(stderr, "FAIL: could not publish the unreachable recipe\n");
			ok = 0;
		}

		/* Builds fine, fails in its own build step. `false` rather
		 * than the shell form's `exit 7`: CPDL has no interpreter to
		 * run (CPDL-E3006), and the assertion below is on the failing
		 * STAGE, not on which nonzero code it was. */
		if (publish_cpdl_recipe(&client, "badbuild", "1.0", tarball_path, sha256, NULL,
		                         CPDL_STD_TOOLS, NULL, "        run \"false\" {\n        }\n",
		                         "        mkdir \"${dest}/usr/share\" chmod 0755\n") != 0) {
			fprintf(stderr, "FAIL: could not publish the badbuild recipe\n");
			ok = 0;
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
				        "FAIL: #101 a recipe whose build step failed reported stage '%s', expected "
				        "build\n",
				        kind != NULL ? kind : "(null)");
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
	 * ADR-0324: a {{REPO_TOKEN}} fetch carries the token of the source
	 * that OWNS the package, and no other source's. Two sources hold two
	 * tokens; the fixture server answers a /private/<credential>/ path
	 * only to a request whose Authorization is exactly that credential,
	 * as a private repository does. Both recipes fetch the same URL,
	 * whose credential is the first source's, and both end in `false`
	 * -- so the STAGE a failure reports (#101) says whether the fetch
	 * got through: "build" when it did, "fetch" when it was refused.
	 * Published before any sync could see them, so ownership comes from
	 * the source named at publish time, which is exactly the case that
	 * had no token at all before ownership was recorded there.
	 */
	{
		static const char *const toks[][2] = { { "tka", "ownertoken" },
		                                       { "tkb", "othertoken" } };
		const char *plain = test_http_src(tarball_path);
		const char *host = strstr(plain, "://") != NULL ? strstr(plain, "://") + 3 : plain;
		const char *path = strchr(host, '/');
		char cred[64], url[1024], content[4096], post[256];
		char *body;
		size_t i;

		for (i = 0; i < 2; i++) {
			snprintf(post, sizeof(post),
			         "{\"name\":\"%s\",\"url\":\"http://127.0.0.1:1/o/%s\",\"kind\":\"gitea\","
			         "\"token\":\"%s\"}",
			         toks[i][0], toks[i][0], toks[i][1]);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/sources", post, &r) != 0 ||
			    r.status < 200 || r.status >= 300) {
				fprintf(stderr, "FAIL: ADR-0324 adding source %s, status=%d\n", toks[i][0],
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);
		}

		/* "u:ownertoken" encodes with no '/', so it is one path segment. */
		if (base64_encode((const unsigned char *)"u:ownertoken", 12, cred, sizeof(cred)) < 0 ||
		    path == NULL) {
			fprintf(stderr, "FAIL: ADR-0324 could not build the private URL\n");
			ok = 0;
			cred[0] = '\0';
		}
		snprintf(url, sizeof(url), "http://u:{{REPO_TOKEN}}@%.*s/private/%s%s",
		         path != NULL ? (int)(path - host) : 0, host, cred, path != NULL ? path : "");

		/* Several sources, none named, nobody owns it: refused, naming both. */
		cpdl_recipe_text(content, sizeof(content), "tokorphan", "1.0", url, sha256, NULL,
		                 CPDL_STD_TOOLS, NULL, "        run \"false\" {\n        }\n",
		                 "        mkdir \"${dest}/usr/share\" chmod 0755\n");
		body = cpdl_publish_body("tokorphan", content, NULL);
		memset(&r, 0, sizeof(r));
		if (body == NULL ||
		    cix_client_request(&client, "POST", "/v1/pkg/recipes", body, &r) != 0 ||
		    r.status != 409 || r.body == NULL || strstr(r.body, "2 sources") == NULL) {
			fprintf(stderr, "FAIL: ADR-0324 a new package with two sources and none named "
			                "must be 409 saying so, got %d: %s\n",
			        r.status, r.body != NULL ? r.body : "");
			ok = 0;
		}
		cix_response_free(&r);
		free(body);

		for (i = 0; i < 2; i++) {
			const char *pkg = i == 0 ? "tokown" : "tokother";
			const char *want = i == 0 ? "build" : "fetch";
			char gpath[64];
			const char *stage;

			cpdl_recipe_text(content, sizeof(content), pkg, "1.0", url, sha256, NULL,
			                 CPDL_STD_TOOLS, NULL, "        run \"false\" {\n        }\n",
			                 "        mkdir \"${dest}/usr/share\" chmod 0755\n");
			if (publish_cpdl_content_from(&client, pkg, "1.0", content, toks[i][0]) != 0) {
				fprintf(stderr, "FAIL: ADR-0324 publishing %s under %s\n", pkg, toks[i][0]);
				ok = 0;
				continue;
			}
			snprintf(post, sizeof(post), "{\"name\":\"%s\"}", pkg);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "POST", "/v1/pkg/install", post, &r);
			cix_response_free(&r);
			poll_pkg_state(&client, pkg, state, sizeof(state), 90);
			snprintf(gpath, sizeof(gpath), "/v1/pkg/%s", pkg);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "GET", gpath, NULL, &r);
			stage = r.json != NULL ? json_str_field(r.json, "stage") : NULL;
			if (strcmp(state, "failed") != 0 || stage == NULL || strcmp(stage, want) != 0) {
				fprintf(stderr, "FAIL: ADR-0324 %s (owned by %s) ended %s at stage %s, "
				                "expected failed at %s -- %s\n",
				        pkg, toks[i][0], state, stage != NULL ? stage : "(none)", want,
				        i == 0 ? "the owner's token did not reach the fetch"
				               : "another source's token was spent on it");
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", gpath, NULL, &r);
			cix_response_free(&r);
		}

		/* The publish recorded the owner, and a named source wins. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/source-ownership", NULL, &r) != 0 ||
		    r.body == NULL || strstr(r.body, "package:tokown") == NULL ||
		    strstr(r.body, "package:tokother") == NULL) {
			fprintf(stderr, "FAIL: ADR-0324 publishing with a source must record it as the "
			                "owner: %s\n",
			        r.body != NULL ? r.body : "");
			ok = 0;
		}
		cix_response_free(&r);
		cpdl_recipe_text(content, sizeof(content), "tokown", "1.1", url, sha256, NULL,
		                 CPDL_STD_TOOLS, NULL, "        run \"false\" {\n        }\n",
		                 "        mkdir \"${dest}/usr/share\" chmod 0755\n");
		body = cpdl_publish_body("tokown", content, "tkb");
		memset(&r, 0, sizeof(r));
		if (body == NULL ||
		    cix_client_request(&client, "POST", "/v1/pkg/recipes", body, &r) != 0 ||
		    r.status != 409 || r.body == NULL ||
		    strstr(r.body, "belongs to source tka") == NULL) {
			fprintf(stderr, "FAIL: ADR-0324 publishing tka's package under tkb must be 409 "
			                "naming tka, got %d: %s\n",
			        r.status, r.body != NULL ? r.body : "");
			ok = 0;
		}
		cix_response_free(&r);
		free(body);

		/* Removing the sources drops their choices; later scenarios
		 * start from a host with none. */
		for (i = 0; i < 2; i++) {
			snprintf(post, sizeof(post), "/v1/pkg/sources/%s", toks[i][0]);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", post, NULL, &r);
			cix_response_free(&r);
		}
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

		/* cix#558: memory_max and its ceiling travel together -- a ceiling below
		 * memory_max is refused, and 4GiB is above the 2GiB default ceiling. */
		snprintf(restore, sizeof(restore),
		         "{\"cpu_max\":\"150000 100000\",\"memory_max\":4294967296,"
		         "\"memory_max_ceiling\":4294967296}");

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
		if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config", restore, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #85 could not set the post-test build budget, status=%d\n",
			        r.status);
			ok = 0;
		}
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
	if (write_builddeps_recipe(&client, "declaredmissing", "1.0", tarball_path, sha256,
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
	/* The floor is named here rather than added by the writer: this
	 * fixture's build must actually RUN, and without `bash` the build
	 * container's PID 1 cannot start the phase at all (cbs#224). The
	 * declaration under test is `greeter`; the rest is what compiling
	 * one hello.c needs. */
	if (write_builddeps_recipe(&client, "declaredpresent", "1.0", tarball_path, sha256,
	                            "greeter tcc linux-headers bash coreutils binutils") != 0) {
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
	/* `greeter@1.0-1` is the pin under test -- greeter is published by
	 * write_recipe() at version 1.0 release 1, so that is its real
	 * identity. The floor beside it is what the build needs to run at
	 * all, same reason as declaredpresent above. */
	if (write_builddeps_recipe(&client, "pinnedgood", "1.0", tarball_path, sha256,
	                            "greeter@1.0-1 tcc linux-headers bash coreutils binutils") != 0 ||
	    write_builddeps_recipe(&client, "pinnedbad", "1.0", tarball_path, sha256, "greeter@9.9") != 0) {
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
		/*
		 * The end state, asserted -- which this case did not do until
		 * now, and the omission hid a real failure.
		 *
		 * It settled for ruling out two specific error strings, so a
		 * build that failed for any OTHER reason counted as a pass.
		 * Measured on 192.168.15.95, 2026-09-26: converting this
		 * fixture to CPDL made its build fail with `CPDL-E3004:
		 * invalid dependency name` -- the test floor's cbs predates
		 * version-pinned tools -- and the run reported no failure at
		 * all. The whole point of this case is that a pin to an
		 * INSTALLED version resolves and the package builds, so
		 * "installed" is the claim and it belongs here.
		 */
		if (strcmp(state, "installed") != 0) {
			fprintf(stderr, "FAIL: #127 correctly-pinned install ended '%s', expected "
			                "installed\n",
			        state);
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pkg/pinnedgood", NULL, &r) == 0 &&
		    r.status == 200) {
			const char *err = json_str_field(r.json, "error");

			/* Kept alongside the state check, not replaced by it: this
			 * names the specific cause the case was written for, and a
			 * named cause is worth more than "not installed". */
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
	if (write_builddeps_recipe(&client, "selfdep", "1.0", tarball_path, sha256,
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
		if (write_builddeps_recipe(&client, "selfdep", "2.0", tarball_path, sha256,
		                           "selfdep tcc linux-headers bash coreutils binutils") != 0) {
			fprintf(stderr, "FAIL: could not write the self-declaring upgrade recipe\n");
			ok = 0;
		}
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/install",
		                       "{\"name\":\"selfdep\",\"version\":\"2.0-1\",\"upgrade\":true}",
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

	/*
	 * #513: the error carries the FULL computed hash -- the one value
	 * needed to re-pin a recipe -- rather than a 12-character prefix
	 * and a pointer elsewhere.
	 */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/pkg/badsum", NULL, &r) == 0 && r.status == 200) {
		const char *err = json_str_field(r.json, "error");
		const char *at = err != NULL ? strstr(err, "hash to ") : NULL;
		int hex = 0;

		if (at != NULL) {
			at += 8;
			while (hex < 64 && ((at[hex] >= '0' && at[hex] <= '9') ||
			                    (at[hex] >= 'a' && at[hex] <= 'f')))
				hex++;
		}
		if (hex != 64 || at[64] != ',') {
			fprintf(stderr, "FAIL: #513 badsum's error does not carry the full computed hash: %s\n",
			        err != NULL ? err : "(none)");
			ok = 0;
		}
	} else {
		fprintf(stderr, "FAIL: #513 GET /v1/pkg/badsum, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);


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
		if (create_image(&client, "router") != 0)
			ok = 0;
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
		if (create_image(&client, "vnew") != 0 || create_image(&client, "vold") != 0)
			ok = 0;
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
		if (write_observing_recipe(&client, "usesstamped", "1.0", tarball_path, sha256,
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
				if (strcmp(observed, "1.9") != 0) {
					fprintf(stderr,
					        "FAIL: #109 the composed environment held stamped '%s', expected the "
					        "declared 1.9 -- a declaration must pin the version, not resolve to "
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
	 * already does, (b) an extra file was genuinely available to
	 * the build *during* it -- checked by its real byte
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
		} else if (write_multisrc_recipe(&client, "multisrc", "1.0", ms_tarball, ms_tarball_sha, extra1_path,
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
		if (write_multisrc_recipe(&client, "multisrcbad", "1.0", ms_tarball, ms_tarball_sha, extra1_path,
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
			if (write_multisrc_recipe(&client, "multisrcquery", "1.0", ms_tarball, ms_tarball_sha,
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
					FILE *cf = fopen(base_path("/usr/share/multisrcquery/extra1.txt"), "r");

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
		char api_srcdir[160];
		char api_build[512], api_install[768];
		char body[4096];
		char body2[4096];
		struct json_writer w;

		if (stage_fixture_tarball(scratch_dir, "apirecipe", "1.0", api_tarball,
		                           sizeof(api_tarball), api_sha, sizeof(api_sha)) != 0) {
			fprintf(stderr, "FAIL: could not stage apirecipe fixture\n");
			ok = 0;
			goto skip_recipe_api;
		}
		fixture_srcdir(api_tarball, api_srcdir, sizeof(api_srcdir));
		snprintf(api_build, sizeof(api_build),
		         "        cd \"${src}/apirecipe/%s\" {\n"
		         "            run \"tcc\" {\n"
		         "                \"-o\" \"hello\" \"hello.c\"\n"
		         "            }\n"
		         "        }\n",
		         api_srcdir);
		snprintf(api_install, sizeof(api_install),
		         "        mkdir \"${dest}/usr/bin\" chmod 0755\n"
		         "        copy \"${src}/apirecipe/%s/hello\" to \"${dest}/usr/bin/apirecipe\"\n",
		         api_srcdir);
		cpdl_recipe_text(body, sizeof(body), "apirecipe", "1.0", test_http_src(api_tarball), api_sha, NULL,
		                 CPDL_STD_TOOLS, NULL, api_build, api_install);

		/* name/declared-name mismatch -> 400, nothing written */
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "wrongname");
		jw_key(&w, "content");
		jw_str(&w, body);
		jw_key(&w, "format");
		jw_str(&w, "cbs");
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
		 * A 409 that is NOT the immutability 409: this version is not
		 * published, and saying it is sends the author looking for a
		 * recipe that does not exist.
		 *
		 * THE COLLIDING PAIR IS NECESSARILY MIXED-LANGUAGE NOW, and
		 * that is not a weaker test than the two shell recipes this
		 * used to publish -- it is the only form of the collision
		 * that is still reachable. A CPDL version always carries a
		 * release, so the release-less `9.9.9` half simply cannot be
		 * written in CPDL; and under ADR-0309 clause 4 a new shell
		 * revision cannot be published at all. What remains, and what
		 * a real host will actually hit, is a NEW CPDL revision
		 * landing on the artifact name an OLD shell revision already
		 * owns -- 65 of 181 installed versions here are still shell,
		 * so there is a large supply of old halves.
		 *
		 * Hence: seed the shell side into the store the way it got
		 * there historically, then publish the CPDL side and require
		 * the refusal. Both spellings of the seeded version are
		 * covered, because `collide0-9.9.9-1` is the artifact name
		 * whether the recipe said `9.9.9` or `9.9.9-1`, and that
		 * equivalence is the whole bug.
		 */
		{
			char coll[2048];
			char coll_name[32];
			int i;
			/* what the stored shell recipe says its version is; the
			 * CPDL recipe published against it is always the bare
			 * form, which release 1 makes equal to the `-1` form. */
			static const char *const seeded[] = { "9.9.9-1", "8.8.8" };
			static const char *const publish_as[] = { "9.9.9", "8.8.8" };

			for (i = 0; i < 2; i++) {
				snprintf(coll_name, sizeof(coll_name), "collide%d", i);
				snprintf(coll, sizeof(coll),
				         "pkg_name=%s\npkg_version=%s\n"
				         "pkg_source=%s\npkg_sha256=%s\n"
				         "pkg_depends=\"\"\npkg_build_depends=\"\"\n"
				         "pkg_build() { :; }\n"
				         "pkg_install() { mkdir -p \"$PKG_DESTDIR/usr/bin\"; "
				         ": > \"$PKG_DESTDIR/usr/bin/collide\"; }\n",
				         coll_name, seeded[i], test_http_src(api_tarball), api_sha);
				if (test_seed_shell_recipe(g_pkg_state_dir, coll_name, seeded[i], coll) != 0) {
					fprintf(stderr, "FAIL: #494 could not seed %s@%s\n", coll_name, seeded[i]);
					ok = 0;
					continue;
				}

				cpdl_recipe_text(coll, sizeof(coll), coll_name, publish_as[i], test_http_src(api_tarball), api_sha,
				                 NULL, CPDL_STD_TOOLS, NULL, "        run \"true\" {\n        }\n",
				                 "        mkdir \"${dest}/usr/bin\" chmod 0755\n"
				                 "        write \"${dest}/usr/bin/collide\" \"x\\n\"\n");
				jw_init(&w);
				jw_obj_open(&w);
				jw_key(&w, "name");
				jw_str(&w, coll_name);
				jw_key(&w, "content");
				jw_str(&w, coll);
				jw_key(&w, "format");
				jw_str(&w, "cbs");
				jw_obj_close(&w);
				w.buf[w.len] = '\0';
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0) {
					fprintf(stderr, "FAIL: #494 publish request failed\n");
					ok = 0;
				} else if (r.status != 409) {
					fprintf(stderr,
					        "FAIL: #494 publishing %s@%s over a stored shell %s status=%d, want "
					        "409 -- both resolve to the artifact %s-%s-1. A 400 here is the "
					        "publish refusing the CPDL document itself, which is a different "
					        "bug from the collision going undetected: read the daemon log for "
					        "the cbs explain diagnostic before touching the collision check\n",
					        coll_name, publish_as[i], seeded[i], r.status, coll_name,
					        publish_as[i]);
					ok = 0;
				}
				cix_response_free(&r);
				jw_free(&w);
			}
		}

		/*
		 * Outright malformed content -> 400. A CPDL document with a
		 * package block and nothing else: no sources, no build, no
		 * install. `cbs explain` rejects it, which is the parser this
		 * format names, and the point of the case is that the publish
		 * validates with that parser rather than storing first and
		 * discovering the problem at build time.
		 */
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "malformed");
		jw_key(&w, "content");
		jw_str(&w, "package \"malformed\" {\n    version \"1.0\"\n");
		jw_key(&w, "format");
		jw_str(&w, "cbs");
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
		jw_key(&w, "format");
		jw_str(&w, "cbs");
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
		 * version) and both versions must coexist afterward. */
		{
			cpdl_recipe_text(body2, sizeof(body2), "apirecipe", "2.0", test_http_src(api_tarball), api_sha, NULL,
			                 CPDL_STD_TOOLS, NULL, api_build, api_install);
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "apirecipe");
			jw_key(&w, "content");
			jw_str(&w, body2);
			jw_key(&w, "format");
			jw_str(&w, "cbs");
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
				if (str_eq(json_str_field(item, "version"), "1.0-1"))
					found_v1 = 1;
				else if (str_eq(json_str_field(item, "version"), "2.0-1"))
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
		           !str_eq(json_str_field(r.json, "version"), "2.0-1") ||
		           !str_eq(json_str_field(r.json, "content"), body2)) {
			fprintf(stderr, "FAIL: GET recipe content mismatch after upsert\n");
			ok = 0;
		}
		cix_response_free(&r);

		/*
		 * #502: the endpoint must not serve a credential, and NOT
		 * ONLY the one the daemon happens to hold.
		 *
		 * redact_repo_token() substitutes exactly one string --
		 * the single repo token of the time -- so a recipe carrying
		 * `user:password@` basic auth matched nothing and
		 * `pkg recipe show` served it in full. It then reached git,
		 * in a repository whose whole point is being readable.
		 *
		 * The secret here is deliberately NOT the configured token:
		 * a test using that one would pass against the old code and
		 * prove nothing. What is asserted is the shape-based
		 * redaction, which is the guarantee -- a credential is
		 * recognisable by being userinfo, not by equality with a
		 * string this host knows.
		 */
		{
			char cred_body[4096];
			char cred_url[512];
			const char *served;
			static const char cred_secret[] = "n0t-the-repo-token-pa55";

			/*
			 * Assembled from pieces rather than written as one
			 * literal, so this file does not itself contain a
			 * URL of the userinfo form. test_secrets scans the
			 * tree for exactly that shape and cannot tell a test
			 * fixture from a live credential -- nor should it
			 * try, since a scanner that judges intent is one
			 * that misses real ones. The string this builds is
			 * the thing under test; the source stays clean.
			 */
			snprintf(cred_url, sizeof(cred_url), "https://someuser%c%s%c%s", ':',
			         cred_secret, '@', "example.invalid/x.tar.gz");
			cpdl_recipe_text(cred_body, sizeof(cred_body), "credrecipe", "1.0", cred_url,
			                 api_sha, NULL, CPDL_STD_TOOLS, NULL, api_build, api_install);
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "credrecipe");
			jw_key(&w, "content");
			jw_str(&w, cred_body);
			jw_key(&w, "format");
			jw_str(&w, "cbs");
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
			    r.status != 204) {
				fprintf(stderr, "FAIL: publish credrecipe, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/recipes/credrecipe", NULL, &r) !=
			        0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: GET credrecipe, status=%d\n", r.status);
				ok = 0;
			} else {
				served = json_str_field(r.json, "content");
				if (served == NULL) {
					fprintf(stderr, "FAIL: credrecipe served no content\n");
					ok = 0;
				} else if (strstr(served, cred_secret) != NULL) {
					fprintf(stderr,
					        "FAIL: GET /v1/pkg/recipes/credrecipe served the credential "
					        "(#502) -- the userinfo of a source url must be redacted "
					        "whether or not it is the configured token\n");
					ok = 0;
				} else if (strstr(served, "REDACTED") == NULL) {
					fprintf(stderr,
					        "FAIL: credrecipe's userinfo was neither served nor redacted "
					        "-- expected REDACTED in the content\n");
					ok = 0;
				}
			}
			cix_response_free(&r);
		}

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
			/*
			 * Publishing 2.0 is the ONLY action this scenario takes:
			 * no install and no upgrade request follows, so whatever
			 * happens next is the daemon's own rolling-rebuild
			 * trigger rather than the test driving it.
			 *
			 * It goes through write_recipe(), which is what published
			 * 1.0 a few lines above, rather than a hand-rolled POST
			 * of shell text -- both because a new shell revision is
			 * refused now (ADR-0309 clause 4) and because the two
			 * versions of one fixture had no business being written
			 * in two different languages by two different code paths.
			 */
			if (write_recipe(&client, "rollpkg", "2.0", tarball2, sha2, NULL) != 0) {
				fprintf(stderr, "FAIL: publish rollpkg 2.0\n");
				ok = 0;
			}

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
					    strcmp(ver, "2.0-1") == 0) {
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
			    !str_eq(json_str_field(r.json, "version"), "2.0-1")) {
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
	 * #555: a package a publish rebuilds in several images is built once.
	 *
	 * The rolling drain starts one queued image per pass while any slot
	 * is free, and it held back only a second job for the same image.
	 * So publishing glibc@2.44-20 on 192.168.15.95 started six identical
	 * glibc builds at once, one per image tracking it. Now an image
	 * whose next package is already building elsewhere waits, and then
	 * installs that build's cached artifact. Two images track dupbuild
	 * as rolling; publishing 2.0 must rebuild both, with exactly one
	 * non-empty build log for dupbuild 2.0-1 -- a cached install writes
	 * an empty one.
	 */
	{
		static const char *const dimgs[] = { "dupimg1", "dupimg2" };
		char dtar1[512], dsha1[128], dtar2[512], dsha2[128];
		char dpath[128], dstate[64];
		int dok = 1, d, w, built = 0;

		dok = stage_fixture_tarball(scratch_dir, "dupbuild", "1.0", dtar1, sizeof(dtar1), dsha1,
		                            sizeof(dsha1)) == 0 &&
		      stage_fixture_tarball(scratch_dir, "dupbuild", "2.0", dtar2, sizeof(dtar2), dsha2,
		                            sizeof(dsha2)) == 0 &&
		      write_recipe(&client, "dupbuild", "1.0", dtar1, dsha1, NULL) == 0;
		for (d = 0; dok && d < 2; d++) {
			char body[128];

			dok = create_image(&client, dimgs[d]) == 0;
			snprintf(body, sizeof(body), "{\"name\":\"dupbuild\",\"image\":\"%s\"}", dimgs[d]);
			memset(&r, 0, sizeof(r));
			if (dok && (cix_client_request(&client, "POST", "/v1/pkg/install", body, &r) != 0 ||
			            (r.status != 202 && r.status != 200)))
				dok = 0;
			cix_response_free(&r);
			snprintf(dpath, sizeof(dpath), "dupbuild@%s", dimgs[d]);
			if (dok && (poll_pkg_state(&client, dpath, dstate, sizeof(dstate), 240) != 0 ||
			            !str_eq(dstate, "installed")))
				dok = 0;
			snprintf(dpath, sizeof(dpath), "/v1/images/%s/manifest", dimgs[d]);
			memset(&r, 0, sizeof(r));
			if (dok && (cix_client_request(&client, "POST", dpath,
			                               "{\"package\":\"dupbuild\",\"mode\":\"rolling\","
			                               "\"version\":\"1.0\"}",
			                               &r) != 0 ||
			            r.status != 204))
				dok = 0;
			cix_response_free(&r);
		}
		if (!dok) {
			fprintf(stderr, "FAIL: could not set up dupbuild 1.0 in dupimg1 and dupimg2\n");
			ok = 0;
		}
		if (dok && write_recipe(&client, "dupbuild", "2.0", dtar2, dsha2, NULL) != 0) {
			fprintf(stderr, "FAIL: publish dupbuild 2.0\n");
			ok = 0;
			dok = 0;
		}
		/* Both images must reach 2.0-1 by the rolling drain alone. */
		for (d = 0; dok && d < 2; d++) {
			snprintf(dpath, sizeof(dpath), "/v1/pkg/dupbuild@%s", dimgs[d]);
			for (w = 0; w < 400; w++) {
				int done = 0;

				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", dpath, NULL, &r) == 0 && r.status == 200 &&
				    str_eq(json_str_field(r.json, "state"), "installed") &&
				    str_eq(json_str_field(r.json, "version"), "2.0-1"))
					done = 1;
				cix_response_free(&r);
				if (done)
					break;
				usleep(300000);
			}
			if (w == 400) {
				fprintf(stderr, "FAIL: dupbuild@%s was not rebuilt to 2.0-1\n", dimgs[d]);
				ok = 0;
				dok = 0;
			}
		}
		if (dok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/build-logs", NULL, &r) == 0 &&
			    r.status == 200 && r.json != NULL) {
				const struct json_value *logs = json_object_get(r.json, "logs");
				size_t k;

				for (k = 0; logs != NULL && logs->type == JSON_ARRAY && k < logs->u.array.count;
				     k++) {
					const struct json_value *l = logs->u.array.items[k];

					if (str_eq(json_str_field(l, "name"), "dupbuild") &&
					    str_eq(json_str_field(l, "version"), "2.0-1") &&
					    json_as_number(json_object_get(l, "size_bytes")) > 0)
						built++;
				}
			}
			cix_response_free(&r);
			if (built != 1) {
				fprintf(stderr,
				        "FAIL: dupbuild 2.0-1 was built %d time(s) for two images, expected "
				        "once -- the second must install the first's artifact\n",
				        built);
				ok = 0;
			}
		}
	}

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
		char hb_artifact_file[PATH_MAX];
		char hb_state[32];
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

		/* A plain recipe (not stage_fixture_tarball(), no DESTDIR/usr/bin
		 * convention needed -- the install phase below just drops its
		 * output at a fixed, predictable name). */
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
		{
			char hb_srcdir[160];
			char hb_build[512], hb_install[512];

			fixture_srcdir(tarball_path, hb_srcdir, sizeof(hb_srcdir));
			snprintf(hb_build, sizeof(hb_build),
			         "        cd \"${src}/hbtest/%s\" {\n"
			         "            run \"sleep\" {\n"
			         "                \"5\"\n"
			         "            }\n"
			         "            run \"tcc\" {\n"
			         "                \"-o\" \"hello\" \"hello.c\"\n"
			         "            }\n"
			         "        }\n",
			         hb_srcdir);
			snprintf(hb_install, sizeof(hb_install),
			         "        copy \"${src}/hbtest/%s/hello\" to \"${dest}/hello\"\n", hb_srcdir);
			if (publish_cpdl_recipe(&client, "hbtest", "1.0", tarball_path, sha256, NULL,
			                         CPDL_STD_TOOLS, NULL, hb_build, hb_install) != 0) {
				fprintf(stderr, "FAIL: could not publish the hbtest recipe\n");
				ok = 0;
				goto skip_hostbuild;
			}
		}

		/* start it -> 202, fetching */
		/*
		 * Issue #200: configure artifact publishing BEFORE the build,
		 * because the tarball a hostbuild must leave behind is built at
		 * build completion.
		 *
		 * It is only built when publishing is configured --
		 * pkg_artifact_publish_resolve() refuses outright with no
		 * repository marked push, and tarring a whole installed tree with nowhere to
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
		if (cix_client_request(&client, "POST", "/v1/pkg/repositories",
		                       "{\"name\":\"unreachable\",\"url\":\"http://127.0.0.1:9/artifacts\","
		                       "\"push\":true}",
		                       &r) != 0 ||
		    r.status != 201) {
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

			/* 1.0-1, not 1.0: hbtest's recipe is CPDL, and a CPDL
			 * version always carries its release.
			 *
			 * And `.cixpkg`, which it was not until #528. The suffix
			 * used to be stuck at .tar.gz because
			 * publish_hostbuild_artifact() named the destination
			 * through pkg_artifact_cache_path() ->
			 * cache_artifact_path_existing(), a reader whose fallback
			 * when nothing exists is PKG_ARTIFACT_FORMAT_TARGZ -- and
			 * it only ever runs when nothing exists, so the recipe's
			 * declared format never reached the call. Converting this
			 * fixture moved the version and left the suffix, which is
			 * precisely what that issue described. Both move now. */
			snprintf(hb_tarball, sizeof(hb_tarball), "%s/cache/hbtest-1.0-1.cixpkg",
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
				char v11_srcdir[160];
				char v11_build[512], v11_install[512];
				char last_hb_body[256];
				int last_hb_status;
				int started = 0;

				fixture_srcdir(tarball_path, v11_srcdir, sizeof(v11_srcdir));
				snprintf(v11_build, sizeof(v11_build),
				         "        cd \"${src}/hbtest/%s\" {\n"
				         "            run \"tcc\" {\n"
				         "                \"-o\" \"hello\" \"hello.c\"\n"
				         "            }\n"
				         "        }\n",
				         v11_srcdir);
				snprintf(v11_install, sizeof(v11_install),
				         "        copy \"${src}/hbtest/%s/hello\" to \"${dest}/hello\"\n",
				         v11_srcdir);
				if (publish_cpdl_recipe(&client, "hbtest", "1.1", tarball_path, sha256, NULL,
				                         CPDL_STD_TOOLS, NULL, v11_build, v11_install) != 0) {
					fprintf(stderr, "FAIL: could not publish the hbtest 1.1 recipe\n");
					ok = 0;
				}

				/* 409 can also mean an earlier job is still
				 * draining -- retry rather than race it.
				 *
				 * The last response is kept so a failure can name
				 * its cause. It used to be discarded, and the
				 * resulting "never started" is what a 404 for a
				 * version that does not exist looked like -- a
				 * whole release cycle spent on a message that
				 * described the symptom and hid the reason. */
				last_hb_status = -1;
				last_hb_body[0] = '\0';
				for (i = 0; ok && i < 100; i++) {
					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "POST", "/v1/pkg/hostbuild",
					                        /* 1.1-1: publish_cpdl_recipe() emits
					                         * release 1, so the recipe this asks
					                         * the daemon to build is stored under
					                         * the release-carrying version. Asking
					                         * for "1.1" finds no recipe, and the
					                         * 100-retry loop below reports that as
					                         * "never started". */
					                        "{\"name\":\"hbtest\","
					                        "\"version\":\"1.1-1\",\"upgrade\":true}",
					                        &r) == 0 &&
					    r.status == 202) {
						cix_response_free(&r);
						started = 1;
						break;
					}
					last_hb_status = r.status;
					snprintf(last_hb_body, sizeof(last_hb_body), "%s",
					         r.body != NULL ? r.body : "(null)");
					cix_response_free(&r);
					usleep(300000);
				}
				if (ok && !started) {
					fprintf(stderr,
					        "FAIL: hbtest 1.1 hostbuild never started, last status=%d body=%s\n",
					        last_hb_status, last_hb_body);
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
			char hd_srcdir[160];
			char hd_build[512], hd_install[512];

			fixture_srcdir(tarball_path, hd_srcdir, sizeof(hd_srcdir));
			snprintf(hd_build, sizeof(hd_build),
			         "        cd \"${src}/hbdepstest/%s\" {\n"
			         "            run \"tcc\" {\n"
			         "                \"-o\" \"hello\" \"hello.c\"\n"
			         "            }\n"
			         "        }\n",
			         hd_srcdir);
			snprintf(hd_install, sizeof(hd_install),
			         "        copy \"${src}/hbdepstest/%s/hello\" to \"${dest}/hello\"\n",
			         hd_srcdir);
			/* The runtime dependency is this fixture's subject -- a
			 * hostbuild whose recipe declares one must still be
			 * accepted -- so it is declared, not dropped. */
			if (publish_cpdl_recipe(&client, "hbdepstest", "1.0", tarball_path, sha256, NULL,
			                         CPDL_STD_TOOLS,
			                         "        runtime {\n"
			                         "            package \"nosuchdep\"\n"
			                         "        }\n",
			                         hd_build, hd_install) != 0) {
				fprintf(stderr, "FAIL: could not publish the hbdepstest recipe\n");
				ok = 0;
				goto skip_hostbuild;
			}
		}

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
		char kept_name[64];
		char kf_container_path[300];

		/* A real, valid source/checksum -- fetch succeeds and a real
		 * build container spawns, extracting the source before the
		 * build phase deliberately fails, distinguishing this from
		 * badsum's own fetch-stage-only failure (step 7). */
		if (publish_cpdl_recipe(&client, "keepfail", "1.0", tarball_path, sha256, NULL,
		                         CPDL_STD_TOOLS, NULL, "        run \"false\" {\n        }\n",
		                         "        mkdir \"${dest}/usr/bin\" chmod 0755\n") != 0) {
			fprintf(stderr, "FAIL: could not publish the keepfail recipe\n");
			ok = 0;
			goto skip_keep_on_failure;
		}

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
		char kept_name[64];
		char rs_container_path[300];

		/*
		 * Leaves a marker, then fails. 1.1 below checks the marker is
		 * still there, which is the whole proof that a resume reuses
		 * the kept container rather than quietly starting over.
		 *
		 * `${build}` rather than the shell form's literal
		 * /build/src: CPDL's workspace is not the shell path's (the
		 * daemon puts it under /build/cbsws), so the variable is the
		 * only spelling that is right in both, and `${build}` sits
		 * outside any source tree where a re-extract could clobber
		 * it.
		 */
		if (publish_cpdl_recipe(&client, "resumeme", "1.0", tarball_path, sha256, NULL,
		                         CPDL_STD_TOOLS, NULL,
		                         "        run \"touch\" {\n"
		                         "            \"${build}/.resumed_marker\"\n"
		                         "        }\n"
		                         "        run \"false\" {\n        }\n",
		                         "        mkdir \"${dest}/usr/bin\" chmod 0755\n") != 0) {
			fprintf(stderr, "FAIL: could not publish the resumeme 1.0 recipe\n");
			ok = 0;
			goto skip_resume;
		}

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
		/* /build/cbsws/build/.resumed_marker -- where the recipe's
		 * `${build}` actually is. The shell form wrote it under
		 * /build/src; cixd stages cbs's workspace at /build/cbsws
		 * (PKG_CBS_WORKSPACE) and cbs puts ${build}, ${src} and
		 * ${dest} beneath it, so this path moved with the recipe.
		 * MEASURED on 192.168.15.95, 2026-09-26, by
		 * recipes/package/probe-cpdlpaths@1, which echoes all three:
		 * build=/build/cbsws/build, src=/build/cbsws/src,
		 * dest=/build/cbsws/dest. It was guessed as /build twice
		 * before that, and 404'd twice. */
		snprintf(rs_container_path, sizeof(rs_container_path),
		         "/v1/containers/%s/files?path=%%2Fbuild%%2Fcbsws%%2Fbuild%%2F.resumed_marker",
		         kept_name);
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
		/* `test -f` in place of the shell form's `[ -f ... ] || exit 1`:
		 * coreutils' test is an ordinary program, and a nonzero exit
		 * fails the phase, which is exactly the assertion. */
		if (publish_cpdl_recipe(&client, "resumeme", "1.1", tarball_path, sha256, NULL,
		                         CPDL_STD_TOOLS, NULL,
		                         "        run \"test\" {\n"
		                         "            \"-f\" \"${build}/.resumed_marker\"\n"
		                         "        }\n",
		                         "        mkdir \"${dest}/usr/share\" chmod 0755\n"
		                         "        mkdir \"${dest}/usr/share/resumeme\" chmod 0755\n"
		                         "        write \"${dest}/usr/share/resumeme/stamp\" "
		                         "\"resumed\\n\"\n") != 0) {
			fprintf(stderr, "FAIL: could not publish the resumeme 1.1 recipe\n");
			ok = 0;
			goto skip_resume;
		}

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

			/* 1.1-1, not 1.1: a CPDL version always carries its
			 * release, and publish_cpdl_recipe() emits release 1. */
			if (ver == NULL || strcmp(ver, "1.1-1") != 0) {
				fprintf(stderr, "FAIL: resumeme (resumed) version='%s', expected 1.1-1\n",
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
	 * #553: a path in an image belongs to exactly one installed package.
	 *
	 * shareda and sharedb each install a file of their own plus the same
	 * shared path. Until 2026-09-30 both installed and whichever went
	 * last held the path, with nothing recording whose bytes those
	 * were -- which is how libcap's recorded libcap.so.2 came to be
	 * coreutils' link and broke every build composed from it (#510).
	 *
	 * So: shareda installs; sharedb is refused, and the error names
	 * shareda and the path; an upgrade of shareda that still carries
	 * the path installs, because its own previous version is not
	 * another package; and deleting shareda removes its files, the
	 * shared one included, since nothing else owns it.
	 *
	 * What #175 tested here -- delete keeping a path another package
	 * also claims -- can no longer be staged through installs. It
	 * remains in unlink_manifest_files() for images that predate the
	 * rule.
	 */
	{
		const char *shared_rel = "usr/share/shared175/common.txt";
		char sha[65], sha2[65];
		char tarball[PATH_MAX], tarball2[PATH_MAX];
		char st[64];
		int stage_ok;

		stage_ok = stage_fixture_tarball(scratch_dir, "shareda", "1.0", tarball,
		                                 sizeof(tarball), sha, sizeof(sha)) == 0 &&
		           stage_fixture_tarball(scratch_dir, "shareda", "1.1", tarball2,
		                                 sizeof(tarball2), sha2, sizeof(sha2)) == 0;
		if (stage_ok &&
		    (write_shared_path_recipe(&client, "shareda", "1.0", tarball, sha, shared_rel, NULL) != 0 ||
		     write_shared_path_recipe(&client, "shareda", "1.1", tarball2, sha2, shared_rel, NULL) !=
		             0 ||
		     write_shared_path_recipe(&client, "sharedb", "1.0", tarball, sha, shared_rel, NULL) != 0)) {
			fprintf(stderr, "FAIL: could not write the shared-path recipes\n");
			ok = 0;
			stage_ok = 0;
		}

		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install",
			                       "{\"name\":\"shareda\",\"version\":\"1.0-1\"}", &r) != 0 ||
			    (r.status != 202 && r.status != 200)) {
				fprintf(stderr, "FAIL: #553 install shareda status=%d\n", r.status);
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
		}
		if (stage_ok && (poll_pkg_state(&client, "shareda", st, sizeof(st), 240) != 0 ||
		                 !str_eq(st, "installed"))) {
			fprintf(stderr, "FAIL: #553 shareda did not install (state=%s)\n", st);
			ok = 0;
			stage_ok = 0;
		}

		/* The collision: refused, naming the owner and the path. */
		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"sharedb\"}",
			                       &r) != 0 ||
			    (r.status != 202 && r.status != 200)) {
				fprintf(stderr, "FAIL: #553 install sharedb status=%d\n", r.status);
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
		}
		if (stage_ok) {
			const char *em;

			if (poll_pkg_state(&client, "sharedb", st, sizeof(st), 240) != 0 ||
			    str_eq(st, "installed")) {
				fprintf(stderr, "FAIL: #553 sharedb installed over a path shareda owns "
				                "(state=%s)\n", st);
				ok = 0;
			}
			memset(&r, 0, sizeof(r));
			em = (cix_client_request(&client, "GET", "/v1/pkg/sharedb", NULL, &r) == 0 &&
			      r.status == 200) ? json_str_field(r.json, "error") : NULL;
			if (em == NULL || strstr(em, "shareda@1.0-1") == NULL ||
			    strstr(em, shared_rel) == NULL) {
				fprintf(stderr, "FAIL: #553 the refusal must name shareda@1.0-1 and %s, got: %s\n",
				        shared_rel, em != NULL ? em : "(no error)");
				ok = 0;
			}
			cix_response_free(&r);
			if (access(base_path("/usr/bin/sharedb"), F_OK) == 0) {
				fprintf(stderr, "FAIL: #553 a refused install left its own file behind\n");
				ok = 0;
			}
		}

		/* Its own next version carrying the same path is not a collision. */
		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install",
			                       "{\"name\":\"shareda\",\"version\":\"1.1-1\",\"upgrade\":true}",
			                       &r) != 0 ||
			    (r.status != 202 && r.status != 200)) {
				fprintf(stderr, "FAIL: #553 upgrade shareda status=%d\n", r.status);
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
		}
		if (stage_ok) {
			if (poll_pkg_state(&client, "shareda", st, sizeof(st), 240) != 0 ||
			    !str_eq(st, "installed")) {
				fprintf(stderr, "FAIL: #553 upgrading shareda over its own path was refused "
				                "(state=%s)\n", st);
				ok = 0;
				stage_ok = 0;
			}
		}

		/*
		 * A path may MOVE when the receiver declares it (cbs#275):
		 * sharedc installs the same shared path with
		 * `replaces { package "shareda" }`, and is allowed. The path then
		 * belongs to sharedc alone, so deleting shareda keeps it, and
		 * deleting sharedc finally takes it.
		 */
		if (stage_ok &&
		    write_shared_path_recipe(&client, "sharedc", "1.0", tarball, sha, shared_rel,
		                             "    replaces {\n        package \"shareda\"\n    }\n\n") !=
		        0) {
			fprintf(stderr, "FAIL: #553 could not publish sharedc (replaces shareda)\n");
			ok = 0;
			stage_ok = 0;
		}
		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"sharedc\"}",
			                       &r) != 0 ||
			    (r.status != 202 && r.status != 200))
				stage_ok = 0;
			cix_response_free(&r);
			if (!stage_ok ||
			    poll_pkg_state(&client, "sharedc", st, sizeof(st), 240) != 0 ||
			    !str_eq(st, "installed")) {
				fprintf(stderr, "FAIL: #553 sharedc, which replaces shareda, did not install "
				                "(state=%s)\n", st);
				ok = 0;
				stage_ok = 0;
			}
		}
		if (stage_ok) {
			static const char *const who[] = { "shareda", "sharedc" };
			int w;

			for (w = 0; w < 2; w++) {
				char path[64];
				int has = 0;

				snprintf(path, sizeof(path), "/v1/pkg/%s", who[w]);
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", path, NULL, &r) == 0 && r.json != NULL) {
					const struct json_value *files = json_object_get(r.json, "files");
					size_t k;

					for (k = 0; files != NULL && files->type == JSON_ARRAY &&
					            k < files->u.array.count; k++)
						if (str_eq(json_as_string(files->u.array.items[k]), shared_rel))
							has = 1;
				}
				cix_response_free(&r);
				if (has != (w == 1)) {
					fprintf(stderr, "FAIL: #553 after the takeover %s %s %s\n", who[w],
					        has ? "still lists" : "does not list", shared_rel);
					ok = 0;
				}
			}
		}
		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", "/v1/pkg/shareda", NULL, &r);
			if (r.status != 204 && r.status != 200) {
				fprintf(stderr, "FAIL: #553 deleting shareda status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
			if (access(base_path("/usr/bin/shareda"), F_OK) == 0) {
				fprintf(stderr, "FAIL: #553 deleting shareda left its own file behind\n");
				ok = 0;
			}
			if (access(base_path("/usr/share/shared175/common.txt"), F_OK) != 0) {
				fprintf(stderr, "FAIL: #553 deleting shareda took a path sharedc now owns\n");
				ok = 0;
			}
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", "/v1/pkg/sharedc", NULL, &r);
			cix_response_free(&r);
			if (access(base_path("/usr/share/shared175/common.txt"), F_OK) == 0) {
				fprintf(stderr, "FAIL: #553 deleting sharedc, the path's owner, left it\n");
				ok = 0;
			}
		}
	}

	/*
	 * #552: a setuid file the recipe declares arrives setuid.
	 *
	 * cbs carries a declared `privileged file` mode in the .cixpkg and
	 * refuses any other setuid entry, but the installer masked every
	 * mode to 0777, so openssh's declared 04711 ssh-keysign reached
	 * jumpbox as 0711 (probe-setuid@1-1 on 192.168.15.95, 2026-09-30).
	 */
	{
		char sha[65];
		char tarball[PATH_MAX];
		char st_s[64];
		struct stat sst;
		int stage_ok;

		stage_ok = stage_fixture_tarball(scratch_dir, "suid552", "1.0", tarball,
		                                 sizeof(tarball), sha, sizeof(sha)) == 0 &&
		           write_suid_recipe(&client, "suid552", "1.0", tarball, sha) == 0;
		if (!stage_ok) {
			fprintf(stderr, "FAIL: #552 could not publish the suid552 recipe\n");
			ok = 0;
		}
		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"suid552\"}",
			                       &r) != 0 ||
			    (r.status != 202 && r.status != 200)) {
				fprintf(stderr, "FAIL: #552 install suid552 status=%d\n", r.status);
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
		}
		if (stage_ok && (poll_pkg_state(&client, "suid552", st_s, sizeof(st_s), 240) != 0 ||
		                 !str_eq(st_s, "installed"))) {
			fprintf(stderr, "FAIL: #552 suid552 did not install (state=%s)\n", st_s);
			ok = 0;
			stage_ok = 0;
		}
		if (stage_ok) {
			if (stat(base_path("/usr/libexec/suid552/helper"), &sst) != 0) {
				fprintf(stderr, "FAIL: #552 helper not installed: %s\n", strerror(errno));
				ok = 0;
			} else if ((sst.st_mode & 07777) != 04711) {
				fprintf(stderr, "FAIL: #552 a declared 04711 file installed as %04o\n",
				        (unsigned)(sst.st_mode & 07777));
				ok = 0;
			}
		}
	}

	/*
	 * cix#558: a recipe declares the memory its whole build needs, and
	 * may not exceed the ceiling the operator pre-allocated.
	 *
	 * node@24.21.0 thrashed under the 2GiB shared budget and finished at
	 * 4GiB (192.168.15.95, 2026-10-01), and nothing anywhere could say
	 * so except recipe prose. Asserted end to end: the pair rule on the
	 * config, a declaration above the ceiling refused before the build
	 * starts and naming both numbers, and a declaration within it
	 * raising the budget for its build and returning it afterwards.
	 */
	{
		char sha[65];
		char tarball[PATH_MAX];
		char st_m[64];
		long long mem = -1, ceil_v = -1, eff = -1;
		int stage_ok = 1;

		/* Left at 4GiB/4GiB by the #85 case above: no raise possible. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/pkg-build-config", NULL, &r) == 0 &&
		    r.status == 200 && r.json != NULL) {
			mem = (long long)json_as_number(json_object_get(r.json, "memory_max"));
			ceil_v = (long long)json_as_number(json_object_get(r.json, "memory_max_ceiling"));
			eff = (long long)json_as_number(json_object_get(r.json, "memory_max_effective"));
		}
		cix_response_free(&r);
		if (mem != 4294967296LL || ceil_v != 4294967296LL || eff != 4294967296LL) {
			fprintf(stderr,
			        "FAIL: #558 build config memory_max=%lld ceiling=%lld effective=%lld, "
			        "expected 4294967296 for all three\n",
			        mem, ceil_v, eff);
			ok = 0;
		}

		/* The pair rule: never a ceiling below memory_max, and an
		 * unlimited memory_max needs an unlimited ceiling. */
		{
			static const char *const refused[] = {
				"{\"memory_max_ceiling\":1}",
				"{\"memory_max\":0}",
				"{\"memory_max\":8589934592}",
				"{\"memory_max_ceiling\":-1}",
			};
			size_t k;

			for (k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config",
				                       refused[k], &r) != 0 ||
				    r.status != 400) {
					fprintf(stderr, "FAIL: #558 PUT %s expected 400, got %d\n", refused[k],
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);
			}
		}

		/* Above the ceiling: refused, naming both numbers. */
		if (stage_fixture_tarball(scratch_dir, "memover", "1.0", tarball, sizeof(tarball), sha,
		                          sizeof(sha)) != 0 ||
		    write_memory_recipe(&client, "memover", "1.0", tarball, sha, "5GiB") != 0) {
			fprintf(stderr, "FAIL: #558 could not publish memover\n");
			ok = 0;
			stage_ok = 0;
		}
		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"memover\"}",
			                       &r) != 0 ||
			    (r.status != 202 && r.status != 200)) {
				fprintf(stderr, "FAIL: #558 install memover status=%d\n", r.status);
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
		}
		if (stage_ok) {
			const char *err = NULL;

			if (poll_pkg_state(&client, "memover", st_m, sizeof(st_m), 240) != 0 ||
			    !str_eq(st_m, "failed")) {
				fprintf(stderr, "FAIL: #558 memover declares 5GiB over a 4GiB ceiling and "
				                "ended '%s', expected failed\n",
				        st_m);
				ok = 0;
			}
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/memover", NULL, &r) == 0 &&
			    r.json != NULL)
				err = json_str_field(r.json, "error");
			if (err == NULL || strstr(err, "5368709120") == NULL ||
			    strstr(err, "memory_max_ceiling of 4294967296") == NULL ||
			    strstr(err, "--memory-max-ceiling=5368709120") == NULL) {
				fprintf(stderr, "FAIL: #558 memover's refusal does not name the need, the "
				                "ceiling and the fix: %s\n",
				        err != NULL ? err : "(none)");
				ok = 0;
			}
			cix_response_free(&r);
		}

		/* Within the ceiling: the budget is raised for the build and
		 * returns to memory_max once nothing is building. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config",
		                       "{\"memory_max\":2147483648,\"memory_max_ceiling\":6442450944}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #558 PUT memory_max 2GiB + ceiling 6GiB, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
		stage_ok = stage_fixture_tarball(scratch_dir, "memraise", "1.0", tarball,
		                                 sizeof(tarball), sha, sizeof(sha)) == 0 &&
		           write_memory_recipe(&client, "memraise", "1.0", tarball, sha, "3GiB") == 0;
		if (!stage_ok) {
			fprintf(stderr, "FAIL: #558 could not publish memraise\n");
			ok = 0;
		}
		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install", "{\"name\":\"memraise\"}",
			                       &r) != 0 ||
			    (r.status != 202 && r.status != 200)) {
				fprintf(stderr, "FAIL: #558 install memraise status=%d\n", r.status);
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
		}
		if (stage_ok && (poll_pkg_state(&client, "memraise", st_m, sizeof(st_m), 240) != 0 ||
		                 !str_eq(st_m, "installed"))) {
			fprintf(stderr, "FAIL: #558 memraise declares 3GiB under a 6GiB ceiling and ended "
			                "'%s', expected installed\n",
			        st_m);
			ok = 0;
			stage_ok = 0;
		}
		if (stage_ok) {
			char busy[512] = "";
			long long jobs = -1;
			int waited;
			const char *log_raise = "pkg memraise: build budget raised to 3221225472 bytes";
			const char *log_back = "build budget back to memory_max 2147483648";

			/*
			 * The raise is held until NO build is running (ADR-0322), so
			 * "memraise is installed" is not yet "the budget is back": any
			 * other build still in flight keeps it. Wait for the slots to
			 * empty first, and if they never do, say what holds them.
			 */
			for (waited = 0; waited < 180; waited++) {
				jobs = -1;
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/system/pkg-build-config", NULL,
				                       &r) == 0 &&
				    r.status == 200 && r.json != NULL) {
					const char *names = json_str_field(r.json, "active_job_names");

					jobs = (long long)json_as_number(json_object_get(r.json, "active_jobs"));
					eff = (long long)json_as_number(
					    json_object_get(r.json, "memory_max_effective"));
					snprintf(busy, sizeof(busy), "%s", names != NULL ? names : "");
				}
				cix_response_free(&r);
				if (jobs == 0)
					break;
				sleep(1);
			}
			if (jobs != 0) {
				fprintf(stderr, "FAIL: #558 builds still in flight 180s after memraise "
				                "installed: %lld (%s)\n",
				        jobs, busy);
				ok = 0;
			} else if (eff != 2147483648LL) {
				fprintf(stderr, "FAIL: #558 with nothing building, memory_max_effective=%lld, "
				                "expected memory_max 2147483648\n",
				        eff);
				ok = 0;
			}

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/system/logs?source=cixd&tail=400", NULL,
			                       &r) != 0 ||
			    r.status != 200 || r.body == NULL || strstr(r.body, log_raise) == NULL ||
			    strstr(r.body, log_back) == NULL) {
				fprintf(stderr, "FAIL: #558 in the daemon log, the raise line is %s and the "
				                "return line is %s\n",
				        r.body != NULL && strstr(r.body, log_raise) != NULL ? "present" : "MISSING",
				        r.body != NULL && strstr(r.body, log_back) != NULL ? "present" : "MISSING");
				ok = 0;
			}
			if (eff != 2147483648LL && r.body != NULL) {
				const char *p = r.body;

				/* Every budget line, and every build start and end the
				 * daemon logged, so the next reader sees what held it. */
				while ((p = strstr(p, "budget")) != NULL) {
					const char *b = p, *end = strchr(p, '\n');

					while (b > r.body && b[-1] != '\n')
						b--;
					fprintf(stderr, "  log: %.*s\n",
					        (int)((end != NULL ? end : p + strlen(p)) - b), b);
					p = end != NULL ? end : p + strlen(p);
				}
			}
			cix_response_free(&r);
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/pkg-build-config",
		                       "{\"memory_max\":4294967296,\"memory_max_ceiling\":4294967296}",
		                       &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #558 could not restore the 4GiB build budget, status=%d\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/*
	 * ADR-0323 and ADR-0324: a recipe is committed to the writable source
	 * that owns its package, then published -- git first.
	 *
	 * The forge is this fixture's stand-in for Gitea's create-file call
	 * (test_http_server_start() with accept_put), so what the daemon sent
	 * is read back byte for byte: the path, the branch, the message, and
	 * the base64 content, decoded and compared with the recipe. Then the
	 * refusals that keep git and the store honest: nothing is committed
	 * until a source is writable, a version already published is refused
	 * before anything reaches git, a version git already has is refused by
	 * the forge and published nowhere, a recipe that would not publish
	 * never reaches git, and a package one source owns is never committed
	 * to another.
	 */
	{
		char forge_dir[PATH_MAX], sha[65], tarball[PATH_MAX], body_path[PATH_MAX];
		char post[512], recipe[4096], st_c[32];
		char *json_body = NULL, *posted = NULL;
		size_t posted_len = 0;
		struct json_writer w;
		pid_t forge_pid = -1;
		int forge_port = 0, waited, stage_ok = 1;

		snprintf(forge_dir, sizeof(forge_dir), "%s/forge", scratch_dir);
		if (mkdir(forge_dir, 0755) != 0 ||
		    test_http_server_start(forge_dir, 1, &forge_port, &forge_pid) != 0) {
			fprintf(stderr, "FAIL: ADR-0324 could not start the fake forge\n");
			ok = 0;
			stage_ok = 0;
		}
		if (stage_ok && (stage_fixture_tarball(scratch_dir, "commitpkg", "1.0", tarball,
		                                       sizeof(tarball), sha, sizeof(sha)) != 0)) {
			fprintf(stderr, "FAIL: ADR-0323 could not stage the commitpkg source\n");
			ok = 0;
			stage_ok = 0;
		}
		if (stage_ok) {
			cpdl_recipe_text_decl(recipe, sizeof(recipe), "commitpkg", "1.0",
			                      test_http_src(tarball), sha, NULL,
			                      "            tool \"bash\"\n"
			                      "            tool \"coreutils\"\n",
			                      NULL, "",
			                      "        run \"true\" {\n        }\n",
			                      "        mkdir \"${dest}/usr/share/commitpkg\" parents\n");
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "commitpkg");
			jw_key(&w, "content");
			jw_str(&w, recipe);
			jw_obj_close(&w);
			json_body = malloc(w.len + 1);
			if (json_body != NULL) {
				memcpy(json_body, w.buf, w.len);
				json_body[w.len] = '\0';
			}
			jw_free(&w);
			if (json_body == NULL)
				stage_ok = 0;
		}

		/* No source is writable: nothing is committed. */
		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipe-commit", json_body, &r) != 0 ||
			    r.status != 409 || r.body == NULL || strstr(r.body, "writable") == NULL) {
				fprintf(stderr, "FAIL: ADR-0324 a commit with no writable source must be 409 "
				                "naming it, got %d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);
		}

		/* A writable gitea source, and what a source may not be. */
		if (stage_ok) {
			snprintf(post, sizeof(post),
			         "{\"name\":\"forge\",\"url\":\"http://127.0.0.1:%d/testowner/cix-recipes\","
			         "\"kind\":\"gitea\",\"ref\":\"main\",\"token\":\"forge-token\",\"write\":true}",
			         forge_port);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/sources", post, &r) != 0 ||
			    r.status != 201 || r.body == NULL || strstr(r.body, "\"write\":true") == NULL ||
			    strstr(r.body, "\"token_set\":true") == NULL ||
			    strstr(r.body, "\"trust_keys\":false") == NULL ||
			    strstr(r.body, "forge-token") != NULL) {
				fprintf(stderr, "FAIL: ADR-0324 POST a writable source, status=%d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
		}
		if (stage_ok) {
			static const struct {
				const char *method, *path, *body;
				int status;
			} refused[] = {
				{ "POST", "/v1/pkg/sources",
				  "{\"name\":\"forge\",\"url\":\"http://x/o/r\",\"kind\":\"gitea\"}", 409 },
				{ "POST", "/v1/pkg/sources",
				  "{\"name\":\"gh\",\"url\":\"https://github.com/o/r\",\"kind\":\"github\","
				  "\"write\":true}",
				  400 },
				{ "POST", "/v1/pkg/sources",
				  "{\"name\":\"Bad Name\",\"url\":\"http://x/o/r\",\"kind\":\"gitea\"}", 400 },
				{ "PUT", "/v1/pkg/sources/forge", "{\"write\":\"yes\"}", 400 },
				{ "PUT", "/v1/pkg/sources/forge", "{\"name\":\"other\"}", 400 },
				{ "PUT", "/v1/pkg/sources/nosuch", "{\"ref\":\"main\"}", 404 },
				{ "DELETE", "/v1/pkg/sources/nosuch", NULL, 404 },
			};
			size_t k;

			for (k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, refused[k].method, refused[k].path,
				                       refused[k].body, &r) != 0 ||
				    r.status != refused[k].status) {
					fprintf(stderr, "FAIL: ADR-0324 %s %s %s expected %d, got %d\n",
					        refused[k].method, refused[k].path,
					        refused[k].body != NULL ? refused[k].body : "", refused[k].status,
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);
			}
		}

		/* The commit itself: forge first, then the store. */
		if (stage_ok) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipe-commit", json_body, &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: ADR-0323 POST recipe-commit expected 202, got %d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
		}
		if (stage_ok) {
			const char *commit = NULL, *path = NULL, *src = NULL;
			int published = 0;

			st_c[0] = '\0';
			for (waited = 0; waited < 120; waited++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/recipe-commit", NULL, &r) == 0 &&
				    r.json != NULL && json_str_field(r.json, "state") != NULL &&
				    strcmp(json_str_field(r.json, "state"), "running") != 0)
					break;
				cix_response_free(&r);
				usleep(500000);
			}
			if (r.json != NULL) {
				snprintf(st_c, sizeof(st_c), "%s",
				         json_str_field(r.json, "state") != NULL ? json_str_field(r.json, "state")
				                                                 : "");
				commit = json_str_field(r.json, "commit");
				path = json_str_field(r.json, "path");
				src = json_str_field(r.json, "source");
				published = json_object_get(r.json, "published") != NULL &&
				            json_object_get(r.json, "published")->u.boolean;
			}
			if (!str_eq(st_c, "done") || commit == NULL ||
			    strcmp(commit, TEST_FORGE_COMMIT_SHA) != 0 || path == NULL ||
			    strcmp(path, "recipes/package/commitpkg@1.0-1.cbs") != 0 || !str_eq(src, "forge") ||
			    !published) {
				fprintf(stderr, "FAIL: ADR-0323 the commit ended %s: %s\n", st_c,
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
		}
		if (stage_ok) {
			struct json_value *sent;
			const char *b64, *branch, *message;
			unsigned char decoded[4096];
			int dn = -1;

			snprintf(body_path, sizeof(body_path), "%s/commitpkg@1.0-1.cbs.request.json",
			         forge_dir);
			sent = slurp_file(body_path, &posted, &posted_len) == 0
			           ? json_parse(posted, posted_len)
			           : NULL;
			branch = sent != NULL ? json_as_string(json_object_get(sent, "branch")) : NULL;
			message = sent != NULL ? json_as_string(json_object_get(sent, "message")) : NULL;
			b64 = sent != NULL ? json_as_string(json_object_get(sent, "content")) : NULL;
			if (b64 != NULL)
				dn = base64_decode(b64, decoded, sizeof(decoded));
			if (branch == NULL || strcmp(branch, "main") != 0 || message == NULL ||
			    strncmp(message, "commitpkg@1.0-1: committed by cixd", 34) != 0 ||
			    dn != (int)strlen(recipe) || memcmp(decoded, recipe, strlen(recipe)) != 0) {
				fprintf(stderr, "FAIL: ADR-0323 the forge did not receive the recipe on main "
				                "with its message: %s\n",
				        posted != NULL ? posted : "(nothing posted)");
				ok = 0;
			}
			json_free(sent);
			free(posted);
			posted = NULL;

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/recipes/commitpkg", NULL, &r) != 0 ||
			    r.status != 200 || !str_eq(json_str_field(r.json, "version"), "1.0-1")) {
				fprintf(stderr, "FAIL: ADR-0323 commitpkg was committed but not published, "
				                "status=%d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);

			/* The same version again: refused before git. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipe-commit", json_body, &r) != 0 ||
			    r.status != 409) {
				fprintf(stderr, "FAIL: ADR-0323 committing a published version again must be "
				                "409, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);

			/* A recipe that would not publish never reaches git. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipe-commit",
			                       "{\"name\":\"commitpkg\",\"content\":\"package {\"}", &r) != 0 ||
			    r.status != 400) {
				fprintf(stderr, "FAIL: ADR-0323 an invalid recipe must be 400 before any "
				                "commit, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);
		}

		/* A version git already has: the forge refuses, nothing publishes. */
		if (stage_ok) {
			char existing[PATH_MAX];
			FILE *fp;
			char *two = NULL;

			snprintf(existing, sizeof(existing), "%s/commitpkg@1.1-1.cbs.request.json",
			         forge_dir);
			fp = fopen(existing, "w");
			if (fp != NULL)
				fclose(fp);
			cpdl_recipe_text_decl(recipe, sizeof(recipe), "commitpkg", "1.1",
			                      test_http_src(tarball), sha, NULL,
			                      "            tool \"bash\"\n"
			                      "            tool \"coreutils\"\n",
			                      NULL, "",
			                      "        run \"true\" {\n        }\n",
			                      "        mkdir \"${dest}/usr/share/commitpkg\" parents\n");
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "commitpkg");
			jw_key(&w, "content");
			jw_str(&w, recipe);
			jw_obj_close(&w);
			two = malloc(w.len + 1);
			if (two != NULL) {
				memcpy(two, w.buf, w.len);
				two[w.len] = '\0';
			}
			jw_free(&w);
			memset(&r, 0, sizeof(r));
			if (two == NULL ||
			    cix_client_request(&client, "POST", "/v1/pkg/recipe-commit", two, &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: ADR-0323 POST commitpkg 1.1 expected 202, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);
			for (waited = 0; waited < 120; waited++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/recipe-commit", NULL, &r) == 0 &&
				    r.json != NULL && json_str_field(r.json, "state") != NULL &&
				    strcmp(json_str_field(r.json, "state"), "running") != 0)
					break;
				cix_response_free(&r);
				usleep(500000);
			}
			if (r.json == NULL || !str_eq(json_str_field(r.json, "state"), "failed") ||
			    json_str_field(r.json, "error") == NULL ||
			    strstr(json_str_field(r.json, "error"), "422") == NULL ||
			    strstr(json_str_field(r.json, "error"), "already exists") == NULL) {
				fprintf(stderr, "FAIL: ADR-0323 a version git already has must fail naming "
				                "the forge's refusal: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/recipes/commitpkg", NULL, &r) != 0 ||
			    !str_eq(json_str_field(r.json, "version"), "1.0-1")) {
				fprintf(stderr, "FAIL: ADR-0323 a commit the forge refused was published\n");
				ok = 0;
			}
			cix_response_free(&r);

			/* commitpkg belongs to forge now; another writable source
			 * may not take it (ADR-0324). */
			snprintf(post, sizeof(post),
			         "{\"name\":\"forge2\",\"url\":\"http://127.0.0.1:%d/other/recipes\","
			         "\"kind\":\"gitea\",\"token\":\"t2\",\"write\":true}",
			         forge_port);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "POST", "/v1/pkg/sources", post, &r);
			cix_response_free(&r);
			if (two != NULL) {
				size_t blen = strlen(two);
				char *withsrc = malloc(blen + 32);

				if (withsrc != NULL) {
					/* Same body, naming forge2: {"name":...,"content":...,"source":"forge2"} */
					snprintf(withsrc, blen + 32, "%.*s,\"source\":\"forge2\"}", (int)(blen - 1),
					         two);
					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "POST", "/v1/pkg/recipe-commit", withsrc,
					                       &r) != 0 ||
					    r.status != 409 || r.body == NULL ||
					    strstr(r.body, "belongs to source forge") == NULL) {
						fprintf(stderr, "FAIL: ADR-0324 committing forge's package to forge2 "
						                "must be 409 naming its owner, got %d: %s\n",
						        r.status, r.body != NULL ? r.body : "");
						ok = 0;
					}
					cix_response_free(&r);
					free(withsrc);
				}
			}
			free(two);
		}

		/* A republish with different content is divergent, not a duplicate. */
		if (stage_ok) {
			size_t blen = strlen(json_body);
			char *same = malloc(blen + 32);
			char *diverge = malloc(blen + 32);
			char *p;

			if (same != NULL && diverge != NULL) {
				/* {"name":...,"content":...} + "format":"cbs" */
				snprintf(same, blen + 32, "%.*s,\"format\":\"cbs\"}", (int)(blen - 1), json_body);
				snprintf(diverge, blen + 32, "%s", same);
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "POST", "/v1/pkg/recipes", same, &r) != 0 ||
				    r.status != 409 || r.body == NULL ||
				    strstr(r.body, "different content") != NULL) {
					fprintf(stderr, "FAIL: ADR-0324 an identical republish must be the plain "
					                "duplicate 409, got %d: %s\n",
					        r.status, r.body != NULL ? r.body : "");
					ok = 0;
				}
				cix_response_free(&r);
				p = strstr(diverge, "usr/share/commitpkg");
				if (p != NULL)
					memcpy(p, "usr/share/commitpkh", 19);
				memset(&r, 0, sizeof(r));
				if (p == NULL ||
				    cix_client_request(&client, "POST", "/v1/pkg/recipes", diverge, &r) != 0 ||
				    r.status != 409 || r.body == NULL ||
				    strstr(r.body, "different content") == NULL) {
					fprintf(stderr, "FAIL: ADR-0324 a republish with different content must be "
					                "the divergent 409, got %d: %s\n",
					        r.status, r.body != NULL ? r.body : "");
					ok = 0;
				}
				cix_response_free(&r);
			}
			free(same);
			free(diverge);
		}

		/*
		 * ADR-0323's author stage: POST /pkg/recipe-revise writes the next
		 * revision with cbs revise and commits it exactly as a commit is.
		 * Refusals first -- no recipe, a malformed digest, no
		 * verification, a version that is not newer -- then a real one,
		 * read back from the forge: the fields that change, the bytes that
		 * must not (the install phase), and the changelog that records how
		 * the digest was established.
		 */
		if (stage_ok) {
			static const struct {
				const char *body;
				int status;
				const char *says;
			} refused[] = {
				{ "{\"name\":\"nosuchpkg\",\"version\":\"2\",\"url\":\"http://x/a.tar.gz\","
				  "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
				  "\"verification\":\"v\"}",
				  404, "never a first one" },
				{ "{\"name\":\"commitpkg\",\"version\":\"1.2\",\"url\":\"http://x/a.tar.gz\","
				  "\"sha256\":\"ABC\",\"verification\":\"v\"}",
				  400, "64 lowercase hex" },
				{ "{\"name\":\"commitpkg\",\"version\":\"1.2\",\"url\":\"http://x/a.tar.gz\","
				  "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}",
				  400, "verification is required" },
				{ "{\"name\":\"commitpkg\",\"version\":\"1.0\",\"url\":\"http://x/a.tar.gz\","
				  "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
				  "\"verification\":\"v\"}",
				  409, "not newer" },
			};
			char rbody[1024], decoded_rev[8192];
			const char *b64;
			struct json_value *sent;
			size_t k;
			int dn = -1;

			for (k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "POST", "/v1/pkg/recipe-revise", refused[k].body,
				                       &r) != 0 ||
				    r.status != refused[k].status || r.body == NULL ||
				    strstr(r.body, refused[k].says) == NULL) {
					fprintf(stderr, "FAIL: ADR-0323 revise %s expected %d naming \"%s\", got "
					                "%d: %s\n",
					        refused[k].body, refused[k].status, refused[k].says, r.status,
					        r.body != NULL ? r.body : "");
					ok = 0;
				}
				cix_response_free(&r);
			}

			snprintf(rbody, sizeof(rbody),
			         "{\"name\":\"commitpkg\",\"version\":\"1.2\",\"url\":\"%s\",\"sha256\":\"%s\","
			         "\"verification\":\"the test fixture's own tarball\"}",
			         test_http_src(tarball), sha);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipe-revise", rbody, &r) != 0 ||
			    r.status != 202) {
				fprintf(stderr, "FAIL: ADR-0323 POST recipe-revise expected 202, got %d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);
			for (waited = 0; waited < 120; waited++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/recipe-commit", NULL, &r) == 0 &&
				    r.json != NULL && json_str_field(r.json, "state") != NULL &&
				    strcmp(json_str_field(r.json, "state"), "running") != 0)
					break;
				cix_response_free(&r);
				usleep(500000);
			}
			if (r.json == NULL || !str_eq(json_str_field(r.json, "state"), "done") ||
			    !str_eq(json_str_field(r.json, "path"), "recipes/package/commitpkg@1.2-1.cbs")) {
				fprintf(stderr, "FAIL: ADR-0323 the revision ended: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);

			snprintf(body_path, sizeof(body_path), "%s/commitpkg@1.2-1.cbs.request.json",
			         forge_dir);
			sent = slurp_file(body_path, &posted, &posted_len) == 0
			           ? json_parse(posted, posted_len)
			           : NULL;
			b64 = sent != NULL ? json_as_string(json_object_get(sent, "content")) : NULL;
			if (b64 != NULL)
				dn = base64_decode(b64, (unsigned char *)decoded_rev, sizeof(decoded_rev) - 1);
			if (dn >= 0)
				decoded_rev[dn] = '\0';
			if (dn < 0 || strstr(decoded_rev, "version \"1.2\"") == NULL ||
			    strstr(decoded_rev, "release 1") == NULL || strstr(decoded_rev, sha) == NULL ||
			    strstr(decoded_rev, "mkdir \"${dest}/usr/share/commitpkg\" parents") == NULL ||
			    strstr(decoded_rev, "1.2-1: commitpkg 1.2, sha256 ") == NULL ||
			    strstr(decoded_rev, "verified by the test fixture's own tarball") == NULL ||
			    strstr(decoded_rev, "Written by cixd from 1.0-1") == NULL) {
				fprintf(stderr, "FAIL: ADR-0323 the forge did not receive the revision cbs "
				                "should have written: %s\n",
				        dn >= 0 ? decoded_rev : "(nothing decoded)");
				ok = 0;
			}
			json_free(sent);
			free(posted);
			posted = NULL;

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/recipes/commitpkg", NULL, &r) != 0 ||
			    !str_eq(json_str_field(r.json, "version"), "1.2-1")) {
				fprintf(stderr, "FAIL: ADR-0323 the revision was committed but not published\n");
				ok = 0;
			}
			cix_response_free(&r);

			/*
			 * A recipe that already has a changelog -- 1.2-1 now does. cbs
			 * v0.1.102 wrote the new changelog over the KEY on that path
			 * (cix-build-system#279, fixed in v0.1.104, which every cbs
			 * this test meets now is), and cixd's revision_says() would
			 * refuse such a revision. So the revision must be committed,
			 * with the new changelog under its own key; a refusal is that
			 * bug returning.
			 */
			snprintf(rbody, sizeof(rbody),
			         "{\"name\":\"commitpkg\",\"version\":\"1.3\",\"url\":\"%s\",\"sha256\":\"%s\","
			         "\"verification\":\"the test fixture's own tarball\"}",
			         test_http_src(tarball), sha);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipe-revise", rbody, &r) != 0) {
				fprintf(stderr, "FAIL: ADR-0323 POST recipe-revise 1.3 got no answer\n");
				ok = 0;
			} else if (r.status == 400) {
				fprintf(stderr, "FAIL: ADR-0323 revising a recipe that has a changelog was "
				                "refused -- cix-build-system#279 is back: %s\n",
				        r.body != NULL ? r.body : "");
				ok = 0;
				cix_response_free(&r);
			} else if (r.status == 202) {
				cix_response_free(&r);
				for (waited = 0; waited < 120; waited++) {
					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "GET", "/v1/pkg/recipe-commit", NULL, &r) == 0 &&
					    r.json != NULL && json_str_field(r.json, "state") != NULL &&
					    strcmp(json_str_field(r.json, "state"), "running") != 0)
						break;
					cix_response_free(&r);
					usleep(500000);
				}
				cix_response_free(&r);
				snprintf(body_path, sizeof(body_path), "%s/commitpkg@1.3-1.cbs.request.json",
				         forge_dir);
				dn = -1;
				sent = slurp_file(body_path, &posted, &posted_len) == 0
				           ? json_parse(posted, posted_len)
				           : NULL;
				b64 = sent != NULL ? json_as_string(json_object_get(sent, "content")) : NULL;
				if (b64 != NULL)
					dn = base64_decode(b64, (unsigned char *)decoded_rev,
					                   sizeof(decoded_rev) - 1);
				if (dn >= 0)
					decoded_rev[dn] = '\0';
				if (dn < 0 || strstr(decoded_rev, "\"changelog\" \"1.3-1: commitpkg 1.3") == NULL) {
					fprintf(stderr, "FAIL: ADR-0323 a revision was committed without its "
					                "changelog under the changelog key: %s\n",
					        dn >= 0 ? decoded_rev : "(nothing decoded)");
					ok = 0;
				}
				json_free(sent);
				free(posted);
				posted = NULL;
			} else {
				fprintf(stderr, "FAIL: ADR-0323 revise 1.3 expected 400 or 202, got %d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				cix_response_free(&r);
				ok = 0;
			}
		}

		/*
		 * ADR-0323: gitea-tags discovery against this fixture's forge.
		 * giteapkg declares the whole block -- its source template
		 * expands to its own main url, which is how the kind knows the
		 * repository -- and the forge lists v1.1 beside the v1.0 it
		 * builds. giteapkg2 declares no source, so its repository is
		 * unknown and the catalogue must say why rather than "never
		 * fetched". The refresh runs through a schedule, the operator's
		 * own route (POST /v1/schedules/{name}/run).
		 */
		if (stage_ok) {
			char gsha[65], gtar[PATH_MAX], archive_dir[PATH_MAX], cmd[3 * PATH_MAX];
			char gurl[256], gtmpl[256], decls[512], grecipe[4096], gpath[PATH_MAX];
			FILE *fp;
			int found = 0;

			snprintf(archive_dir, sizeof(archive_dir), "%s/api/v1/repos/o/giteapkg/archive",
			         forge_dir);
			snprintf(gurl, sizeof(gurl),
			         "http://127.0.0.1:%d/api/v1/repos/o/giteapkg/archive/v1.0.tar.gz", forge_port);
			snprintf(gtmpl, sizeof(gtmpl),
			         "http://127.0.0.1:%d/api/v1/repos/o/giteapkg/archive/v{version}.tar.gz",
			         forge_port);
			if (stage_fixture_tarball(scratch_dir, "giteapkg", "1.0", gtar, sizeof(gtar), gsha,
			                          sizeof(gsha)) != 0) {
				fprintf(stderr, "FAIL: ADR-0323 could not stage the giteapkg source\n");
				ok = 0;
			} else {
				snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && cp '%s' '%s/v1.0.tar.gz'",
				         archive_dir, gtar, archive_dir);
				if (system(cmd) != 0) {
					fprintf(stderr, "FAIL: ADR-0323 could not place the giteapkg archive\n");
					ok = 0;
				}
				snprintf(gpath, sizeof(gpath), "%s/api/v1/repos/o/giteapkg/tags", forge_dir);
				fp = fopen(gpath, "w");
				if (fp != NULL) {
					fputs("[{\"name\":\"v1.1\",\"id\":\"x\"},{\"name\":\"latest\"},"
					      "{\"name\":\"v1.0\"}]",
					      fp);
					fclose(fp);
				}

				snprintf(decls, sizeof(decls),
				         "    upstream \"gitea-tags\" {\n"
				         "        tag \"v{version}\"\n"
				         "        source \"%s\"\n"
				         "        verify origin\n"
				         "    }\n",
				         gtmpl);
				cpdl_upstream_recipe_text(grecipe, sizeof(grecipe), "giteapkg", gurl, gsha, decls);
				jw_init(&w);
				jw_obj_open(&w);
				jw_key(&w, "name");
				jw_str(&w, "giteapkg");
				jw_key(&w, "content");
				jw_str(&w, grecipe);
				jw_key(&w, "source");
				jw_str(&w, "forge");
				jw_key(&w, "format");
				jw_str(&w, "cbs");
				jw_obj_close(&w);
				w.buf[w.len] = '\0';
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
				    r.status != 204) {
					fprintf(stderr, "FAIL: ADR-0323 publishing giteapkg got %d: %s\n", r.status,
					        r.body != NULL ? r.body : "");
					ok = 0;
				}
				cix_response_free(&r);
				jw_free(&w);

				cpdl_upstream_recipe_text(grecipe, sizeof(grecipe), "giteapkg2",
				                          test_http_src(gtar), gsha,
				                          "    upstream \"gitea-tags\" {\n"
				                          "        tag \"v{version}\"\n"
				                          "        verify origin\n"
				                          "    }\n");
				jw_init(&w);
				jw_obj_open(&w);
				jw_key(&w, "name");
				jw_str(&w, "giteapkg2");
				jw_key(&w, "content");
				jw_str(&w, grecipe);
				jw_key(&w, "source");
				jw_str(&w, "forge");
				jw_key(&w, "format");
				jw_str(&w, "cbs");
				jw_obj_close(&w);
				w.buf[w.len] = '\0';
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
				    r.status != 204) {
					fprintf(stderr, "FAIL: ADR-0323 publishing giteapkg2 got %d: %s\n",
					        r.status, r.body != NULL ? r.body : "");
					ok = 0;
				}
				cix_response_free(&r);
				jw_free(&w);
			}

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "PUT", "/v1/schedules/discover-test",
			                       "{\"action\":\"pkg.discover\","
			                       "\"params\":{\"kind\":\"gitea-tags\"},"
			                       "\"schedule\":{\"every\":{\"hours\":24}}}",
			                       &r) != 0 ||
			    (r.status != 200 && r.status != 201)) {
				fprintf(stderr, "FAIL: ADR-0323 creating the refresh schedule got %d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/schedules/discover-test/run", NULL,
			                       &r) != 0 ||
			    r.status != 200 || r.body == NULL ||
			    strstr(r.body, "discovery started") == NULL) {
				fprintf(stderr, "FAIL: ADR-0323 running the refresh must start discovery "
				                "part, got %d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);

			for (waited = 0; waited < 60 && found != 3; waited++) {
				const struct json_value *pkgs;
				size_t k;

				found = 0;
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/source-catalogue", NULL, &r) == 0 &&
				    r.json != NULL &&
				    (pkgs = json_object_get(r.json, "packages")) != NULL &&
				    pkgs->type == JSON_ARRAY) {
					for (k = 0; k < pkgs->u.array.count; k++) {
						const struct json_value *e = pkgs->u.array.items[k];
						const char *n = json_str_field(e, "name");

						if (str_eq(n, "giteapkg") &&
						    str_eq(json_str_field(e, "resolved_version"), "1.1") &&
						    str_eq(json_str_field(e, "status"), "blocked") &&
						    json_str_field(e, "reason") != NULL &&
						    strstr(json_str_field(e, "reason"), "not trusted") != NULL)
							found |= 1;
						if (str_eq(n, "giteapkg2") && json_str_field(e, "reason") != NULL &&
						    strstr(json_str_field(e, "reason"), "no upstream source template") !=
						        NULL)
							found |= 2;
					}
				}
				if (found != 3) {
					cix_response_free(&r);
					usleep(500000);
				}
			}
			if (found != 3) {
				fprintf(stderr, "FAIL: ADR-0323 discovery: giteapkg %s resolved to 1.1, giteapkg2 "
				                "%s say why it cannot be read: %s\n",
				        (found & 1) ? "was" : "was NOT", (found & 2) ? "did" : "did NOT",
				        r.body != NULL ? r.body : "(no catalogue)");
				ok = 0;
			}
			cix_response_free(&r);

			/*
			 * ADR-0323 rung 4 and the author stage: trust the forge's
			 * origin, serve v1.1's archive at the template's path, run
			 * discovery again -- and giteapkg@1.1-1 must be written from
			 * 1.0-1, committed to the forge with the archive's url and
			 * sha256 and a changelog naming origin trust, and published.
			 */
			if (found == 3) {
				char trust[256], vbody[PATH_MAX], decoded_g[8192];
				struct json_value *sent;
				const char *b64;
				int dn = -1, published = 0;

				snprintf(cmd, sizeof(cmd), "cp '%s' '%s/v1.1.tar.gz'", gtar, archive_dir);
				snprintf(trust, sizeof(trust), "{\"origins\":[\"http://127.0.0.1:%d\"]}",
				         forge_port);
				memset(&r, 0, sizeof(r));
				if (system(cmd) != 0 ||
				    cix_client_request(&client, "PUT", "/v1/pkg/trusted-origins", trust, &r) != 0 ||
				    r.status != 200) {
					fprintf(stderr, "FAIL: ADR-0323 trusting the forge's origin got %d: %s\n",
					        r.status, r.body != NULL ? r.body : "");
					ok = 0;
				}
				cix_response_free(&r);
				memset(&r, 0, sizeof(r));
				cix_client_request(&client, "POST", "/v1/schedules/discover-test/run", NULL, &r);
				cix_response_free(&r);
				for (waited = 0; waited < 120 && !published; waited++) {
					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "GET", "/v1/pkg/recipes/giteapkg", NULL, &r) ==
					        0 &&
					    str_eq(json_str_field(r.json, "version"), "1.1-1"))
						published = 1;
					cix_response_free(&r);
					if (!published)
						usleep(500000);
				}
				snprintf(vbody, sizeof(vbody), "%s/giteapkg@1.1-1.cbs.request.json", forge_dir);
				sent = slurp_file(vbody, &posted, &posted_len) == 0
				           ? json_parse(posted, posted_len)
				           : NULL;
				b64 = sent != NULL ? json_as_string(json_object_get(sent, "content")) : NULL;
				if (b64 != NULL)
					dn = base64_decode(b64, (unsigned char *)decoded_g, sizeof(decoded_g) - 1);
				if (dn >= 0)
					decoded_g[dn] = '\0';
				if (!published || dn < 0 || strstr(decoded_g, "/archive/v1.1.tar.gz\"") == NULL ||
				    strstr(decoded_g, gsha) == NULL || strstr(decoded_g, "version \"1.1\"") == NULL ||
				    strstr(decoded_g, "verified by origin trust") == NULL) {
					fprintf(stderr, "FAIL: ADR-0323 discovery did not author giteapkg@1.1-1 from "
					                "the trusted origin (published=%d): %s\n",
					        published, dn >= 0 ? decoded_g : "(nothing committed)");
					ok = 0;
				}
				json_free(sent);
				free(posted);
				posted = NULL;
				memset(&r, 0, sizeof(r));
				cix_client_request(&client, "PUT", "/v1/pkg/trusted-origins", "{\"origins\":[]}", &r);
				cix_response_free(&r);
			}
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", "/v1/schedules/discover-test", NULL, &r);
			cix_response_free(&r);
		}

		/*
		 * ADR-0323 rung 2, a signed checksum list. kfake declares the
		 * kernel.org kind with `verify checksums`; the seeded release
		 * list resolves stable to 7.2.8, the forge serves a list signed
		 * by the fixture key, and the archive's name in it is
		 * linux-fixture-7.tar.xz ({major} of 7.2.8). Without the key
		 * installed the row says which key and how; with a key pinned
		 * under the wrong fingerprint nothing is installed; with the
		 * key, kfake@7.2.8-1 is written with the sha256 the signed list
		 * gives. {"refresh":false} keeps the run off kernel.org.
		 */
		if (stage_ok) {
			static const char KSHA1[] =
			    "dde23ff805667659ae248f40740a4b9e95d8c8a920d97220ee7e74cd08b37493";
			static const char KSHA7[] =
			    "d0b5a8b22c671da9b231dc90e8143e6714e414b25668cfd4a949f9678b943810";
			char ktmpl[256], kurl[256], klist[256], kdecls[1024], krecipe[4096];
			char kdir[PATH_MAX], kfile[PATH_MAX + 32], kcmd[PATH_MAX + 32], kkeys[128];
			char kreq[PATH_MAX], kdecoded[8192];
			struct json_value *sent;
			const char *b64;
			FILE *fp;
			int kfound = 0, kpublished = 0, dn = -1;

			snprintf(ktmpl, sizeof(ktmpl),
			         "http://127.0.0.1:%d/k/{version}/linux-fixture-{major}.tar.xz", forge_port);
			snprintf(kurl, sizeof(kurl), "http://127.0.0.1:%d/k/1.0/linux-fixture-1.tar.xz",
			         forge_port);
			snprintf(klist, sizeof(klist),
			         "http://127.0.0.1:%d/k/v{major}.x/{version}/sha256sums.asc", forge_port);
			snprintf(kdecls, sizeof(kdecls),
			         "    upstream \"kernel.org\" {\n"
			         "        source \"%s\"\n"
			         "        verify checksums \"openpgp-clearsigned\" {\n"
			         "            url \"%s\"\n"
			         "            key \"%s\"\n"
			         "        }\n"
			         "    }\n",
			         ktmpl, klist, TEST_FPR);
			cpdl_upstream_recipe_text(krecipe, sizeof(krecipe), "kfake", kurl, KSHA1, kdecls);
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "kfake");
			jw_key(&w, "content");
			jw_str(&w, krecipe);
			jw_key(&w, "source");
			jw_str(&w, "forge");
			jw_key(&w, "format");
			jw_str(&w, "cbs");
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
			    r.status != 204) {
				fprintf(stderr, "FAIL: rung 2 publishing kfake got %d: %s\n", r.status,
				        r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);
			jw_free(&w);

			/* No platform default channel exists until an operator sets
			 * one, and kernel.org has channels: kfake gets its own. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "PUT", "/v1/pkg/kfake/source-policy",
			                       "{\"channel\":\"stable\",\"depth\":\"n\"}", &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: rung 2 setting kfake's source policy got %d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);


			snprintf(kdir, sizeof(kdir), "%s/k/v7.x/7.2.8", forge_dir);
			snprintf(kcmd, sizeof(kcmd), "mkdir -p '%s'", kdir);
			snprintf(kfile, sizeof(kfile), "%s/sha256sums.asc", kdir);
			if (system(kcmd) != 0 || (fp = fopen(kfile, "w")) == NULL) {
				fprintf(stderr, "FAIL: rung 2 could not place the signed list\n");
				ok = 0;
			} else {
				fputs(SIGNED_DOC, fp);
				fclose(fp);
			}

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "PUT", "/v1/schedules/discover-kernel",
			                       "{\"action\":\"pkg.discover\","
			                       "\"params\":{\"kind\":\"kernel.org\",\"refresh\":false},"
			                       "\"schedule\":{\"every\":{\"hours\":24}}}",
			                       &r) != 0 ||
			    (r.status != 200 && r.status != 201)) {
				fprintf(stderr, "FAIL: rung 2 creating the schedule got %d: %s\n", r.status,
				        r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/schedules/discover-kernel/run", NULL,
			                       &r) != 0 ||
			    r.status != 200 || r.body == NULL || strstr(r.body, "discovery started") == NULL) {
				fprintf(stderr, "FAIL: rung 2 a run with refresh false must start discovery "
				                "without fetching, got %d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);

			for (waited = 0; waited < 60 && !kfound; waited++) {
				const struct json_value *pkgs;
				size_t k;

				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/source-catalogue", NULL, &r) == 0 &&
				    r.json != NULL &&
				    (pkgs = json_object_get(r.json, "packages")) != NULL &&
				    pkgs->type == JSON_ARRAY) {
					for (k = 0; k < pkgs->u.array.count; k++) {
						const struct json_value *e = pkgs->u.array.items[k];

						if (str_eq(json_str_field(e, "name"), "kfake") &&
						    str_eq(json_str_field(e, "resolved_version"), "7.2.8") &&
						    str_eq(json_str_field(e, "status"), "blocked") &&
						    json_str_field(e, "reason") != NULL &&
						    strstr(json_str_field(e, "reason"), "no upstream key") != NULL)
							kfound = 1;
					}
				}
				if (!kfound) {
					cix_response_free(&r);
					usleep(500000);
				}
			}
			if (!kfound) {
				fprintf(stderr, "FAIL: rung 2 with no key installed, kfake's row must say "
				                "which key is missing: %s\n",
				        r.body != NULL ? r.body : "(no catalogue)");
				ok = 0;
			}
			cix_response_free(&r);

			/* Pinned under a fingerprint that is not the key's own: refused. */
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "fingerprint");
			jw_str(&w, "0000000000000000000000000000000000000000");
			jw_key(&w, "key");
			jw_str(&w, TEST_KEY);
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/kfake/upstream-keys", w.buf, &r) !=
			        0 ||
			    r.status != 400) {
				fprintf(stderr, "FAIL: rung 2 a key pinned under another fingerprint must be "
				                "refused, got %d: %s\n",
				        r.status, r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);
			jw_free(&w);

			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "fingerprint");
			jw_str(&w, TEST_FPR);
			jw_key(&w, "key");
			jw_str(&w, TEST_KEY);
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/kfake/upstream-keys", w.buf, &r) !=
			        0 ||
			    r.status != 201 || r.body == NULL || strstr(r.body, TEST_FPR) == NULL) {
				fprintf(stderr, "FAIL: rung 2 installing the key got %d: %s\n", r.status,
				        r.body != NULL ? r.body : "");
				ok = 0;
			}
			cix_response_free(&r);
			jw_free(&w);

			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "POST", "/v1/schedules/discover-kernel/run", NULL, &r);
			cix_response_free(&r);
			for (waited = 0; waited < 120 && !kpublished; waited++) {
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/recipes/kfake", NULL, &r) == 0 &&
				    str_eq(json_str_field(r.json, "version"), "7.2.8-1"))
					kpublished = 1;
				cix_response_free(&r);
				if (!kpublished)
					usleep(500000);
			}
			snprintf(kreq, sizeof(kreq), "%s/kfake@7.2.8-1.cbs.request.json", forge_dir);
			sent = slurp_file(kreq, &posted, &posted_len) == 0 ? json_parse(posted, posted_len)
			                                                    : NULL;
			b64 = sent != NULL ? json_as_string(json_object_get(sent, "content")) : NULL;
			if (b64 != NULL)
				dn = base64_decode(b64, (unsigned char *)kdecoded, sizeof(kdecoded) - 1);
			if (dn >= 0)
				kdecoded[dn] = '\0';
			if (!kpublished || dn < 0 ||
			    strstr(kdecoded, "/k/7.2.8/linux-fixture-7.tar.xz\"") == NULL ||
			    strstr(kdecoded, KSHA7) == NULL || strstr(kdecoded, "version \"7.2.8\"") == NULL ||
			    strstr(kdecoded, "verified by signed checksums") == NULL) {
				fprintf(stderr, "FAIL: rung 2 discovery did not author kfake@7.2.8-1 from the "
				                "signed list (published=%d): %s\n",
				        kpublished, dn >= 0 ? kdecoded : "(nothing committed)");
				ok = 0;
			}
			json_free(sent);
			free(posted);
			posted = NULL;

			snprintf(kkeys, sizeof(kkeys), "/v1/pkg/kfake/upstream-keys/%s", TEST_FPR);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "DELETE", kkeys, NULL, &r) != 0 || r.status != 204) {
				fprintf(stderr, "FAIL: rung 2 removing the key got %d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", "/v1/schedules/discover-kernel", NULL, &r);
			cix_response_free(&r);
		}

		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "DELETE", "/v1/pkg/sources/forge2", NULL, &r);
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/pkg/sources/forge", NULL, &r) != 0 ||
		    r.status != 200 || r.body == NULL || strstr(r.body, "\"sources\":[]") == NULL) {
			fprintf(stderr, "FAIL: ADR-0324 removing the sources left %s\n",
			        r.body != NULL ? r.body : "(no body)");
			ok = 0;
		}
		cix_response_free(&r);
		free(json_body);
		if (forge_pid > 0)
			test_http_server_stop(forge_pid);
	}

	/*
	 * ADR-0324: a sync over two sources. Each offers a package of its
	 * own and both offer `shared`, with different content. Their own
	 * packages arrive; `shared` is held -- merged from neither -- until
	 * the operator chooses; once chosen it arrives from that source, and
	 * when the choice moves to the other source its copy, a different
	 * recipe under the same version, is refused as divergent and the
	 * store keeps what it had.
	 *
	 * Each source is a plain tar served where Gitea's archive endpoint
	 * puts it; extraction detects the format and strips the archive's
	 * top directory, as it does for a real forge archive.
	 */
	{
		char forge_dir[PATH_MAX], sha[65], tarball[PATH_MAX], path[PATH_MAX];
		char post[512], recipe[4096];
		pid_t forge_pid = -1;
		int forge_port = 0, stage_ok = 1, i;
		static const char *const repos[][3] = {
			/* source, owner/repo dir, its own package */
			{ "srca", "oa/ra", "synca" },
			{ "srcb", "ob/rb", "syncb" },
		};

		snprintf(forge_dir, sizeof(forge_dir), "%s/syncforge", scratch_dir);
		if (mkdir(forge_dir, 0755) != 0 ||
		    test_http_server_start(forge_dir, 1, &forge_port, &forge_pid) != 0 ||
		    stage_fixture_tarball(scratch_dir, "syncsrc", "1.0", tarball, sizeof(tarball), sha,
		                          sizeof(sha)) != 0) {
			fprintf(stderr, "FAIL: ADR-0324 could not stage the sync fixture\n");
			ok = 0;
			stage_ok = 0;
		}
		for (i = 0; stage_ok && i < 2; i++) {
			const char *pkgs[2] = { repos[i][2], "shared" };
			int j;

			if (run_cmd("mkdir -p '%s/tree/%s/recipes/package' '%s/api/v1/repos/%s/archive'",
			            forge_dir, repos[i][1], forge_dir, repos[i][1]) != 0)
				stage_ok = 0;
			for (j = 0; stage_ok && j < 2; j++) {
				FILE *fp;
				char install[160];

				/* `shared` installs a directory named after its source,
				 * so the two copies are different recipes. */
				snprintf(install, sizeof(install),
				         "        mkdir \"${dest}/usr/share/%s-from-%s\" parents\n", pkgs[j],
				         repos[i][0]);
				cpdl_recipe_text_decl(recipe, sizeof(recipe), pkgs[j], "1.0", test_http_src(tarball),
				                      sha, NULL,
				                      "            tool \"bash\"\n"
				                      "            tool \"coreutils\"\n",
				                      NULL,
				                      /* a metadata block, so an approval has a place to go */
				                      "    metadata {\n        \"changelog\" \"fixture\"\n    }\n\n",
				                      "        run \"true\" {\n        }\n", install);
				snprintf(path, sizeof(path), "%s/tree/%s/recipes/package/%s@1.0-1.cbs", forge_dir,
				         repos[i][1], pkgs[j]);
				fp = fopen(path, "w");
				if (fp == NULL || fputs(recipe, fp) < 0) {
					stage_ok = 0;
					if (fp != NULL)
						fclose(fp);
					break;
				}
				fclose(fp);
			}
			/* <repo>/recipes/..., so the archive has one top directory. */
			if (stage_ok &&
			    run_cmd("tar -cf '%s/api/v1/repos/%s/archive/main.tar.gz' -C '%s/tree/%.2s' '%s'",
			            forge_dir, repos[i][1], forge_dir, repos[i][1], repos[i][1] + 3) != 0)
				stage_ok = 0;
			snprintf(post, sizeof(post),
			         "{\"name\":\"%s\",\"url\":\"http://127.0.0.1:%d/%s\",\"kind\":\"gitea\"}",
			         repos[i][0], forge_port, repos[i][1]);
			memset(&r, 0, sizeof(r));
			if (stage_ok &&
			    (cix_client_request(&client, "POST", "/v1/pkg/sources", post, &r) != 0 ||
			     r.status != 201))
				stage_ok = 0;
			cix_response_free(&r);
		}
		if (!stage_ok) {
			fprintf(stderr, "FAIL: ADR-0324 could not set up the two sources\n");
			ok = 0;
		}

		if (stage_ok) {
			char st_s[32];

			/* First sync: own packages arrive, `shared` is held. */
			if (sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0 || !str_eq(st_s, "success") ||
			    r.body == NULL || strstr(r.body, "\"held\":2") == NULL) {
				fprintf(stderr, "FAIL: ADR-0324 the first two-source sync must succeed holding "
				                "shared once per source: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);
			for (i = 0; i < 3; i++) {
				static const char *const names[] = { "synca", "syncb", "shared" };
				char rpath[128];

				snprintf(rpath, sizeof(rpath), "/v1/pkg/recipes/%s", names[i]);
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", rpath, NULL, &r) != 0 ||
				    r.status != (i < 2 ? 200 : 404)) {
					fprintf(stderr, "FAIL: ADR-0324 after the first sync %s answered %d, "
					                "expected %d\n",
					        names[i], r.status, i < 2 ? 200 : 404);
					ok = 0;
				}
				cix_response_free(&r);
			}
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/source-ownership", NULL, &r) != 0 ||
			    r.body == NULL || strstr(r.body, "\"item\":\"package:shared\"") == NULL ||
			    strstr(r.body, "srca, srcb") == NULL) {
				fprintf(stderr, "FAIL: ADR-0324 source-ownership must hold package:shared, "
				                "offered by srca, srcb: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);

			/* Chosen: it arrives from srcb. */
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "PUT", "/v1/pkg/source-ownership",
			                       "{\"item\":\"package:shared\",\"source\":\"srcb\"}", &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: ADR-0324 choosing srcb for shared, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
			if (sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0 || !str_eq(st_s, "success")) {
				fprintf(stderr, "FAIL: ADR-0324 the second sync ended %s\n", st_s);
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/recipes/shared", NULL, &r) != 0 ||
			    r.status != 200 || r.body == NULL || strstr(r.body, "shared-from-srcb") == NULL) {
				fprintf(stderr, "FAIL: ADR-0324 shared must arrive from srcb once chosen: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);

			/* The choice moves to srca: its copy is a different recipe
			 * under the same version, so it is refused, not merged. */
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "PUT", "/v1/pkg/source-ownership",
			                   "{\"item\":\"package:shared\",\"source\":\"srca\"}", &r);
			cix_response_free(&r);
			if (sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0 || !str_eq(st_s, "success") ||
			    r.body == NULL || strstr(r.body, "\"divergent\":1") == NULL) {
				fprintf(stderr, "FAIL: ADR-0324 srca's different copy of shared@1.0-1 must be "
				                "counted divergent: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/recipes/shared", NULL, &r) != 0 ||
			    r.body == NULL || strstr(r.body, "shared-from-srcb") == NULL) {
				fprintf(stderr, "FAIL: ADR-0324 a divergent copy must not replace the stored "
				                "recipe: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);

			/*
			 * Git is authoritative for recipes: srcb's copy gains one
			 * comment line in git. That changes nothing built, so the
			 * stored text becomes git's, nothing rebuilds, and it is
			 * counted refreshed -- never divergent.
			 */
			if (run_cmd("sed -i '1i # a comment git added after publishing' "
			            "'%s/tree/%s/recipes/package/shared@1.0-1.cbs'",
			            forge_dir, repos[1][1]) != 0 ||
			    run_cmd("tar -cf '%s/api/v1/repos/%s/archive/main.tar.gz' -C '%s/tree/%.2s' '%s'",
			            forge_dir, repos[1][1], forge_dir, repos[1][1], repos[1][1] + 3) != 0) {
				fprintf(stderr, "FAIL: ADR-0324 could not edit srcb's copy of shared\n");
				ok = 0;
			}
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "PUT", "/v1/pkg/source-ownership",
			                   "{\"item\":\"package:shared\",\"source\":\"srcb\"}", &r);
			cix_response_free(&r);
			if (sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0 || !str_eq(st_s, "success") ||
			    r.body == NULL || strstr(r.body, "\"refreshed\":1") == NULL ||
			    /* the top-level total comes first in the body */
			    strstr(r.body, "\"divergent\":") == NULL ||
			    strncmp(strstr(r.body, "\"divergent\":"), "\"divergent\":0,", 14) != 0) {
				fprintf(stderr, "FAIL: ADR-0324 a comment-only change in git must be counted "
				                "refreshed, not divergent: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/recipes/shared", NULL, &r) != 0 ||
			    r.body == NULL || strstr(r.body, "a comment git added after publishing") == NULL ||
			    strstr(r.body, "shared-from-srcb") == NULL) {
				fprintf(stderr, "FAIL: ADR-0324 the stored recipe must now be git's text: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);

			/*
			 * Approvals travel both ways (ADR-0324). Git gains one: the
			 * next sync adopts it, nothing rebuilds. Then git loses it,
			 * as a hand edit might: this host now holds an approval git
			 * lacks, srcb is made writable, and the next sync writes the
			 * stored text back -- one batch commit, the update naming
			 * the blob it read, the only change the approval line.
			 */
			{
				static const char approval[] =
				    "abababababababababababababababababababababababababababababababab";
				char gitfile[PATH_MAX], served[PATH_MAX], req_path[PATH_MAX];
				char *text = NULL, *posted_req = NULL, *b64 = NULL;
				size_t text_len = 0, posted_len = 0;
				struct json_value *sent = NULL;
				const struct json_value *file0 = NULL;
				int waited, written = 0;

				snprintf(gitfile, sizeof(gitfile), "%s/tree/%s/recipes/package/shared@1.0-1.cbs",
				         forge_dir, repos[1][1]);
				if (run_cmd("sed -i '/^    metadata {$/a\\        \"artifact_sha256\" \"%s\"' '%s'",
				            approval, gitfile) != 0 ||
				    run_cmd("tar -cf '%s/api/v1/repos/%s/archive/main.tar.gz' -C '%s/tree/%.2s' "
				            "'%s'",
				            forge_dir, repos[1][1], forge_dir, repos[1][1], repos[1][1] + 3) != 0) {
					fprintf(stderr, "FAIL: ADR-0324 could not add an approval to srcb's copy\n");
					ok = 0;
				}
				if (sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0 ||
				    r.body == NULL || strstr(r.body, "\"refreshed\":1") == NULL) {
					fprintf(stderr, "FAIL: ADR-0324 an approval git carries must be adopted as a "
					                "refresh: %s\n",
					        r.body != NULL ? r.body : "(no body)");
					ok = 0;
				}
				cix_response_free(&r);
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/recipes/shared", NULL, &r) != 0 ||
				    r.body == NULL || strstr(r.body, approval) == NULL) {
					fprintf(stderr, "FAIL: ADR-0324 the adopted approval must be in the stored "
					                "recipe: %s\n",
					        r.body != NULL ? r.body : "(no body)");
					ok = 0;
				}
				cix_response_free(&r);

				/* Git loses it; srcb becomes writable; the forge serves the file. */
				if (run_cmd("sed -i '/artifact_sha256/d' '%s'", gitfile) != 0 ||
				    run_cmd("tar -cf '%s/api/v1/repos/%s/archive/main.tar.gz' -C '%s/tree/%.2s' "
				            "'%s'",
				            forge_dir, repos[1][1], forge_dir, repos[1][1], repos[1][1] + 3) != 0 ||
				    run_cmd("mkdir -p '%s/api/v1/repos/%s/contents/recipes/package'", forge_dir,
				            repos[1][1]) != 0 ||
				    slurp_file(gitfile, &text, &text_len) != 0 ||
				    (b64 = malloc(((text_len + 2) / 3) * 4 + 1)) == NULL ||
				    base64_encode((const unsigned char *)text, text_len, b64,
				                  ((text_len + 2) / 3) * 4 + 1) != 0) {
					fprintf(stderr, "FAIL: ADR-0324 could not stage git's copy for write-back\n");
					ok = 0;
				} else {
					FILE *fp;

					snprintf(served, sizeof(served),
					         "%s/api/v1/repos/%s/contents/recipes/package/shared@1.0-1.cbs",
					         forge_dir, repos[1][1]);
					fp = fopen(served, "w");
					if (fp == NULL ||
					    fprintf(fp, "{\"sha\":\"b10bb10b\",\"encoding\":\"base64\","
					                "\"content\":\"%s\"}",
					            b64) < 0) {
						fprintf(stderr, "FAIL: ADR-0324 could not serve git's copy\n");
						ok = 0;
					}
					if (fp != NULL)
						fclose(fp);
				}
				free(text);
				free(b64);
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "PUT", "/v1/pkg/sources/srcb",
				                       "{\"write\":true,\"token\":\"srcb-token\"}", &r) != 0 ||
				    r.status != 200) {
					fprintf(stderr, "FAIL: ADR-0324 making srcb writable, status=%d\n", r.status);
					ok = 0;
				}
				cix_response_free(&r);
				if (sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0 || !str_eq(st_s, "success")) {
					fprintf(stderr, "FAIL: ADR-0324 the write-back sync ended %s\n", st_s);
					ok = 0;
				}
				cix_response_free(&r);
				for (waited = 0; waited < 60 && !written; waited++) {
					memset(&r, 0, sizeof(r));
					if (cix_client_request(&client, "GET", "/v1/pkg/sync", NULL, &r) == 0 &&
					    r.body != NULL && strstr(r.body, "\"written\":1") != NULL)
						written = 1;
					else
						usleep(500000);
					if (!written && waited == 59)
						fprintf(stderr, "FAIL: ADR-0324 the approval was never written back: "
						                "%s\n",
						        r.body != NULL ? r.body : "(no body)");
					cix_response_free(&r);
				}
				if (!written)
					ok = 0;
				snprintf(req_path, sizeof(req_path), "%s/contents.request.json", forge_dir);
				sent = slurp_file(req_path, &posted_req, &posted_len) == 0
				           ? json_parse(posted_req, posted_len)
				           : NULL;
				if (sent != NULL) {
					const struct json_value *files = json_object_get(sent, "files");

					if (files != NULL && files->type == JSON_ARRAY && files->u.array.count == 1)
						file0 = files->u.array.items[0];
				}
				if (file0 == NULL ||
				    !str_eq(json_as_string(json_object_get(file0, "operation")), "update") ||
				    !str_eq(json_as_string(json_object_get(file0, "path")),
				            "recipes/package/shared@1.0-1.cbs") ||
				    !str_eq(json_as_string(json_object_get(file0, "sha")), "b10bb10b") ||
				    json_as_string(json_object_get(file0, "content")) == NULL) {
					fprintf(stderr, "FAIL: ADR-0324 the write-back must be one update of "
					                "shared's file naming the blob it read: %s\n",
					        posted_req != NULL ? posted_req : "(nothing posted)");
					ok = 0;
				} else {
					unsigned char decoded[8192];
					int dn = base64_decode(json_as_string(json_object_get(file0, "content")),
					                       decoded, sizeof(decoded) - 1);

					if (dn >= 0)
						decoded[dn] = '\0';
					if (dn < 0 || strstr((char *)decoded, approval) == NULL ||
					    strstr((char *)decoded, "shared-from-srcb") == NULL) {
						fprintf(stderr, "FAIL: ADR-0324 the written-back text must be the stored "
						                "recipe carrying its approval\n");
						ok = 0;
					}
				}
				json_free(sent);
				free(posted_req);
			}

			/*
			 * ADR-0324 step C, end to end against the fake forge. This
			 * host gets a catalogue key and srcb gets its public half:
			 * the next sync signs srcb's tree (this host may write it),
			 * commits INDEX and INDEX.minisig as creates, and verifies
			 * its own index. With write off, the forge's tree -- which
			 * has no index -- is refused whole. With the committed index
			 * put into it, it is accepted; and a recipe the index does
			 * not list is refused and never stored.
			 */
			{
				char *pem = gen_ed25519_pem(), *pub = NULL, *req = NULL;
				size_t req_len = 0;
				char req_path[PATH_MAX], tree_dir[PATH_MAX];
				struct json_writer jw;
				struct json_value *sentj = NULL;
				const struct json_value *files = NULL;
				int k;

				snprintf(tree_dir, sizeof(tree_dir), "%s/tree/%s", forge_dir, repos[1][1]);
				snprintf(req_path, sizeof(req_path), "%s/contents.request.json", forge_dir);
				unlink(req_path); /* the write-back's batch; this one must be recorded too */
				jw_init(&jw);
				jw_obj_open(&jw);
				jw_key(&jw, "key");
				jw_str(&jw, pem != NULL ? pem : "");
				jw_obj_close(&jw);
				jw.buf[jw.len] = '\0';
				memset(&r, 0, sizeof(r));
				if (pem == NULL ||
				    cix_client_request(&client, "PUT", "/v1/system/catalogue-key", jw.buf, &r) !=
				        0 ||
				    r.status != 200 || r.json == NULL ||
				    json_str_field(r.json, "public_key") == NULL) {
					fprintf(stderr, "FAIL: ADR-0324 installing a catalogue key, status=%d\n",
					        r.status);
					ok = 0;
				} else {
					pub = strdup(json_str_field(r.json, "public_key"));
				}
				cix_response_free(&r);
				jw_free(&jw);
				free(pem);

				jw_init(&jw);
				jw_obj_open(&jw);
				jw_key(&jw, "catalogue_key");
				jw_str(&jw, pub != NULL ? pub : "");
				jw_obj_close(&jw);
				jw.buf[jw.len] = '\0';
				memset(&r, 0, sizeof(r));
				if (pub == NULL ||
				    cix_client_request(&client, "PUT", "/v1/pkg/sources/srcb", jw.buf, &r) != 0 ||
				    r.status != 200 || r.body == NULL || strstr(r.body, "RW") == NULL) {
					fprintf(stderr, "FAIL: ADR-0324 giving srcb the catalogue key, status=%d\n",
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);
				jw_free(&jw);

				/* 1. This host writes srcb: it signs, commits, and accepts its own index. */
				if (sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0 || r.body == NULL ||
				    strstr(r.body, "\"name\":\"srcb\",\"fetched\":true") == NULL) {
					fprintf(stderr, "FAIL: ADR-0324 the signing host must accept its own signed "
					                "tree: %s\n",
					        r.body != NULL ? r.body : "(no body)");
					ok = 0;
				}
				cix_response_free(&r);
				sentj = slurp_file(req_path, &req, &req_len) == 0 ? json_parse(req, req_len)
				                                                  : NULL;
				files = sentj != NULL ? json_object_get(sentj, "files") : NULL;
				if (files == NULL || files->type != JSON_ARRAY || files->u.array.count != 2 ||
				    !str_eq(json_as_string(json_object_get(files->u.array.items[0], "path")),
				            "recipes/INDEX") ||
				    !str_eq(json_as_string(json_object_get(files->u.array.items[0],
				                                           "operation")),
				            "create") ||
				    !str_eq(json_as_string(json_object_get(files->u.array.items[1], "path")),
				            "recipes/INDEX.minisig")) {
					fprintf(stderr, "FAIL: ADR-0324 the index and its signature must be "
					                "committed as creates: %s\n",
					        req != NULL ? req : "(nothing posted)");
					ok = 0;
				}

				/* 2. Not the writer any more: the forge's tree has no index. */
				memset(&r, 0, sizeof(r));
				cix_client_request(&client, "PUT", "/v1/pkg/sources/srcb", "{\"write\":false}",
				                   &r);
				cix_response_free(&r);
				if (sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0 || r.body == NULL ||
				    strstr(r.body, "catalogue: the tree carries no signed index") == NULL) {
					fprintf(stderr, "FAIL: ADR-0324 an unsigned tree from a source with a "
					                "catalogue key must be refused, naming why: %s\n",
					        r.body != NULL ? r.body : "(no body)");
					ok = 0;
				}
				cix_response_free(&r);

				/* 3. The committed index, put into the forge's tree: accepted. */
				for (k = 0; files != NULL && files->type == JSON_ARRAY &&
				            k < (int)files->u.array.count;
				     k++) {
					const char *b64 =
					    json_as_string(json_object_get(files->u.array.items[k], "content"));
					const char *rel =
					    json_as_string(json_object_get(files->u.array.items[k], "path"));
					unsigned char decoded[65536];
					int dn = b64 != NULL ? base64_decode(b64, decoded, sizeof(decoded)) : -1;
					char out_path[PATH_MAX];
					FILE *fp;

					if (dn < 0 || rel == NULL)
						continue;
					snprintf(out_path, sizeof(out_path), "%s/%s", tree_dir, rel);
					fp = fopen(out_path, "wb");
					if (fp != NULL) {
						if (fwrite(decoded, 1, (size_t)dn, fp) != (size_t)dn)
							ok = 0;
						fclose(fp);
					}
				}
				if (run_cmd("tar -cf '%s/api/v1/repos/%s/archive/main.tar.gz' -C '%s/tree/%.2s' "
				            "'%s'",
				            forge_dir, repos[1][1], forge_dir, repos[1][1], repos[1][1] + 3) != 0 ||
				    sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0 || r.body == NULL ||
				    strstr(r.body, "\"name\":\"srcb\",\"fetched\":true") == NULL) {
					fprintf(stderr, "FAIL: ADR-0324 a tree carrying its signed index must be "
					                "accepted: %s\n",
					        r.body != NULL ? r.body : "(no body)");
					ok = 0;
				}
				cix_response_free(&r);

				/* 4. A recipe the index does not list: refused, never stored. */
				cpdl_recipe_text_decl(recipe, sizeof(recipe), "unlisted", "1.0",
				                      test_http_src(tarball), sha, NULL,
				                      "            tool \"bash\"\n"
				                      "            tool \"coreutils\"\n",
				                      NULL, "", "        run \"true\" {\n        }\n",
				                      "        mkdir \"${dest}/usr/share/unlisted\" parents\n");
				snprintf(path, sizeof(path), "%s/recipes/package/unlisted@1.0-1.cbs", tree_dir);
				{
					FILE *fp = fopen(path, "w");

					if (fp != NULL) {
						fputs(recipe, fp);
						fclose(fp);
					}
				}
				memset(&r, 0, sizeof(r));
				cix_client_request(&client, "PUT", "/v1/pkg/source-ownership",
				                   "{\"item\":\"package:unlisted\",\"source\":\"srcb\"}", &r);
				cix_response_free(&r);
				if (run_cmd("tar -cf '%s/api/v1/repos/%s/archive/main.tar.gz' -C '%s/tree/%.2s' "
				            "'%s'",
				            forge_dir, repos[1][1], forge_dir, repos[1][1], repos[1][1] + 3) != 0 ||
				    sync_and_wait(&client, st_s, sizeof(st_s), &r) != 0) {
					fprintf(stderr, "FAIL: ADR-0324 the tampered-tree sync did not run\n");
					ok = 0;
				}
				cix_response_free(&r);
				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/pkg/recipes/unlisted", NULL, &r) != 0 ||
				    r.status != 404) {
					fprintf(stderr, "FAIL: ADR-0324 a recipe the signed index does not list "
					                "must not be stored, got %d\n",
					        r.status);
					ok = 0;
				}
				cix_response_free(&r);

				memset(&r, 0, sizeof(r));
				cix_client_request(&client, "DELETE", "/v1/system/catalogue-key", NULL, &r);
				cix_response_free(&r);
				json_free(sentj);
				free(req);
				free(pub);
			}
		}

		/*
		 * ADR-0326: upstream keys travel in the catalogue. srca carries
		 * three: synca's key under its own fingerprint, the same key
		 * filed under a fingerprint that is not its own, and a key for
		 * syncb, which srcb owns. Untrusted for keys, srca supplies
		 * none; trusted, only the first is adopted. A catalogue key
		 * cannot be deleted by an operator, and leaves when its file
		 * leaves git.
		 */
		if (stage_ok) {
			static const char ZERO_FPR[] = "0000000000000000000000000000000000000000";
			char st_k[32], ktree[PATH_MAX], kfile[PATH_MAX + 128], kdel[160];
			const char *files[3][2] = { { "synca", TEST_FPR },
				                     { "synca", ZERO_FPR },
				                     { "syncb", TEST_FPR } };
			FILE *fp;

			snprintf(ktree, sizeof(ktree), "%s/tree/%s/recipes/keys", forge_dir, repos[0][1]);
			if (run_cmd("mkdir -p '%s'", ktree) != 0) {
				fprintf(stderr, "FAIL: ADR-0326 could not make recipes/keys\n");
				ok = 0;
			}
			for (i = 0; i < 3; i++) {
				snprintf(kfile, sizeof(kfile), "%s/%s@%s.asc", ktree, files[i][0], files[i][1]);
				fp = fopen(kfile, "w");
				if (fp == NULL || fputs(TEST_KEY, fp) < 0) {
					fprintf(stderr, "FAIL: ADR-0326 could not write %s\n", kfile);
					ok = 0;
				}
				if (fp != NULL)
					fclose(fp);
			}
			if (run_cmd("tar -cf '%s/api/v1/repos/%s/archive/main.tar.gz' -C '%s/tree/%.2s' '%s'",
			            forge_dir, repos[0][1], forge_dir, repos[0][1], repos[0][1] + 3) != 0 ||
			    sync_and_wait(&client, st_k, sizeof(st_k), &r) != 0 ||
			    !str_eq(st_k, "success")) {
				fprintf(stderr, "FAIL: ADR-0326 the sync with keys ended %s\n", st_k);
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/synca/upstream-keys", NULL, &r) != 0 ||
			    r.status != 200 || r.body == NULL || strstr(r.body, TEST_FPR) != NULL) {
				fprintf(stderr, "FAIL: ADR-0326 a source not trusted for keys must supply "
				                "none: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);

			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "PUT", "/v1/pkg/sources/srca",
			                       "{\"trust_keys\":true}", &r) != 0 ||
			    r.status != 200) {
				fprintf(stderr, "FAIL: ADR-0326 trusting srca for keys, status=%d\n", r.status);
				ok = 0;
			}
			cix_response_free(&r);
			if (sync_and_wait(&client, st_k, sizeof(st_k), &r) != 0 || !str_eq(st_k, "success")) {
				fprintf(stderr, "FAIL: ADR-0326 the trusted sync ended %s\n", st_k);
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/synca/upstream-keys", NULL, &r) != 0 ||
			    r.body == NULL || strstr(r.body, TEST_FPR) == NULL ||
			    strstr(r.body, "\"source\":\"srca\"") == NULL || strstr(r.body, ZERO_FPR) != NULL) {
				fprintf(stderr, "FAIL: ADR-0326 a trusted source's key must be adopted under "
				                "its own fingerprint only: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/syncb/upstream-keys", NULL, &r) != 0 ||
			    r.body == NULL || strstr(r.body, TEST_FPR) != NULL) {
				fprintf(stderr, "FAIL: ADR-0326 srca must not vouch for syncb, which srcb "
				                "owns: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);
			snprintf(kdel, sizeof(kdel), "/v1/pkg/synca/upstream-keys/%s", TEST_FPR);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "DELETE", kdel, NULL, &r) != 0 || r.status != 409) {
				fprintf(stderr, "FAIL: ADR-0326 deleting a catalogue key must be 409, got %d\n",
				        r.status);
				ok = 0;
			}
			cix_response_free(&r);

			/* Removed from git: gone at the next sync. */
			snprintf(kfile, sizeof(kfile), "%s/synca@%s.asc", ktree, TEST_FPR);
			unlink(kfile);
			if (run_cmd("tar -cf '%s/api/v1/repos/%s/archive/main.tar.gz' -C '%s/tree/%.2s' '%s'",
			            forge_dir, repos[0][1], forge_dir, repos[0][1], repos[0][1] + 3) != 0 ||
			    sync_and_wait(&client, st_k, sizeof(st_k), &r) != 0 ||
			    !str_eq(st_k, "success")) {
				fprintf(stderr, "FAIL: ADR-0326 the sync after removing the key ended %s\n", st_k);
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "GET", "/v1/pkg/synca/upstream-keys", NULL, &r) != 0 ||
			    r.body == NULL || strstr(r.body, TEST_FPR) != NULL) {
				fprintf(stderr, "FAIL: ADR-0326 a key removed from git must leave at the next "
				                "sync: %s\n",
				        r.body != NULL ? r.body : "(no body)");
				ok = 0;
			}
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "PUT", "/v1/pkg/sources/srca", "{\"trust_keys\":false}",
			                   &r);
			cix_response_free(&r);
		}

		for (i = 0; i < 2; i++) {
			char dpath[128];

			snprintf(dpath, sizeof(dpath), "/v1/pkg/sources/%s", repos[i][0]);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", dpath, NULL, &r);
			cix_response_free(&r);
		}
		if (forge_pid > 0)
			test_http_server_stop(forge_pid);
	}

	/*
	 * #551: a stored image version with the wrong content must not come
	 * back when its package set recurs.
	 *
	 * An image version is named by its package set (ADR-0108), and a set
	 * seen before reuses the tree first stored under that name
	 * (ADR-0155). Chains before #531's fix stored trees missing files --
	 * jumpbox's login without /bin/login -- and every later install of
	 * the same set threw the correct fresh tree away in their favour, so
	 * the post-install check failed the install. Reproduced here: install,
	 * delete one of the package's files straight out of the stored tree,
	 * uninstall, and reinstall the same version, whose set hashes back to
	 * that stored tree. It must install, with the file back.
	 */
	{
		char sha[65];
		char tarball[PATH_MAX];
		char img_dir[PATH_MAX], version[128], victim[PATH_MAX];
		char st_s[64];
		struct stat vst;
		int round;
		int stage_ok;

		snprintf(img_dir, sizeof(img_dir), "%s/rebuildable/images/img551", g_data_dir);
		stage_ok = stage_fixture_tarball(scratch_dir, "dedup551", "1.0", tarball,
		                                 sizeof(tarball), sha, sizeof(sha)) == 0 &&
		           write_shared_path_recipe(&client, "dedup551", "1.0", tarball, sha,
		                                    "usr/share/shared175/d551.txt", NULL) == 0 &&
		           create_image(&client, "img551") == 0;
		if (!stage_ok) {
			fprintf(stderr, "FAIL: #551 could not set up dedup551 in img551\n");
			ok = 0;
		}
		for (round = 0; stage_ok && round < 2; round++) {
			memset(&r, 0, sizeof(r));
			if (cix_client_request(&client, "POST", "/v1/pkg/install",
			                       "{\"name\":\"dedup551\",\"image\":\"img551\"}", &r) != 0 ||
			    (r.status != 202 && r.status != 200)) {
				fprintf(stderr, "FAIL: #551 install round %d status=%d\n", round, r.status);
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
			if (stage_ok &&
			    (poll_pkg_state(&client, "dedup551@img551", st_s, sizeof(st_s), 240) != 0 ||
			     !str_eq(st_s, "installed"))) {
				fprintf(stderr, "FAIL: #551 install round %d ended %s -- %s\n", round, st_s,
				        round == 1 ? "the wrong stored tree was reused" : "setup");
				ok = 0;
				stage_ok = 0;
			}
			if (!stage_ok)
				break;
			if (test_image_fixture_read_current_version(img_dir, version, sizeof(version)) != 0) {
				fprintf(stderr, "FAIL: #551 img551 has no current version\n");
				ok = 0;
				break;
			}
			snprintf(victim, sizeof(victim), "%s/%s/rootfs/usr/bin/dedup551", img_dir, version);
			if (round == 1) {
				if (stat(victim, &vst) != 0) {
					fprintf(stderr, "FAIL: #551 %s is missing after the reinstall\n", victim);
					ok = 0;
				}
				break;
			}
			/* Round 0: damage the stored tree the way #531 did, then
			 * uninstall so the reinstall's set hashes back to it. */
			if (unlink(victim) != 0) {
				fprintf(stderr, "FAIL: #551 could not remove %s: %s\n", victim, strerror(errno));
				ok = 0;
				break;
			}
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", "/v1/pkg/dedup551@img551", NULL, &r);
			if (r.status != 204 && r.status != 200) {
				fprintf(stderr, "FAIL: #551 uninstall status=%d\n", r.status);
				ok = 0;
				stage_ok = 0;
			}
			cix_response_free(&r);
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
		char state[64];
		char zz_scratch[] = "/tmp/cix_test_186_XXXXXX";
		char zz_tarball[512];
		char zz_sha[128];
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

		if (ok186) {
			char zz_srcdir[160];
			char zz_build[512];

			fixture_srcdir(zz_tarball, zz_srcdir, sizeof(zz_srcdir));
			snprintf(zz_build, sizeof(zz_build),
			         "        cd \"${src}/zzlibc/%s\" {\n"
			         "            run \"tcc\" {\n"
			         "                \"-o\" \"hello\" \"hello.c\"\n"
			         "            }\n"
			         "        }\n",
			         zz_srcdir);
			if (publish_cpdl_recipe(&client, "zzlibc", "1.0", zz_tarball, zz_sha, NULL,
			                         CPDL_STD_TOOLS, NULL, zz_build,
			                         "        mkdir \"${dest}/lib\" chmod 0755\n"
			                         "        mkdir \"${dest}/lib/x86_64-linux-gnu\" chmod 0755\n"
			                         "        write \"${dest}/lib/x86_64-linux-gnu/libc.so.6\" "
			                         "\"not-a-real-libc\\n\"\n") != 0) {
				fprintf(stderr, "FAIL: #186 could not publish the zzlibc recipe\n");
				ok = 0;
				ok186 = 0;
			}
		}

		if (ok186 && create_image(&client, "libcollide") != 0) {
			ok = 0;
			ok186 = 0;
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
			char co_srcdir[160];
			char co_build[512], co_install[512];

			fixture_srcdir(zz_tarball, co_srcdir, sizeof(co_srcdir));
			snprintf(co_build, sizeof(co_build),
			         "        cd \"${src}/collide/%s\" {\n"
			         "            run \"tcc\" {\n"
			         "                \"-o\" \"hello\" \"hello.c\"\n"
			         "            }\n"
			         "        }\n",
			         co_srcdir);
			snprintf(co_install, sizeof(co_install),
			         "        mkdir \"${dest}/usr/bin\" chmod 0755\n"
			         "        copy \"${src}/collide/%s/hello\" to \"${dest}/usr/bin/collide\"\n",
			         co_srcdir);
			/* zzlibc among the build tools is the point of #186: this
			 * package is built against a libc another package owns. */
			if (publish_cpdl_recipe(&client, "collide", "1.0", zz_tarball, zz_sha, NULL,
			                         CPDL_STD_TOOLS "            tool \"zzlibc\"\n", NULL,
			                         co_build, co_install) != 0) {
				fprintf(stderr, "FAIL: #186 could not publish the collide recipe\n");
				ok = 0;
				ok186 = 0;
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
		/* A build that would never finish on its own -- #213 cancels
		 * it. coreutils is declared, so `sleep` is really there. */
		if (okc && publish_cpdl_recipe(&client, "sleeper", "1.0", c_tarball, c_sha, NULL,
		                                CPDL_STD_TOOLS, NULL,
		                                "        run \"sleep\" {\n"
		                                "            \"600\"\n"
		                                "        }\n",
		                                "        mkdir \"${dest}/usr/share\" chmod 0755\n") != 0) {
			fprintf(stderr, "FAIL: #213 could not publish the sleeper recipe\n");
			ok = 0;
			okc = 0;
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
