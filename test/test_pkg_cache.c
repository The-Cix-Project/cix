/*
 * Proves Part 3 of the pkg/ redesign (task #768, ADR-0122): the local
 * build-artifact cache + LRU eviction, and the plain-HTTP precompiled-
 * artifact server tier, both feeding into the exact same cache-hit
 * install path (a real, unmodified build container whose own upperdir
 * is pre-populated before it ever runs, see pkg.c's own comment).
 * Kept hermetic like test_pkg.c's own fixture: real gcc compiles a
 * real tiny C program via file:// sources, and a real python3
 * http.server stands in for a plain artifact host.
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

#define TEST_PORT 7649
#define PORT_ARG "--port=7649"
#define HTTP_PORT 17650
/* Issue #129: the push receiver -- a separate port from the pull
 * server above, so a push can never be mistaken for a pull. */
#define PUSH_PORT 17651

static char g_data_dir[PATH_MAX];
static char g_pkg_state_dir[PATH_MAX];
static char g_cache_dir[PATH_MAX];

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

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
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

static int wait_for_pkg_state(const struct cix_client *c, const char *name, const char *image,
                               const char *want, int max_attempts)
{
	int i;
	char path[256];

	if (image != NULL)
		snprintf(path, sizeof(path), "/v1/pkg/%s@%s", name, image);
	else
		snprintf(path, sizeof(path), "/v1/pkg/%s", name);
	for (i = 0; i < max_attempts; i++) {
		struct cix_response r;
		const char *state;
		int matched;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", path, NULL, &r) != 0 || r.status != 200) {
			cix_response_free(&r);
			return -1;
		}
		state = json_str_field(r.json, "state");
		matched = state != NULL && strcmp(state, want) == 0;
		cix_response_free(&r);
		if (matched)
			return 0;
		usleep(100000);
	}
	return -1;
}

/* Real gcc "hello" fixture (name embedded in both source and output
 * filename), tarred with the standard <name>-<version>/ wrapper. */
static int stage_source_tarball(const char *scratch_dir, const char *name, const char *version,
                                 char *out_tarball_path, size_t tarball_path_size, char *out_sha256,
                                 size_t sha256_size)
{
	char src_dir[512], hello_c[600], makefile[600];
	FILE *f;

	snprintf(src_dir, sizeof(src_dir), "%s/%s-%s", scratch_dir, name, version);
	if (run_cmd("mkdir -p '%s'", src_dir) != 0)
		return -1;

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
	fclose(f);

	snprintf(out_tarball_path, tarball_path_size, "%s/%s-%s.tarball", scratch_dir, name, version);
	if (run_cmd("tar -cf '%s' -C '%s' '%s-%s'", out_tarball_path, scratch_dir, name, version) != 0)
		return -1;
	return compute_file_sha256(out_tarball_path, out_sha256, sha256_size);
}

static int write_recipe(const char *pkg_state_dir, const char *name, const char *version,
                         const char *source_url, const char *source_sha256,
                         const char *artifact_sha256)
{
	char name_dir[256], path[300];
	FILE *f;

	snprintf(name_dir, sizeof(name_dir), "%s/recipes/%s", pkg_state_dir, name);
	run_cmd("mkdir -p '%s'", name_dir);
	snprintf(path, sizeof(path), "%s/%s", name_dir, version);
	run_cmd("mkdir -p '%s'", path);
	snprintf(path, sizeof(path), "%s/recipes/%s/%s/build.sh", pkg_state_dir, name, version);
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "pkg_name=%s\n", name);
	fprintf(f, "pkg_version=%s\n", version);
	fprintf(f, "pkg_source=%s\n", source_url);
	fprintf(f, "pkg_sha256=%s\n", source_sha256);
	fprintf(f, "pkg_depends=\"\"\n");
	fprintf(f, "pkg_build_depends=\"tcc libc-dev bash coreutils\"\n");
	if (artifact_sha256 != NULL && artifact_sha256[0] != '\0')
		fprintf(f, "pkg_artifact_sha256=%s\n", artifact_sha256);
	fprintf(f, "\npkg_build() {\n\ttcc -o hello hello.c\n}\n\n");
	fprintf(f, "pkg_install() {\n\tmkdir -p \"$PKG_DESTDIR/usr/bin\"\n\tcp hello "
	           "\"$PKG_DESTDIR/usr/bin/%s\"\n}\n",
	        name);
	fclose(f);
	return 0;
}

