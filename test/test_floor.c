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
			 * How many files it delivered, alongside how long it
			 * took. Reported, not asserted: on 192.168.15.95,
			 * 2026-09-25 the count came back 38 for a .cixpkg
			 * install of binutils and 0 for a .tar.gz install of
			 * the same package, so a zero does not yet distinguish
			 * "installed nothing" from "this response does not
			 * carry a file list for that path" (cix#526). The
			 * number is what found the defect and is worth
			 * printing; gating on it needs that question answered
			 * first.
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
			/*
			 * A suspiciously small list gets NAMED, not just
			 * counted. cix#529 spent cycles on "binutils
			 * installed 7 file(s)" -- a number that is either a
			 * real truncated install or an artefact of which
			 * entry GET /v1/pkg/<name> resolved to (cix#520,
			 * cix#526, which is why the count above is reported
			 * and not asserted). Those two readings call for
			 * opposite fixes and the number cannot tell them
			 * apart; the file names can, instantly.
			 */
			if (nfiles > 0 && nfiles < 12) {
				size_t fi;

				for (fi = 0; fi < nfiles; fi++) {
					const char *fn = json_as_string(files->u.array.items[fi]);

					if (fn != NULL)
						fprintf(stderr, "      floor: %s file: %s\n", name, fn);
				}
			}
			cix_response_free(&r);
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
/*
 * The last `lines` lines of the newest retained build log whose name
 * begins with "<pkg>-" (GET /v1/pkg/build-logs, then /{file}).
 *
 * A failed build's own output is the only place its cause is written:
 * the package's `error` field says "build failed (exit status 3)" and
 * nothing else, and logstore_write() does not echo to stderr, so a test
 * that prints neither reports a number for a message. That cost a wrong
 * cause reported as fact -- cix#516, where every package built from a
 * converted fixture failed with exit 3 and the diagnosis came from a
 * downstream symptom instead.
 *
 * Filtered by package name rather than taking the newest entry outright,
 * because these tests build concurrently and the newest log is often
 * another package's. `lines` of 0 prints the whole log.
 */
void test_print_build_log(const struct cix_client *c, const char *pkg, int lines)
{
	struct cix_response list, file;
	const struct json_value *logs;
	char prefix[80], path[400];
	char found[192] = "";
	size_t i;

	memset(&list, 0, sizeof(list));
	if (cix_client_request(c, "GET", "/v1/pkg/build-logs", NULL, &list) != 0 ||
	    list.status != 200 || list.json == NULL) {
		fprintf(stderr, "    (build log unavailable: GET /v1/pkg/build-logs status=%d)\n",
		        list.status);
		cix_response_free(&list);
		return;
	}
	snprintf(prefix, sizeof(prefix), "%s-", pkg);
	logs = json_object_get(list.json, "logs");
	for (i = 0; logs != NULL && logs->type == JSON_ARRAY && i < logs->u.array.count; i++) {
		const char *f = json_as_string(json_object_get(logs->u.array.items[i], "file"));

		if (f != NULL && strncmp(f, prefix, strlen(prefix)) == 0) {
			/* Copied before the free: f points into the tree below it. */
			snprintf(found, sizeof(found), "%s", f);
			break;
		}
	}
	cix_response_free(&list);
	if (found[0] == '\0') {
		fprintf(stderr, "    (no retained build log for %s)\n", pkg);
		return;
	}

	snprintf(path, sizeof(path), "/v1/pkg/build-logs/%s", found);
	memset(&file, 0, sizeof(file));
	if (cix_client_request(c, "GET", path, NULL, &file) != 0 || file.status != 200 ||
	    file.body == NULL) {
		fprintf(stderr, "    (build log %s unavailable: status=%d)\n", found, file.status);
		cix_response_free(&file);
		return;
	}
	if (lines <= 0) {
		fprintf(stderr, "    --- build log %s (%d bytes) ---\n%.*s\n", found,
		        (int)file.body_len, (int)file.body_len, file.body);
	} else {
		const char *start = file.body + file.body_len;
		int seen = 0;

		while (start > file.body && seen <= lines) {
			start--;
			if (*start == '\n')
				seen++;
		}
		fprintf(stderr, "    --- build log %s (last %d lines) ---\n%s\n", found, lines, start);
	}
	cix_response_free(&file);
}
