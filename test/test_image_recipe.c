/*
 * Proves Part 4 of the pkg/ redesign (task #769, ADR-0123): image
 * recipes -- a declarative package-list definition for an image,
 * git-syncable text -- and the image-artifact fast path: a fully-
 * pinned recipe with a matching configured plain-HTTP artifact server
 * fetches one whole-rootfs tarball, checksum-verified against the
 * recipe's own declared hash, instead of running any per-package
 * build. Kept hermetic like test_pkg_cache.c's own fixture: the shared
 * loopback server stands in for the artifact host, a real tar
 * builds the fixture payload.
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

#define TEST_PORT 7653
#define PORT_ARG "--port=7653"
static int g_http_port; /* assigned by test_http_server_start() */

static char g_data_dir[PATH_MAX];

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

/* Matches image_hash_manifest_string()'s own algorithm exactly
 * (sha256sum of the canonical, no-trailing-newline "name@version,..."
 * string) -- computed independently here so the test can predict the
 * artifact URL the daemon will request, without any daemon-side
 * cooperation. */
static int compute_manifest_hash(const char *canonical, char *out_hash, size_t out_hash_size)
{
	char tmp_path[] = "/tmp/cix_test_imgrecipe_hash_XXXXXX";
	int fd;
	ssize_t written;
	int rc;

	fd = mkstemp(tmp_path);
	if (fd < 0)
		return -1;
	written = write(fd, canonical, strlen(canonical));
	close(fd);
	if (written < 0 || (size_t)written != strlen(canonical)) {
		unlink(tmp_path);
		return -1;
	}
	rc = compute_file_sha256(tmp_path, out_hash, out_hash_size);
	unlink(tmp_path);
	return rc;
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

/*
 * The shared loopback file server (test_image_fixture.c), rooted at dir.
 * This was `python3 -m http.server`, and a Cix build environment has no
 * Python, so the test failed before reaching what it tests
 * (cix-tests@v2.57.246-1, 192.168.15.95, 2026-09-23).
 */
static pid_t start_http_server(const char *dir)
{
	pid_t pid;

	if (test_http_server_start(dir, 0, &g_http_port, &pid) != 0) {
		perror("test_http_server_start");
		return -1;
	}
	return pid;
}

static int stop_http_server(pid_t pid)
{
	return test_http_server_stop(pid);
}

int main(void)
{
	pid_t daemon_pid, http_pid;
	struct cix_client client;
	struct cix_response r;
	char scratch_dir[] = "/tmp/cix_test_imgrecipe_XXXXXX";
	char artifact_path[512], artifact_sha256[128];
	char target_hash[128];
	char version_before[128];

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	if (mkdtemp(scratch_dir) == NULL) {
		fprintf(stderr, "FAIL: mkdtemp\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	if (run_cmd("mkdir -p '%s/images'", scratch_dir) != 0) {
		fprintf(stderr, "FAIL: could not create images subdir\n");
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

	/* --- scenario 1: fresh recipe list is empty --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/images/recipes", NULL, &r) == 0 && r.status == 200,
	      "GET /v1/images/recipes (fresh)");
	if (r.json != NULL) {
		const struct json_value *recipes = json_object_get(r.json, "recipes");

		CHECK(recipes != NULL && recipes->type == JSON_ARRAY && recipes->u.array.count == 0,
		      "fresh recipe list is empty");
	}
	cix_response_free(&r);

	/* --- scenario 2: bulk-declare (no artifact tier) --- */
	{
		struct json_writer w;

		CHECK(cix_client_request(&client, "POST", "/v1/images", "{\"name\":\"bulkimg\"}", &r) == 0 &&
		          r.status == 201,
		      "image create bulkimg");
		cix_response_free(&r);

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "bulkimg");
		jw_key(&w, "content");
		jw_str(&w, "image_packages=\"foo:pinned:1.0 bar:rolling:2.0\"\n");
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		CHECK(cix_client_request(&client, "POST", "/v1/images/recipes", w.buf, &r) == 0 &&
		          r.status == 204,
		      "POST /v1/images/recipes (bulkimg)");
		jw_free(&w);
		cix_response_free(&r);
	}

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/images/recipes", NULL, &r) == 0 && r.status == 200,
	      "GET /v1/images/recipes (one entry)");
	if (r.json != NULL) {
		const struct json_value *recipes = json_object_get(r.json, "recipes");

		CHECK(recipes != NULL && recipes->type == JSON_ARRAY && recipes->u.array.count == 1,
		      "recipe list has exactly one entry after add");
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/images/recipes/bulkimg", NULL, &r) == 0 &&
	          r.status == 200,
	      "GET /v1/images/recipes/bulkimg");
	if (r.json != NULL) {
		const char *c = json_str_field(r.json, "content");

		CHECK(c != NULL && strstr(c, "foo:pinned:1.0") != NULL, "recipe content round-trips");
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/images/bulkimg/apply-recipe", NULL, &r) == 0 &&
	          r.status == 204,
	      "apply-recipe bulkimg is synchronous (204, no artifact configured)");
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/images/bulkimg", NULL, &r) == 0 && r.status == 200,
	      "GET /v1/images/bulkimg after apply");
	if (r.json != NULL) {
		const struct json_value *manifest = json_object_get(r.json, "manifest");

		CHECK(manifest != NULL && manifest->type == JSON_ARRAY && manifest->u.array.count == 2,
		      "bulk-declared manifest has both entries");
	}
	cix_response_free(&r);

	/* Genuine "no build happened" proof: current_version is still the
	 * empty-manifest hash (sha256 of ""), since bulk-declare never
	 * touches the rootfs -- packages still need real `pkg install`
	 * calls to actually be realized, exactly like a manual manifest
	 * edit already requires (v1 scope, pkg.h's own doc comment). */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/images/bulkimg", NULL, &r) == 0, "re-fetch bulkimg");
	if (r.json != NULL) {
		const char *cv = json_str_field(r.json, "current_version");

		CHECK(cv != NULL &&
		          strcmp(cv, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") ==
		              0,
		      "bulk-declare never produced a new rootfs version");
	}
	cix_response_free(&r);

	/* --- scenario 3: unknown recipe apply is 404 --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/images/noimg/apply-recipe", NULL, &r) == 0 &&
	          r.status == 404,
	      "apply-recipe on an image with no stored recipe is 404");
	cix_response_free(&r);

	/* --- scenario 4: invalid recipe content is rejected --- */
	{
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "badimg");
		jw_key(&w, "content");
		jw_str(&w, "image_packages=\"not-a-valid-entry\"\n");
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		CHECK(cix_client_request(&client, "POST", "/v1/images/recipes", w.buf, &r) == 0 &&
		          r.status == 400,
		      "malformed image_packages entry is rejected (400)");
		jw_free(&w);
		cix_response_free(&r);
	}

	/* --- scenario 5: fully-pinned recipe + artifact fast path --- */
	{
		char payload_dir[600];

		snprintf(payload_dir, sizeof(payload_dir), "%s/payload/usr/bin", scratch_dir);
		CHECK(run_cmd("mkdir -p '%s'", payload_dir) == 0, "mkdir payload dir");
		CHECK(run_cmd("printf '#!/usr/bin/env sh\\necho hi\\n' > '%s/imgtool' && chmod +x "
		              "'%s/imgtool'",
		              payload_dir, payload_dir) == 0,
		      "write payload binary");

		CHECK(compute_manifest_hash("artifactpkg@1.0", target_hash, sizeof(target_hash)) == 0,
		      "compute expected manifest hash");

		snprintf(artifact_path, sizeof(artifact_path), "%s/images/artifactimg-%s.tar.gz",
		         scratch_dir, target_hash);
		CHECK(run_cmd("tar -C '%s/payload' -czf '%s' .", scratch_dir, artifact_path) == 0,
		      "tar the artifact payload");
		CHECK(compute_file_sha256(artifact_path, artifact_sha256, sizeof(artifact_sha256)) == 0,
		      "compute artifact sha256");
	}

	http_pid = start_http_server(scratch_dir);
	CHECK(http_pid > 0, "start http server");
	usleep(300000);

	{
		char base_url[64];
		struct json_writer w;

		snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", g_http_port);
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "base_url");
		jw_str(&w, base_url);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		CHECK(cix_client_request(&client, "PUT", "/v1/pkg/artifact-config", w.buf, &r) == 0 &&
		          r.status == 200,
		      "PUT /v1/pkg/artifact-config");
		jw_free(&w);
		cix_response_free(&r);
	}

	CHECK(cix_client_request(&client, "POST", "/v1/images", "{\"name\":\"artifactimg\"}", &r) == 0 &&
	          r.status == 201,
	      "image create artifactimg");
	cix_response_free(&r);

	{
		char recipe_content[300];
		struct json_writer w;

		snprintf(recipe_content, sizeof(recipe_content),
		         "image_packages=\"artifactpkg:pinned:1.0\"\nimage_artifact_sha256=%s\n",
		         artifact_sha256);
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "artifactimg");
		jw_key(&w, "content");
		jw_str(&w, recipe_content);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		CHECK(cix_client_request(&client, "POST", "/v1/images/recipes", w.buf, &r) == 0 &&
		          r.status == 204,
		      "POST /v1/images/recipes (artifactimg)");
		jw_free(&w);
		cix_response_free(&r);
	}

	/*
	 * ADR-0209: everything the old fast path needed is deliberately in
	 * place here -- every entry pinned, a real image_artifact_sha256
	 * in the recipe, a configured artifact server, and a genuine,
	 * correctly-named, correctly-checksummed tarball actually being
	 * served. The point is that none of it matters any more. Apply is
	 * bulk-declare and nothing else.
	 *
	 * Asserted as absence, not just as a different status code: an
	 * available artifact that is quietly fetched anyway would still
	 * return 204 here, so the checks below look at what is on disk.
	 */
	/* Captured before, compared after: "current_version did not change"
	 * is the property, and asserting it positively beats asserting the
	 * version merely differs from the artifact's hash -- which any
	 * wrong value would satisfy. */
	{
		const char *cv;

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "GET", "/v1/images/artifactimg", NULL, &r) == 0 &&
		          r.status == 200,
		      "GET /v1/images/artifactimg before apply");
		cv = r.json != NULL ? json_str_field(r.json, "current_version") : NULL;
		CHECK(cv != NULL, "image has a current_version before apply");
		snprintf(version_before, sizeof(version_before), "%s", cv != NULL ? cv : "");
		cix_response_free(&r);
	}

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/images/artifactimg/apply-recipe", NULL, &r) == 0 &&
	          r.status == 204,
	      "apply-recipe is synchronous (204) even with a matching artifact available");
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/images/artifactimg", NULL, &r) == 0 &&
	          r.status == 200,
	      "GET /v1/images/artifactimg after apply");
	if (r.json != NULL) {
		const char *cv = json_str_field(r.json, "current_version");
		const struct json_value *manifest = json_object_get(r.json, "manifest");

		CHECK(manifest != NULL && manifest->type == JSON_ARRAY && manifest->u.array.count == 1,
		      "apply declared the manifest");
		CHECK(cv != NULL && strcmp(cv, version_before) == 0,
		      "no new version was produced -- current_version is unchanged");
		CHECK(cv != NULL && strcmp(cv, target_hash) != 0,
		      "current_version is not the artifact's own manifest hash");
	}
	cix_response_free(&r);

	/* Declaring is not installing: bulk-declare writes a manifest, and
	 * the package still needs a real install to exist anywhere. */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/pkg/artifactpkg@artifactimg", NULL, &r) == 0 &&
	          r.status == 404,
	      "a declared package is not reported installed");
	cix_response_free(&r);

	/* The artifact's own payload must be nowhere on disk. */
	{
		char check_path[PATH_MAX];

		snprintf(check_path, sizeof(check_path), "%s/rebuildable/images/artifactimg/%s/rootfs/usr/bin/imgtool",
		         g_data_dir, target_hash);
		CHECK(access(check_path, F_OK) != 0,
		      "the artifact was never fetched or extracted");
	}

	/* --- scenario 6: recipe rm --- */
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "DELETE", "/v1/images/recipes/bulkimg", NULL, &r) == 0 &&
	          r.status == 204,
	      "DELETE /v1/images/recipes/bulkimg");
	cix_response_free(&r);
	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "GET", "/v1/images/recipes/bulkimg", NULL, &r) == 0 &&
	          r.status == 404,
	      "GET removed recipe is 404");
	cix_response_free(&r);

	/*
	 * Issue #97: the declared-vs-installed reconciliation. What matters
	 * is that it tells the three states apart -- "installed with no
	 * recipe" especially, since that one is invisible everywhere else
	 * and means the thing cannot be rebuilt from source control. Found
	 * live: 11 images on the real box, 8 recipes, and two of the three
	 * orphans were debris nobody had noticed.
	 *
	 * Both states are created here rather than assumed from whatever
	 * else this test happens to have left lying around.
	 */
	{
		int saw_orphan = 0, saw_unapplied = 0;
		size_t k;

		/* An image with no recipe. */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "POST", "/v1/images", "{\"name\":\"recon-orphan\"}", &r);
		cix_response_free(&r);

		/* A recipe nobody has applied. */
		memset(&r, 0, sizeof(r));
		cix_client_request(&client, "POST", "/v1/images/recipes",
		                   "{\"name\":\"recon-unapplied\",\"content\":\"image_name=\\\"recon-unapplied\\\""
		                   "\\nimage_packages=\\\"tcc:rolling:0.9.27\\\"\\n\"}",
		                   &r);
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "GET", "/v1/software", NULL, &r) == 0 && r.status == 200,
		      "GET /v1/software reconciles declared against installed");
		{
			const struct json_value *list = json_object_get(r.json, "software");

			if (list != NULL && list->type == JSON_ARRAY) {
				for (k = 0; k < list->u.array.count; k++) {
					const struct json_value *e = list->u.array.items[k];
					const char *nm = json_as_string(json_object_get(e, "name"));
					const struct json_value *de = json_object_get(e, "declared");
					const struct json_value *in = json_object_get(e, "installed");
					int d = de != NULL && de->type == JSON_BOOL && de->u.boolean;
					int i2 = in != NULL && in->type == JSON_BOOL && in->u.boolean;

					if (nm == NULL)
						continue;
					if (strcmp(nm, "recon-orphan") == 0 && !d && i2)
						saw_orphan = 1;
					if (strcmp(nm, "recon-unapplied") == 0 && d && !i2)
						saw_unapplied = 1;
				}
			}
		}
		cix_response_free(&r);
		CHECK(saw_orphan, "an image with no recipe is reported as installed-but-not-declared -- the "
		                  "state that means it cannot be rebuilt from source control");
		CHECK(saw_unapplied, "a recipe nobody has applied is reported as declared-but-not-installed");
	}


	stop_http_server(http_pid);
	CHECK(stop_daemon(daemon_pid) == 0, "daemon shut down cleanly");
	run_cmd("rm -rf '%s'", scratch_dir);
	test_data_dir_cleanup(g_data_dir);

	if (g_failures > 0) {
		fprintf(stderr, "test_image_recipe: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_image_recipe: OK\n");
	return 0;
}
