/*
 * test_apishape -- a 200 response carries the keys its contract
 * declares required (cix#575).
 *
 * ADR-0218 made the spec authoritative over which ROUTES exist: a path
 * the contract does not declare is unroutable, and `test_apiroute`
 * gates that. Nothing did the same for a response BODY. A handler could
 * stop sending a field the spec marks `required`, or rename it, and
 * every gate stayed green -- `test_apigen` counts operations and
 * permissions, `test_apiroute` checks routing, `test_api_surfaces`
 * checks that no presentation channel invents a path, and none of them
 * compares a response's keys against its schema.
 *
 * That is cix#574, exactly. `GET /v1/pkg/source-catalogue` moved from a
 * `state` string plus four counts to an ADR-0256 `stage`/`status` pair
 * plus four differently-named counts; the spec kept declaring the old
 * shape as required and `cixctl` kept reading it, so an operator got a
 * blank verdict on all 172 rows and a summary of four zeros -- on the
 * one surface ADR-0323 names for roll visibility. Two tests already
 * called that endpoint and both passed, because each asserted the
 * fields it happened to use rather than the ones the contract promises.
 *
 * WHAT THIS GATES, and what it does not.
 *
 * `build/generated/api_shapes.h` carries one entry per operation with
 * the required keys of its 200 JSON response -- top level, plus the
 * item keys of every array property that declares any, which is where
 * the half of cix#574 an operator actually saw lived (`packages[]`
 * holding `SourceCatalogueEntry`). This test walks the entries that
 * need no path parameter and are safe to call -- GET only -- and
 * asserts every declared key is present.
 *
 * A non-200 answer is NOT a failure here. An endpoint legitimately
 * 404s when the thing it describes does not exist -- measured on
 * 192.168.15.95, 2026-10-07: of the 82 gateable GETs, 81 answered 200
 * and `GET /v1/pki/intermediate` answered 404, that host having no
 * intermediate CA. This gate has nothing to say about a response that
 * is not the one the schema describes, so it asserts the implication
 * only: IF 200, THEN the promised keys.
 *
 * `apigen` reads a narrow schema subset on purpose -- a bare array, an
 * `allOf`, a schema with no `required` at all contribute no assertion
 * -- so a spec drifting towards shapes the generator cannot see would
 * weaken this test. What stops that being silent is NOT the count it
 * prints: `make selftest` captures both streams and prints them only
 * on FAIL, so a passing run's output is discarded. It is the named
 * assertion below, which fails if #574's own endpoint ever leaves the
 * gate.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include "generated/api_shapes.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7690
#define PORT_ARG "--port=7690"

static char g_data_dir[PATH_MAX];

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
 * Which keys the contract promised and the body did not carry, as one
 * comma-separated list -- all of them, not the first.
 *
 * Reporting every missing key matters here: a renamed field usually
 * takes its neighbours with it (cix#574 moved `state` to `stage` AND
 * `status`, and all four counts at once), and a gate that names one of
 * six sends the reader back for another run to learn the rest.
 */
