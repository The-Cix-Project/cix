/*
 * Proves Part 4 of the pkg/ redesign (task #769, ADR-0123): image
 * recipes -- a declarative package-list definition for an image, a JSON
 * document since ADR-0311 -- are listed, applied (bulk-declare, ADR-0209),
 * validated, reconciled against the images that exist, and removed. The
 * image-artifact fast path this once also proved is gone: ADR-0209
 * retired it and ADR-0328 removed image tarballs, so a JSON recipe has
 * no field that could ask for one.
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

static char g_data_dir[PATH_MAX];

static int g_failures;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s\n", msg); \
			g_failures++; \
		} \
	} while (0)

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

/* A string field, "" when absent, so a missing field fails a CHECK
 * instead of crashing the test in strcmp(). */
static const char *jstr(const struct json_value *obj, const char *key)
{
	const char *s = json_str_field(obj, key);

	return s != NULL ? s : "";
}

/* ADR-0320: the version image's manifest records for package into out,
 * "" when it has none. */
static void manifest_version(const struct cix_client *c, const char *image, const char *package,
                             char *out, size_t out_size)
{
	struct cix_response r;
	char path[128];

	out[0] = '\0';
	memset(&r, 0, sizeof(r));
	snprintf(path, sizeof(path), "/v1/images/%s", image);
	if (cix_client_request(c, "GET", path, NULL, &r) == 0 && r.status == 200 && r.json != NULL) {
		const struct json_value *m = json_object_get(r.json, "manifest");
		size_t i;

		for (i = 0; m != NULL && m->type == JSON_ARRAY && i < m->u.array.count; i++) {
			const char *p = json_str_field(m->u.array.items[i], "package");
			const char *v = json_str_field(m->u.array.items[i], "version");

			if (p != NULL && v != NULL && strcmp(p, package) == 0)
				snprintf(out, out_size, "%s", v);
		}
	}
	cix_response_free(&r);
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

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

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
		jw_str(&w, "{\"packages\":[{\"package\":\"foo\",\"mode\":\"pinned\",\"version\":\"1.0\"},"
		            "{\"package\":\"bar\",\"mode\":\"rolling\",\"version\":\"2.0\"}]}");
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

		CHECK(c != NULL &&
		          strstr(c, "{\"package\":\"foo\",\"mode\":\"pinned\",\"version\":\"1.0\"}") != NULL,
		      "recipe content round-trips");
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	CHECK(cix_client_request(&client, "POST", "/v1/images/bulkimg/apply-recipe", NULL, &r) == 0 &&
	          r.status == 200,
	      "apply-recipe bulkimg is synchronous (200, no artifact configured)");
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
		jw_str(&w, "{\"packages\":[{\"package\":\"not-a-valid-entry\"}]}");
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		CHECK(cix_client_request(&client, "POST", "/v1/images/recipes", w.buf, &r) == 0 &&
		          r.status == 400,
		      "a packages entry with no mode or version is rejected (400)");
		jw_free(&w);
		cix_response_free(&r);
	}

	/*
	 * ADR-0320: the image's policy decides how its recipe, manifest and
	 * installs agree. bulkimg's recipe pins foo at 1.0 and its manifest
	 * was just declared from it.
	 */
	{
		char v[64];

		/* No saved policy is the defaults. */
		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "GET", "/v1/images/bulkimg/policy", NULL, &r) == 0 &&
		          r.status == 200 && r.json != NULL &&
		          strcmp(jstr(r.json, "recipe"), "manual") == 0 &&
		          strcmp(jstr(r.json, "apply"), "declare") == 0 &&
		          strcmp(jstr(r.json, "downgrade"), "refuse") == 0,
		      "ADR-0320: an image with no policy reports manual/declare/refuse");
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "PUT", "/v1/images/bulkimg/policy",
		                         "{\"apply\":\"sideways\"}", &r) == 0 &&
		          r.status == 400,
		      "ADR-0320: an unknown policy value is a 400");
		cix_response_free(&r);

		/* The image moves ahead of its recipe: foo is pinned at 3.0 by
		 * hand, so re-applying the recipe would walk it back to 1.0. */
		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "POST", "/v1/images/bulkimg/manifest",
		                         "{\"package\":\"foo\",\"mode\":\"pinned\",\"version\":\"3.0\"}",
		                         &r) == 0 &&
		          r.status == 204,
		      "ADR-0320: pin foo ahead of the recipe");
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "POST", "/v1/images/bulkimg/apply-recipe", NULL, &r) ==
		              0 &&
		          r.status == 409 && r.json != NULL,
		      "ADR-0320: an apply that would downgrade is refused (409)");
		if (r.json != NULL) {
			const struct json_value *d = json_object_get(r.json, "downgrades");
			const struct json_value *d0 =
			    d != NULL && d->type == JSON_ARRAY && d->u.array.count == 1 ? d->u.array.items[0]
			                                                               : NULL;

			CHECK(d0 != NULL && strcmp(jstr(d0, "package"), "foo") == 0 &&
			          strcmp(jstr(d0, "from"), "3.0") == 0 &&
			          strcmp(jstr(d0, "to"), "1.0") == 0,
			      "ADR-0320: the refusal names foo 3.0 -> 1.0, and only that");
		}
		cix_response_free(&r);
		manifest_version(&client, "bulkimg", "foo", v, sizeof(v));
		CHECK(strcmp(v, "3.0") == 0, "ADR-0320: a refused apply changes nothing");

		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "POST", "/v1/images/bulkimg/apply-recipe",
		                         "{\"allow_downgrade\":true}", &r) == 0 &&
		          r.status == 200 && r.json != NULL &&
		          json_as_number(json_object_get(r.json, "declared")) == 2,
		      "ADR-0320: allow_downgrade applies it, declaring both entries");
		cix_response_free(&r);
		manifest_version(&client, "bulkimg", "foo", v, sizeof(v));
		CHECK(strcmp(v, "1.0") == 0, "ADR-0320: the allowed downgrade moved foo to 1.0");

		/* Export is the live manifest in recipe form. */
		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "GET", "/v1/images/bulkimg/recipe/export", NULL, &r) ==
		              0 &&
		          r.status == 200 && r.json != NULL &&
		          strcmp(jstr(r.json, "image"), "bulkimg") == 0 &&
		          r.body != NULL && strstr(r.body, "\"package\":\"foo\"") != NULL &&
		          strstr(r.body, "\"version\":\"1.0\"") != NULL,
		      "ADR-0320: recipe/export renders the live manifest");
		cix_response_free(&r);

		/* Under recipe: follow, a changed recipe applies itself. */
		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "PUT", "/v1/images/bulkimg/policy",
		                         "{\"recipe\":\"follow\"}", &r) == 0 &&
		          r.status == 200 && r.json != NULL &&
		          strcmp(jstr(r.json, "recipe"), "follow") == 0 &&
		          strcmp(jstr(r.json, "apply"), "declare") == 0,
		      "ADR-0320: PUT sets recipe=follow and keeps the other settings");
		cix_response_free(&r);
		{
			struct json_writer w;

			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, "bulkimg");
			jw_key(&w, "content");
			jw_str(&w, "{\"packages\":[{\"package\":\"foo\",\"mode\":\"pinned\",\"version\":\"1.5\"},"
			            "{\"package\":\"bar\",\"mode\":\"rolling\",\"version\":\"2.0\"}]}");
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			memset(&r, 0, sizeof(r));
			CHECK(cix_client_request(&client, "POST", "/v1/images/recipes", w.buf, &r) == 0 &&
			          r.status == 204,
			      "ADR-0320: publish a changed recipe for a following image");
			cix_response_free(&r);
			jw_free(&w);
		}
		manifest_version(&client, "bulkimg", "foo", v, sizeof(v));
		CHECK(strcmp(v, "1.5") == 0, "ADR-0320: recipe=follow applied the new recipe by itself");

		/* Back to the defaults, which is the same as having no policy. */
		memset(&r, 0, sizeof(r));
		CHECK(cix_client_request(&client, "PUT", "/v1/images/bulkimg/policy",
		                         "{\"recipe\":\"manual\"}", &r) == 0 &&
		          r.status == 200,
		      "ADR-0320: back to the defaults");
		cix_response_free(&r);
	}

	/* --- scenario 5: recipe rm --- */
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
		                   "{\"name\":\"recon-unapplied\",\"content\":\"{\\\"packages\\\":"
		                   "[{\\\"package\\\":\\\"tcc\\\",\\\"mode\\\":\\\"rolling\\\","
		                   "\\\"version\\\":\\\"0.9.27\\\"}]}\"}",
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

	CHECK(stop_daemon(daemon_pid) == 0, "daemon shut down cleanly");
	test_data_dir_cleanup(g_data_dir);

	if (g_failures > 0) {
		fprintf(stderr, "test_image_recipe: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_image_recipe: OK\n");
	return 0;
}