static long cache_json_long(const struct json_value *v, const char *key)
{
	return (long)json_as_number(json_object_get(v, key));
}

static pid_t start_http_server(const char *dir)
{
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		char port_str[16];
		char *argv[8];

		snprintf(port_str, sizeof(port_str), "%d", HTTP_PORT);
		argv[0] = "python3";
		argv[1] = "-m";
		argv[2] = "http.server";
		argv[3] = port_str;
		argv[4] = "--directory";
		argv[5] = (char *)dir;
		argv[6] = NULL;
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execvp("python3", argv);
		_exit(127);
	}
	return pid;
}

static int stop_http_server(pid_t pid)
{
	int status;

	kill(pid, SIGTERM);
	return waitpid(pid, &status, 0) == pid ? 0 : -1;
}

/*
 * Issue #129: a minimal PUT-accepting artifact server, standing in for
 * the real one. python3 -m http.server cannot do this (GET only), and
 * the point of the test is what actually arrives over the wire, so it
 * records the body plus the two headers the contract depends on:
 * the bearer token and the declared digest.
 *
 * Deliberately dumb -- it verifies nothing itself. The test asserts
 * against what it recorded, so a daemon that sent the wrong digest
 * would be caught here rather than quietly accepted.
 */
static pid_t start_push_server(const char *dir)
{
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		char script[2048];
		char *argv[5];

		snprintf(script, sizeof(script),
		         "import http.server,os\n"
		         "D=%s%s%s\n"
		         "class H(http.server.BaseHTTPRequestHandler):\n"
		         "    def do_PUT(self):\n"
		         "        n=int(self.headers.get('Content-Length','0'))\n"
		         "        b=self.rfile.read(n)\n"
		         "        base=os.path.basename(self.path)\n"
		         "        open(os.path.join(D,base),'wb').write(b)\n"
		         "        open(os.path.join(D,base+'.headers'),'w').write(\n"
		         "            (self.headers.get('Authorization') or '')+chr(10)+\n"
		         "            (self.headers.get('X-Cix-Sha256') or '')+chr(10))\n"
		         "        self.send_response(201)\n"
		         "        self.end_headers()\n"
		         "    def log_message(self,*a):\n"
		         "        pass\n"
		         "http.server.HTTPServer(('127.0.0.1',%d),H).serve_forever()\n",
		         "'", dir, "'", PUSH_PORT);
		argv[0] = "python3";
		argv[1] = "-c";
		argv[2] = script;
		argv[3] = NULL;
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execvp("python3", argv);
		_exit(127);
	}
	return pid;
}

