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

/*
 * One file each floor package is in the floor FOR, asserted against
 * the install's own file list.
 *
 * A count proves an artifact landed; it does not prove the package is
 * usable, and the two came apart on 192.168.15.95, 2026-09-25
 * (probe-cix-testreport@47): binutils reported "installed in 1s, 38
 * file(s)" and every fixture build still failed the finalize policy
 * with "the build image has no strip". Naming the file turns the next
 * run into a reading of whether strip is in the package or missing
 * from the composed environment -- two different bugs that the count
 * alone cannot tell apart (cix#516).
 *
 * Only the packages with an obvious single reason to be here. A
 * package with no entry is checked for a non-empty list and no more.
 */
static const struct {
	const char *name;
	const char *path;
} floor_required_file[] = {
	{ "bash", "usr/bin/bash" },       { "coreutils", "usr/bin/cp" },
	{ "tcc", "usr/bin/tcc" },         { "binutils", "usr/bin/strip" },
	{ "m4", "usr/bin/m4" },           { "xz", "usr/bin/xz" },
	{ "cbs", "usr/bin/cbs" },
};

/* Whether files[] carries path. Returns 1 when no entry names this
 * package, so an unlisted package is never failed by this check. */
static int floor_files_have_required(const char *name, const struct json_value *files)
{
	size_t i, j;

	for (i = 0; i < sizeof(floor_required_file) / sizeof(floor_required_file[0]); i++) {
		if (strcmp(floor_required_file[i].name, name) != 0)
			continue;
		if (files == NULL || files->type != JSON_ARRAY)
			return 0;
		for (j = 0; j < files->u.array.count; j++) {
			const char *s = json_as_string(files->u.array.items[j]);

			if (s != NULL && strcmp(s, floor_required_file[i].path) == 0)
				return 1;
		}
		fprintf(stderr, "FAIL: floor package %s installed without %s, which is what it is "
		                "in this floor to provide\n",
		        name, floor_required_file[i].path);
		return 0;
	}
	return 1;
}

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
			int ok;
			const struct json_value *files = json_object_get(r.json, "files");
			size_t nfiles = (files != NULL && files->type == JSON_ARRAY)
			                        ? files->u.array.count
			                        : 0;

			/* Timed, because a slow floor is otherwise invisible inside
			 * a test that later times out (probe-cix-testreport@7-1).
			 * stderr: unbuffered, so it survives the test being killed. */
			fprintf(stderr, "    floor: %s installed in %lds, %zu file(s)\n", name,
			        (long)(time(NULL) - start), nfiles);
			/* Both reads happen while r still owns the tree: files
			 * points into it, so the free belongs after the check
			 * and not before it. */
			ok = (nfiles > 0 && floor_files_have_required(name, files));
			cix_response_free(&r);
			if (!ok) {
				fprintf(stderr,
				        "FAIL: floor package %s did not deliver what this floor needs "
				        "(%zu file(s) installed)\n",
				        name, nfiles);
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
