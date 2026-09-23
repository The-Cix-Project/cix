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
			cix_response_free(&r);
			/* Timed, because a slow floor is otherwise invisible inside
			 * a test that later times out (probe-cix-testreport@7-1).
			 * stderr: unbuffered, so it survives the test being killed. */
			fprintf(stderr, "    floor: %s installed in %lds\n", name,
			        (long)(time(NULL) - start));
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

int test_floor_install_all(const struct cix_client *c)
{
	int i;

	for (i = 0; test_floor_install[i] != NULL; i++)
		if (floor_install_one(c, test_floor_install[i]) != 0)
			return -1;
	return 0;
}