int main(void)
{
	pid_t daemon_pid, http_pid;
	struct cix_client client;
	struct cix_response r;
	char scratch_dir[] = "/tmp/cix_test_pkgcache_XXXXXX";
	char tarball_path[512], sha256[128];
	char recipe_check_path[PATH_MAX];
	char *content;
	size_t content_len;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;
	/*
	 * ADR-0209: the build floor. Real, recipe-built package artifacts
	 * seeded into this daemon's own cache, so installing them is a
	 * cache hit that needs no build environment -- the same way a fresh
	 * host gets its first packages. There is no shared sandbox to
	 * inherit one from any more, and nothing here is fabricated.
	 */
	if (test_image_fixture_seed_floor_packages(g_data_dir, "build/floor-artifacts") != 0) {
		fprintf(stderr, "could not seed the build floor -- fetch the real package artifacts "
		                "into build/floor-artifacts first (ADR-0209)\n");
		return 1;
	}
	snprintf(g_pkg_state_dir, sizeof(g_pkg_state_dir), "%s/rebuildable/pkg", g_data_dir);
	snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/cache", g_pkg_state_dir);

	if (mkdtemp(scratch_dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (stage_source_tarball(scratch_dir, "cachetest", "1.0", tarball_path, sizeof(tarball_path),
	                          sha256, sizeof(sha256)) != 0) {
		fprintf(stderr, "FAIL: could not stage source fixture\n");
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
		fprintf(stderr, "FAIL: daemon never became healthy\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	{
		static const char *const floor[] = { "bash", "coreutils", "tcc", "libc-dev", NULL };
		int fi;

		for (fi = 0; floor[fi] != NULL; fi++) {
			char fbody[128];
			int fr;

			snprintf(fbody, sizeof(fbody), "{\"name\":\"%s\"}", floor[fi]);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "POST", "/v1/pkg/install", fbody, &r);
			cix_response_free(&r);
			for (fr = 0; fr < 600; fr++) {
				const char *st = NULL;

				memset(&r, 0, sizeof(r));
				snprintf(fbody, sizeof(fbody), "/v1/pkg/%s", floor[fi]);
				if (cix_client_request(&client, "GET", fbody, NULL, &r) == 0 && r.json != NULL)
					st = json_str_field(r.json, "state");
				if (st != NULL && strcmp(st, "installed") == 0) {
					cix_response_free(&r);
					break;
				}
				cix_response_free(&r);
				usleep(300000);
			}
		}
		/* The floor has done its job. Its tarballs are ~90 MB and this
		 * test reasons about cache size and LRU eviction, so leaving
		 * them in would change the subject -- and did: they evicted the
		 * very entries under test. */
		if (test_image_fixture_clear_floor_cache(g_data_dir) != 0) {
			fprintf(stderr, "could not clear the floor from the cache\n");
			return 1;
		}
	}

	{
		char source_url[600];

		snprintf(source_url, sizeof(source_url), "file://%s", tarball_path);
		CHECK(write_recipe(g_pkg_state_dir, "cachetest", "1.0", source_url, sha256, NULL) == 0,
		      "write cachetest recipe");
	}

	/* --- scenario 1: fresh cache is empty --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/pkg/cache", NULL, &r) == 0 && r.status == 200,
	      "GET /v1/pkg/cache (fresh)");
	if (r.json != NULL)
		CHECK(cache_json_long(r.json, "entry_count") == 0, "fresh cache has zero entries");
	cix_response_free(&r);

	/* --- scenario 2: a real install (real fetch, real gcc compile)
	 * populates the cache. --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/pkg/install",
	                         "{\"name\":\"cachetest\",\"image\":\"imgA\"}", &r) == 0 &&
	              r.status == 202,
	      "POST /v1/pkg/install cachetest@imgA (real build)");
	cix_response_free(&r);
	CHECK(wait_for_pkg_state(&client, "cachetest", "imgA", "installed", 300) == 0,
	      "cachetest@imgA reaches state=installed");

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/pkg/cache", NULL, &r) == 0 && r.status == 200,
	      "GET /v1/pkg/cache (after real build)");
	if (r.json != NULL) {
		CHECK(cache_json_long(r.json, "entry_count") == 1,
		      "cache has exactly one entry after the real build");
		CHECK(cache_json_long(r.json, "current_bytes") > 0, "cache reports nonzero occupied bytes");
	}
	cix_response_free(&r);

	{
		char cache_tarball[PATH_MAX];

		snprintf(cache_tarball, sizeof(cache_tarball), "%s/cachetest-1.0.tar.gz", g_cache_dir);
		CHECK(access(cache_tarball, F_OK) == 0, "cache tarball actually exists on disk");
	}

	/* --- scenario 3: prove the SECOND install (different image, same
	 * name+version) is a genuine cache hit, not a lucky re-fetch --
	 * delete the real source tarball first so a real fetch would fail. */
	CHECK(unlink(tarball_path) == 0, "delete the real source tarball before the second install");

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/pkg/install",
	                         "{\"name\":\"cachetest\",\"image\":\"imgB\"}", &r) == 0 &&
	              r.status == 202,
	      "POST /v1/pkg/install cachetest@imgB (cache hit -- source tarball no longer exists)");
	cix_response_free(&r);
	CHECK(wait_for_pkg_state(&client, "cachetest", "imgB", "installed", 300) == 0,
	      "cachetest@imgB reaches state=installed via cache hit despite missing source");

	/* --- scenario 4: cache-config get/set --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/pkg/cache-config", NULL, &r) == 0 &&
	              r.status == 200,
	      "GET /v1/pkg/cache-config (default)");
	if (r.json != NULL)
		CHECK(cache_json_long(r.json, "max_bytes") == 2147483648L, "default cache cap is 2 GiB");
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "PUT", "/v1/pkg/cache-config", "{\"max_bytes\":104857600}",
	                         &r) == 0 &&
	              r.status == 200,
	      "PUT /v1/pkg/cache-config (100 MiB)");
	if (r.json != NULL)
		CHECK(cache_json_long(r.json, "max_bytes") == 104857600L, "cache cap updated to 100 MiB");
	cix_response_free(&r);

	/* --- scenario 5: LRU eviction -- a cap smaller than the existing
	 * cached entry forces it out when a second, distinct package is
	 * cached. --- */
	{
		char tarball2[512], sha2562[128], source_url2[600];

		if (stage_source_tarball(scratch_dir, "cachetest2", "1.0", tarball2, sizeof(tarball2),
		                          sha2562, sizeof(sha2562)) == 0) {
			snprintf(source_url2, sizeof(source_url2), "file://%s", tarball2);
			CHECK(write_recipe(g_pkg_state_dir, "cachetest2", "1.0", source_url2, sha2562, NULL) ==
			              0,
			      "write cachetest2 recipe");
		}

		/* Cap small enough that only one tiny compiled-hello artifact
		 * fits at a time (~a few KB each), forcing the older
		 * cachetest-1.0 entry to be evicted once cachetest2 is cached. */
		memset(&r, 0, sizeof(r));
		/*
		 * 2048, not 4096. The cap has to admit exactly one artifact and
		 * refuse the second, and the artifacts got smaller: fixture
		 * recipes compile with tcc now rather than the host's gcc
		 * (ADR-0209 -- tcc is what this project builds with, and what
		 * the seeded floor actually contains). A tcc-built hello packs
		 * to ~1.1 KB against gcc's ~2.3 KB, so two of them fit under
		 * 4096 and nothing was ever evicted -- the test passed on
		 * arithmetic that no longer held. Measured, not guessed.
		 */
		CHECK(cix_client_request(&client, "PUT", "/v1/pkg/cache-config", "{\"max_bytes\":2048}",
		                         &r) == 0 &&
		              r.status == 200,
		      "PUT /v1/pkg/cache-config (tiny cap, forces eviction)");
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "POST", "/v1/pkg/install",
		                         "{\"name\":\"cachetest2\",\"image\":\"imgC\"}", &r) == 0 &&
		              r.status == 202,
		      "POST /v1/pkg/install cachetest2@imgC");
		cix_response_free(&r);
		CHECK(wait_for_pkg_state(&client, "cachetest2", "imgC", "installed", 300) == 0,
		      "cachetest2@imgC reaches state=installed");

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "GET", "/v1/pkg/cache", NULL, &r) == 0 && r.status == 200,
		      "GET /v1/pkg/cache (after LRU eviction)");
		if (r.json != NULL) {
			long total = cache_json_long(r.json, "current_bytes");

			CHECK(total <= 2048, "cache stays under its own tiny configured cap after eviction");
		}
		cix_response_free(&r);

		{
			char old_cache_tarball[PATH_MAX];

			snprintf(old_cache_tarball, sizeof(old_cache_tarball), "%s/cachetest-1.0.tar.gz",
			         g_cache_dir);
			CHECK(access(old_cache_tarball, F_OK) != 0,
			      "the older cachetest-1.0 cache entry was actually evicted");
		}
	}

	/* Restore a real cap before the artifact-server scenarios below. */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "PUT", "/v1/pkg/cache-config", "{\"max_bytes\":104857600}",
	                         &r) == 0 &&
	              r.status == 200,
	      "PUT /v1/pkg/cache-config (restore 100 MiB before artifact scenarios)");
	cix_response_free(&r);

	/* --- scenario 6: cache-clear --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "DELETE", "/v1/pkg/cache", NULL, &r) == 0 && r.status == 204,
	      "DELETE /v1/pkg/cache");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/pkg/cache", NULL, &r) == 0 && r.status == 200,
	      "GET /v1/pkg/cache (after clear)");
	if (r.json != NULL)
		CHECK(cache_json_long(r.json, "entry_count") == 0, "cache is empty after clear");
	cix_response_free(&r);

	/* --- scenario 7: network artifact-fetch tier -- a recipe with a
	 * declared pkg_artifact_sha256, an intentionally-broken pkg_source,
	 * and a real artifact server: install must succeed via the
	 * artifact, never touching the broken source at all. --- */
	{
		char artifact_stage_dir[PATH_MAX];
		char artifact_bin_dir[PATH_MAX];
		char artifact_c[PATH_MAX], artifact_bin[PATH_MAX];
		char artifact_tarball[PATH_MAX];
		char artifact_sha[128];
		int ok = 1;

		snprintf(artifact_stage_dir, sizeof(artifact_stage_dir), "%s/artifact-serve", scratch_dir);
		snprintf(artifact_bin_dir, sizeof(artifact_bin_dir), "%s/stage/usr/bin", artifact_stage_dir);
		if (run_cmd("mkdir -p '%s'", artifact_bin_dir) != 0)
			ok = 0;
		snprintf(artifact_c, sizeof(artifact_c), "%s/art.c", scratch_dir);
		{
			FILE *f = fopen(artifact_c, "w");

			if (f == NULL) {
				ok = 0;
			} else {
				fputs("#include <stdio.h>\nint main(void){printf(\"hello from "
				      "artifact\\n\");return 0;}\n",
				      f);
				fclose(f);
			}
		}
		snprintf(artifact_bin, sizeof(artifact_bin), "%s/artifacttest", artifact_bin_dir);
		if (run_cmd("gcc -o '%s' '%s'", artifact_bin, artifact_c) != 0)
			ok = 0;

		if (ok) {
			snprintf(artifact_tarball, sizeof(artifact_tarball),
			         "%s/artifacttest-1.0.tar.gz", artifact_stage_dir);
			if (run_cmd("tar -C '%s/stage' -czf '%s' .", artifact_stage_dir, artifact_tarball) !=
			    0)
				ok = 0;
		}
		if (ok && compute_file_sha256(artifact_tarball, artifact_sha, sizeof(artifact_sha)) != 0)
			ok = 0;

		CHECK(ok, "stage artifact-server fixture (compiled binary + tarball)");

		if (ok) {
			http_pid = start_http_server(artifact_stage_dir);
			CHECK(http_pid > 0, "start artifact http.server");
			usleep(300000);

			{
				char artifact_url[256], put_body[512];

				snprintf(artifact_url, sizeof(artifact_url), "http://127.0.0.1:%d", HTTP_PORT);
				snprintf(put_body, sizeof(put_body), "{\"base_url\":\"%s\"}", artifact_url);
				memset(&r, 0, sizeof(r));
				CHECK(cix_client_request(&client, "PUT", "/v1/pkg/artifact-config", put_body, &r) ==
				              0 &&
				              r.status == 200,
				      "PUT /v1/pkg/artifact-config");
				cix_response_free(&r);
			}

			CHECK(write_recipe(g_pkg_state_dir, "artifacttest", "1.0",
			                    "file:///nonexistent/artifacttest-1.0.tar", artifact_sha,
			                    artifact_sha) == 0,
			      "write artifacttest recipe (broken source, real artifact checksum)");
			/* Note: pkg_source's own sha256 is deliberately set to the
			 * ARTIFACT's checksum too, harmlessly unused -- the source
			 * URL itself points at a file that can never be fetched, so
			 * only the artifact tier can possibly succeed here. */

			memset(&r, 0, sizeof(r));
			CHECK(cix_client_request(&client, "POST", "/v1/pkg/install",
			                         "{\"name\":\"artifacttest\",\"image\":\"imgD\"}", &r) == 0 &&
			              r.status == 202,
			      "POST /v1/pkg/install artifacttest@imgD (artifact tier, broken source)");
			cix_response_free(&r);
			CHECK(wait_for_pkg_state(&client, "artifacttest", "imgD", "installed", 300) == 0,
			      "artifacttest@imgD reaches state=installed via the artifact tier");

			memset(&r, 0, sizeof(r));
			CHECK(cix_client_request(&client, "GET", "/v1/pkg/cache", NULL, &r) == 0 &&
			              r.status == 200,
			      "GET /v1/pkg/cache (after artifact-tier install)");
			if (r.json != NULL)
				CHECK(cache_json_long(r.json, "entry_count") == 1,
				      "the verified artifact was promoted into the local cache");
			cix_response_free(&r);

			/*
			 * ---- a cache-hit hostbuild must still run its follow-up ----
			 *
			 * An artifact hit skips the build container entirely (#144),
			 * which is the point. What must NOT be skipped is the work
			 * that follows a build completing -- and for a hostbuild
			 * named "cix" that includes triggering the bootroot
			 * assembly (ADR-0057).
			 *
			 * This regressed exactly once and was invisible: the
			 * cache-hit path chained and pushed but never triggered
			 * assembly, so `pkg hostbuild cix --deploy` reported the
			 * package installed and produced no image at all. On a real
			 * host the only sign was
			 * bootroot_assembly_started_generation staying at 0 while
			 * everything else claimed success.
			 *
			 * Asserted on the generation counter rather than on a
			 * finished image: mkbootroot will fail here (no real
			 * artifacts, no toolchain), and that is fine. The question
			 * is whether the daemon even TRIED, which is precisely what
			 * was lost.
			 */
			{
				long gen_before = -1;
				long gen_after = -1;

				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/system/boot", NULL, &r) == 0 &&
				    r.json != NULL)
					gen_before =
					    cache_json_long(r.json, "bootroot_assembly_started_generation");
				cix_response_free(&r);

				/* The same bytes, published under the name the daemon
				 * will ask for: <base>/<name>-<version>.tar.gz. */
				CHECK(run_cmd("cp '%s' '%s/cix-1.0.tar.gz'", artifact_tarball,
				               artifact_stage_dir) == 0,
				      "publish the cix artifact under its own name");

				CHECK(write_recipe(g_pkg_state_dir, "cix", "1.0",
				                    "file:///nonexistent/cix-1.0.tar", artifact_sha,
				                    artifact_sha) == 0,
				      "write a cix recipe served only by the artifact tier");

				memset(&r, 0, sizeof(r));
				CHECK(cix_client_request(&client, "POST", "/v1/pkg/hostbuild",
				                         "{\"name\":\"cix\",\"build_image\":\"imgD\"}",
				                         &r) == 0 &&
				              r.status == 202,
				      "POST /v1/pkg/hostbuild cix (artifact tier, no build container)");
				cix_response_free(&r);

				CHECK(wait_for_pkg_state(&client, "cix", "__hostbuild", "installed", 300) == 0,
				      "the cix hostbuild reaches state=installed from the artifact");

				memset(&r, 0, sizeof(r));
				if (cix_client_request(&client, "GET", "/v1/system/boot", NULL, &r) == 0 &&
				    r.json != NULL)
					gen_after =
					    cache_json_long(r.json, "bootroot_assembly_started_generation");
				cix_response_free(&r);

				CHECK(gen_after > gen_before,
				      "a cache-hit cix hostbuild still triggers the bootroot assembly "
				      "(started_generation advanced past its pre-install value)");
			}

			stop_http_server(http_pid);
		}
	}

	/*
	 * ---- issue #129: a fresh build publishes itself ----
	 *
	 * The whole point: before this, a built artifact lived only in one
	 * box's local cache and nothing could retrieve it, so every other
	 * host rebuilt it from source. Here a real build must result in a
	 * real PUT arriving at the configured server, under exactly the
	 * name a puller will later ask for, carrying a digest that
	 * actually matches the bytes.
	 */
	{
		char push_dir[PATH_MAX];
		char pushed[PATH_MAX];
		char headers_path[PATH_MAX];
		char push_tarball[512];
		char source_url[600];
		char push_sha256[128];
		pid_t push_pid;
		int ok = 1;
		int i;

		snprintf(push_dir, sizeof(push_dir), "%s/pushed", scratch_dir);
		if (run_cmd("mkdir -p '%s'", push_dir) != 0)
			ok = 0;
		CHECK(ok, "create push receiver directory");

		if (ok && stage_source_tarball(scratch_dir, "pushtest", "1.0", push_tarball,
		                                sizeof(push_tarball), push_sha256,
		                                sizeof(push_sha256)) != 0)
			ok = 0;
		snprintf(source_url, sizeof(source_url), "file://%s", push_tarball);
		CHECK(ok, "stage pushtest source tarball");

		if (ok) {
			push_pid = start_push_server(push_dir);
			CHECK(push_pid > 0, "start push receiver");
			usleep(400000);

			{
				char put_body[640];

				snprintf(put_body, sizeof(put_body),
				         "{\"base_url\":\"http://127.0.0.1:%d\",\"auth_token\":\"tok-129\","
				         "\"push_enabled\":true}",
				         PUSH_PORT);
				memset(&r, 0, sizeof(r));
				CHECK(cix_client_request(&client, "PUT", "/v1/pkg/artifact-config", put_body,
				                         &r) == 0 &&
				              r.status == 200,
				      "PUT /v1/pkg/artifact-config with push_enabled");
				if (r.json != NULL) {
					const struct json_value *pe =
					    json_object_get(r.json, "push_enabled");

					CHECK(pe != NULL && pe->type == JSON_BOOL && pe->u.boolean,
					      "artifact-config reports push_enabled true");
					CHECK(json_object_get(r.json, "auth_token") == NULL,
					      "the token itself is never echoed back");
				}
				cix_response_free(&r);
			}

			CHECK(write_recipe(g_pkg_state_dir, "pushtest", "1.0", source_url, push_sha256,
			                    NULL) == 0,
			      "write pushtest recipe (real source -- must genuinely build)");

			memset(&r, 0, sizeof(r));
			CHECK(cix_client_request(&client, "POST", "/v1/pkg/install",
			                         "{\"name\":\"pushtest\",\"image\":\"imgD\"}", &r) == 0 &&
			              r.status == 202,
			      "POST /v1/pkg/install pushtest@imgD");
			cix_response_free(&r);
			CHECK(wait_for_pkg_state(&client, "pushtest", "imgD", "installed", 300) == 0,
			      "pushtest@imgD reaches state=installed via a real build");

			/* The push is asynchronous by design (it must never block
			 * the event loop), so the arrival is polled, not assumed. */
			snprintf(pushed, sizeof(pushed), "%s/pushtest-1.0.tar.gz", push_dir);
			snprintf(headers_path, sizeof(headers_path), "%s/pushtest-1.0.tar.gz.headers",
			         push_dir);
			for (i = 0; i < 100; i++) {
				if (access(headers_path, R_OK) == 0)
					break;
				usleep(200000);
			}
			CHECK(access(pushed, R_OK) == 0,
			      "the freshly built package arrived at the artifact server");

			if (access(pushed, R_OK) == 0) {
				char received_sha[65];
				char declared_sha[128] = { 0 };
				char auth_line[256] = { 0 };
				FILE *hf = fopen(headers_path, "r");

				if (hf != NULL) {
					if (fgets(auth_line, sizeof(auth_line), hf) != NULL)
						auth_line[strcspn(auth_line, "\r\n")] = '\0';
					if (fgets(declared_sha, sizeof(declared_sha), hf) != NULL)
						declared_sha[strcspn(declared_sha, "\r\n")] = '\0';
					fclose(hf);
				}
				CHECK(strcmp(auth_line, "Bearer tok-129") == 0,
				      "the push carried the configured bearer token");

				/* The digest must describe the bytes that actually
				 * arrived -- a header computed from anything else
				 * would make the receiver's corruption check
				 * meaningless. */
				CHECK(compute_file_sha256(pushed, received_sha, sizeof(received_sha)) == 0,
				      "hash the received artifact");
				CHECK(strcmp(declared_sha, received_sha) == 0,
				      "X-Cix-Sha256 matches the bytes that arrived");

				/* And it must be a real archive, not a truncated
				 * upload that merely hashed consistently. */
				CHECK(run_cmd("gzip -t '%s' 2>/dev/null", pushed) == 0,
				      "the received artifact is a valid gzip");
			}

			/*
			 * A second install of the SAME package must not re-publish:
			 * it is a cache hit, so its bytes already came from here.
			 * Proven by removing the receiver's copy and confirming
			 * nothing puts it back.
			 */
			run_cmd("rm -f '%s' '%s'", pushed, headers_path);
			memset(&r, 0, sizeof(r));
			cix_client_request(&client, "DELETE", "/v1/pkg/pushtest@imgD", NULL, &r);
			cix_response_free(&r);
			memset(&r, 0, sizeof(r));
			CHECK(cix_client_request(&client, "POST", "/v1/pkg/install",
			                         "{\"name\":\"pushtest\",\"image\":\"imgD\"}", &r) == 0 &&
			              r.status == 202,
			      "POST /v1/pkg/install pushtest@imgD again (expect a cache hit)");
			cix_response_free(&r);
			CHECK(wait_for_pkg_state(&client, "pushtest", "imgD", "installed", 300) == 0,
			      "pushtest@imgD reinstalls from cache");
			usleep(1500000);
			CHECK(access(pushed, R_OK) != 0,
			      "a cache hit does NOT re-publish what it did not build");

			kill(push_pid, SIGTERM);
			waitpid(push_pid, NULL, 0);
		}
	}

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);
	run_cmd("rm -rf '%s'", scratch_dir);

	if (g_failures > 0) {
		fprintf(stderr, "test_pkg_cache: %d failure(s)\n", g_failures);
		return 1;
	}
	fprintf(stderr, "test_pkg_cache: OK\n");
	return 0;
}