static int missing_keys(const struct json_value *obj, const char *const *required, char *out,
                        size_t out_size)
{
	int n = 0;
	int i;

	out[0] = '\0';
	if (obj == NULL || obj->type != JSON_OBJECT)
		return -1;
	for (i = 0; required[i] != NULL; i++) {
		if (json_object_get(obj, required[i]) != NULL)
			continue;
		if (out[0] != '\0')
			snprintf(out + strlen(out), out_size - strlen(out), ", ");
		snprintf(out + strlen(out), out_size - strlen(out), "%s", required[i]);
		n++;
	}
	return n;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	int ok = 1;
	size_t i;
	int checked = 0, items_checked = 0, skipped_status = 0;
	/* #574's own endpoint, asserted by name below. */
	int catalogue_gated = 0;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	for (i = 0; i < sizeof(cix_api_shapes) / sizeof(cix_api_shapes[0]); i++) {
		const struct cix_api_shape *s = &cix_api_shapes[i];
		struct cix_response r;
		char missing[512];
		int n;

		/*
		 * GET only, and only when the path names no parameter. A
		 * write would change the state the next operation reads, and
		 * a parameterised path would need a real resource this test
		 * has no business creating -- #575's own costing says the
		 * parameterless GETs are the subset that scales.
		 */
		if (s->required == NULL || s->n_params != 0 || strcmp(s->method, "GET") != 0)
			continue;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", s->path, NULL, &r) != 0) {
			fprintf(stderr, "FAIL: %s: GET %s did not complete\n", s->op_id, s->path);
			ok = 0;
			cix_response_free(&r);
			continue;
		}
		if (r.status != 200) {
			skipped_status++;
			cix_response_free(&r);
			continue;
		}
		n = missing_keys(r.json, s->required, missing, sizeof(missing));
		if (n < 0) {
			fprintf(stderr,
			        "FAIL: %s: GET %s answered 200 but the body is not a JSON object, "
			        "and its schema declares required keys\n",
			        s->op_id, s->path);
			ok = 0;
		} else if (n > 0) {
			fprintf(stderr,
			        "FAIL: %s: GET %s answered 200 without %d key(s) its contract "
			        "declares required: %s\n",
			        s->op_id, s->path, n, missing);
			ok = 0;
		} else {
			checked++;
			if (strcmp(s->op_id, "getSourceCatalogue") == 0)
				catalogue_gated = 1;
		}

		/*
		 * One level into EVERY array property that declares item
		 * keys, and only where the array has a first element: an
		 * empty list is a legitimate answer and says nothing about
		 * the shape of an entry.
		 */
		if (s->arrays != NULL && r.json != NULL && r.json->type == JSON_OBJECT) {
			int a;

			for (a = 0; s->arrays[a].prop != NULL; a++) {
				const struct json_value *arr =
				    json_object_get(r.json, s->arrays[a].prop);

				if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0)
					continue;
				n = missing_keys(arr->u.array.items[0], s->arrays[a].required, missing,
				                 sizeof(missing));
				if (n < 0) {
					fprintf(stderr,
					        "FAIL: %s: GET %s: %s[0] is not an object, and its schema "
					        "declares required keys\n",
					        s->op_id, s->path, s->arrays[a].prop);
					ok = 0;
				} else if (n > 0) {
					fprintf(stderr,
					        "FAIL: %s: GET %s: %s[0] is missing %d key(s) its contract "
					        "declares required: %s\n",
					        s->op_id, s->path, s->arrays[a].prop, n, missing);
					ok = 0;
				} else {
					items_checked++;
				}
			}
		}
		cix_response_free(&r);
	}

	/*
	 * The endpoint this test exists for, asserted BY NAME.
	 *
	 * A count is the wrong guarantee here, and `make selftest` is why:
	 * it captures both streams of every test and prints them only on
	 * FAIL, then deletes the log -- so a passing test's output is
	 * discarded, and the coverage line below is invisible in exactly
	 * the case where the gate is healthy. (The same shape as
	 * logstore_write() not echoing to stderr: adding a line is not the
	 * same as making it visible.) A floor nobody can read the real
	 * number against is a safety net, not a measure.
	 *
	 * So the thing that must not silently stop being true is named:
	 * cix#574 was `GET /pkg/source-catalogue` answering with a shape
	 * its contract did not describe, and if that endpoint ever leaves
	 * this gate -- renamed, parameterised, its `required` list dropped,
	 * or answering a status this test skips -- the failure says so
	 * instead of a count quietly going down by one.
	 */
	if (!catalogue_gated) {
		fprintf(stderr,
		        "FAIL: GET /v1/pkg/source-catalogue was not gated against its contract, "
		        "and it is the endpoint this test exists for (#574). Either its schema "
		        "stopped declaring required keys, or it did not answer 200 here, or the "
		        "operationId changed -- all three need a look rather than a silently "
		        "smaller count\n");
		ok = 0;
	}

	/*
	 * And a floor, as a net under the rest.
	 *
	 * An exact count would be a fifth place to edit on every spec
	 * change; test_apigen already holds the operation count in four,
	 * and two releases failed on a missed one. 20 rather than something
	 * near the real figure, because the real figure for a FRESH daemon
	 * has never been measured -- 82 operations are gateable and 81
	 * answered 200 on a live host (192.168.15.95, 2026-10-07), but this
	 * daemon has no sources, no images but `base` and no containers.
	 * Guessing near 81 would make the gate fail for being new rather
	 * than for being weak, and the number cannot be read off a passing
	 * run to do better. Run `./build/test_apishape` directly to see it.
	 */
	if (checked < 20) {
		fprintf(stderr,
		        "FAIL: only %d operation(s) were gated against their contract; this test "
		        "stops being a gate that low, so either the spec lost required lists or "
		        "apigen stopped reading them\n",
		        checked);
		ok = 0;
	}

	/* Discarded by `make selftest` on a pass -- see the comment above.
	 * Here for a direct run, and for the failure output. */
	printf("%s: %d operation(s) gated against their declared response shape, %d of them one "
	       "level into an array; %d answered a status other than 200 and were not gated\n",
	       ok ? "ok" : "FAIL", checked, items_checked, skipped_status);

	kill(daemon_pid, SIGTERM);
	waitpid(daemon_pid, NULL, 0);
	test_data_dir_cleanup(g_data_dir);

	if (!ok) {
		printf("TEST FAILED\n");
		return 1;
	}
	printf("test_apishape: PASS\n");
	return 0;
}
