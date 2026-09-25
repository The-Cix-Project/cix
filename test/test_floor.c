#include "test_floor.h"
#include "json.h"
#include "test_image_fixture.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 600 polls 300 ms apart: three minutes, the budget every copy of this
 * loop had before it was shared. A cache-hit install takes seconds; the
 * margin is for a box busy with other builds. */
#define FLOOR_POLLS 600
#define FLOOR_POLL_US 300000

static int floor_install_one(const struct cix_client *c, const char *name)
{
	struct cix_response r;
	char buf[160];
	int i;
	time_t start = time(NULL);

	snprintf(buf, sizeof(buf), "{\"name\":\"%s\"}", name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "POST", "/v1/pkg/install", buf, &r) != 0 ||
	    (r.status != 202 && r.status != 200)) {
		fprintf(stderr, "FAIL: floor install of %s: status=%d %.200s\n", name, r.status,
		        r.body != NULL ? r.body : "");
		cix_response_free(&r);
		return -1;
	}
	cix_response_free(&r);

	snprintf(buf, sizeof(buf), "/v1/pkg/%s", name);
	for (i = 0; i < FLOOR_POLLS; i++) {
		const char *state = NULL;

		memset(&r, 0, sizeof(r));
		/* A request that fails counts as "not yet": extracting an artifact
		 * runs synchronously in the single-threaded daemon, so one status
		 * request can time out while the install is perfectly healthy. */
		if (cix_client_request(c, "GET", buf, NULL, &r) == 0 && r.json != NULL)
			state = json_as_string(json_object_get(r.json, "state"));
		if (state != NULL && strcmp(state, "installed") == 0) {
			/*
			 * What it delivered, not only what it says about
			 * itself. "installed" is a state string, and this floor
			 * exists to make a claim about CONTENT.
			 *
			 * Measured on 192.168.15.95, 2026-09-25
			 * (probe-cix-testreport@46): binutils@2.42-13 reported
			 * "installed in 0s" with no artifact unpack anywhere in
			 * the daemon's log, and every fixture build then failed
			 * the finalize policy with "the build image has no
			 * strip" -- the one tool binutils is in this floor to
			 * provide. A file count makes that a reading rather
			 * than an inference (cix#516).
			 */
			const struct json_value *files = json_object_get(r.json, "files");
			size_t nfiles = (files != NULL && files->type == JSON_ARRAY)
			                        ? files->u.array.count
			                        : 0;

			/* Timed, because a slow floor is otherwise invisible inside
			 * a test that later times out (probe-cix-testreport@7-1).
			 * stderr: unbuffered, so it survives the test being killed. */
			fprintf(stderr, "    floor: %s installed in %lds, %zu file(s)\n", name,
			        (long)(time(NULL) - start), nfiles);
			cix_response_free(&r);
			if (nfiles == 0) {
				fprintf(stderr,
				        "FAIL: floor package %s reports installed and lists no files "
				        "-- the artifact did not land\n",
				        name);
				return -1;
			}
			return 0;
		}
		if (state != NULL && strcmp(state, "failed") == 0) {
			const char *err = json_as_string(json_object_get(r.json, "error"));

			fprintf(stderr, "FAIL: floor package %s failed: %.240s\n", name,
			        err != NULL ? err : "(no error recorded)");
			cix_response_free(&r);
			return -1;
		}
		cix_response_free(&r);
		usleep(FLOOR_POLL_US);
	}
	fprintf(stderr, "FAIL: floor package %s did not reach installed in %d s\n", name,
	        FLOOR_POLLS * (FLOOR_POLL_US / 1000) / 1000);
	return -1;
}

/*
 * glibc, which nothing here installs, before anything that depends on
 * it -- and everything does.
 *
 * The daemon installs the C library into the default image itself when
 * it finds none (#186), asynchronously, as it comes up. Nothing waited
 * for that, so the floor raced it and lost: measured on 192.168.15.95,
 * 2026-09-25 (probe-cix-testreport@24, cix v2.57.273), the daemon began
 * unpacking glibc into __pkgbuild-0 and never finished before the test
 * gave up, while eleven floor installs ran past it, each one logging
 *
 *   pkg install: <name>: skipping the undeclared-link check --
 *   declared dependency "glibc" is not installed in image "base" (#389)
 *
 * and the kmod-build that followed failed with "declared build tool
 * \"glibc\" is not installed anywhere". Teardown then found glibc's
 * extraction still on disk, half-written, in dest.cbs-tmp-PzNrAx.
 *
 * It was intermittent in exactly the way a race is: the same floor
 * failed with CIXPKG-E4001 in one run and with no extraction error at
 * all in the next, because what differed was timing rather than state
 * -- which is what sent three probes into the artifact and one into
 * cbs before the daemon's own log was readable here (#485).
 *
 * An absent entry counts as "not yet": the daemon may not have created
 * it when the first poll lands.
 */
static int floor_wait_for_libc(const struct cix_client *c)
{
	struct cix_response r;
	time_t start = time(NULL);
	int i;

	for (i = 0; i < FLOOR_POLLS; i++) {
		const char *state = NULL;

		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, "GET", "/v1/pkg/glibc@base", NULL, &r) == 0 &&
		    r.json != NULL)
			state = json_as_string(json_object_get(r.json, "state"));
		if (state != NULL && strcmp(state, "installed") == 0) {
			cix_response_free(&r);
			fprintf(stderr, "    floor: glibc (the daemon's own) ready in %lds\n",
			        (long)(time(NULL) - start));
			return 0;
		}
		if (state != NULL && strcmp(state, "failed") == 0) {
			const char *err = json_as_string(json_object_get(r.json, "error"));

			fprintf(stderr, "FAIL: the daemon's own glibc install failed: %.240s\n",
			        err != NULL ? err : "(no error recorded)");
			cix_response_free(&r);
			return -1;
		}
		cix_response_free(&r);
		usleep(FLOOR_POLL_US);
	}
	fprintf(stderr,
	        "FAIL: the daemon's own glibc install did not reach installed in %d s -- every "
	        "floor package depends on it, so nothing after this would mean anything\n",
	        FLOOR_POLLS * (FLOOR_POLL_US / 1000) / 1000);
	return -1;
}

int test_floor_install_all(const struct cix_client *c)
{
	int i;

	if (floor_wait_for_libc(c) != 0)
		return -1;
	for (i = 0; test_floor_install[i] != NULL; i++)
		if (floor_install_one(c, test_floor_install[i]) != 0)
			return -1;
	return 0;
}
