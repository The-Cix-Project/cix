#include "test_cleanup.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "json.h"

#define TEST_CLEANUP_MAX_CONTAINERS 128
#define TEST_CLEANUP_NAME_MAX 64

/*
 * How many enumerate-and-delete passes before giving up, and how long
 * to pause between them. Ten passes at 200ms is two seconds, which is
 * far longer than any autostart sequence these tests set up and short
 * enough that a genuine leak still fails promptly.
 */
#define TEST_CLEANUP_PASSES 10
#define TEST_CLEANUP_PAUSE_NANOS (200L * 1000L * 1000L)

/* Enumerates containers and deletes every one of them. Returns how many
 * it found, so the caller can tell "nothing left" from "deleted some".
 *
 * Delete status is deliberately ignored: a container that is already
 * gone is success as far as cleanup is concerned, and a test that has
 * already failed should still leave the host clean. */
static int delete_every_container(const struct cix_client *client)
{
	struct cix_response r;
	char names[TEST_CLEANUP_MAX_CONTAINERS][TEST_CLEANUP_NAME_MAX];
	int count = 0;
	int i;
	char path[128];

	memset(&r, 0, sizeof(r));
	if (cix_client_request(client, "GET", "/v1/containers", NULL, &r) == 0 && r.json != NULL) {
		const struct json_value *arr = json_object_get(r.json, "containers");

		if (arr != NULL && arr->type == JSON_ARRAY) {
			size_t j;

			for (j = 0; j < arr->u.array.count && count < TEST_CLEANUP_MAX_CONTAINERS; j++) {
				const char *nm =
				    json_as_string(json_object_get(arr->u.array.items[j], "name"));

				if (nm != NULL && nm[0] != '\0')
					snprintf(names[count++], TEST_CLEANUP_NAME_MAX, "%s", nm);
			}
		}
	}
	cix_response_free(&r);

	for (i = 0; i < count; i++) {
		snprintf(path, sizeof(path), "/v1/containers/%s", names[i]);
		memset(&r, 0, sizeof(r));
		cix_client_request(client, "DELETE", path, NULL, &r);
		cix_response_free(&r);
	}
	return count;
}

/*
 * Cleanup RACES the daemon's own autostart, so it retries.
 *
 * One enumerate-then-delete pass is not enough, and this cost four
 * consecutive fifteen-minute build cycles on 192.168.15.95 before it
 * was read correctly. The tests that call this create containers with
 * restart:always and then restart the daemon; the daemon brings those
 * back on its own schedule, pausing on real readiness checks along the
 * way. A container that autostarts AFTER the enumeration is not in the
 * list this deletes, so it is still attached to the network when the
 * network delete goes out, and that delete is refused 409 -- correctly.
 * The daemon is not wrong; a single-pass cleanup is.
 *
 * The build log said so plainly once looked at: the four
 * "depA: autostarted (restart:always)" lines are printed AFTER the
 * failing delete, not before it.
 *
 * So: delete, try the network, and on a refusal go round again. Each
 * pass re-enumerates, which is what picks up whatever appeared since
 * the last one.
 */
int test_cleanup_containers_and_network(const struct cix_client *client, const char *network_name)
{
	struct cix_response r;
	char path[128];
	int status = 0;
	int pass;

	snprintf(path, sizeof(path), "/v1/networks/%s", network_name);

	for (pass = 0; pass < TEST_CLEANUP_PASSES; pass++) {
		struct timespec pause = { 0, TEST_CLEANUP_PAUSE_NANOS };

		delete_every_container(client);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(client, "DELETE", path, NULL, &r) == 0 && r.status == 204) {
			cix_response_free(&r);
			return 0;
		}
		status = r.status;
		cix_response_free(&r);
		nanosleep(&pause, NULL);
	}

	/*
	 * Name what is still holding it. A bare 409 says only "something",
	 * and the daemon's own answer -- which container, in what state,
	 * on which networks -- is one GET away. Without this the next
	 * occurrence costs another ten-minute build cycle to learn nothing.
	 */
	fprintf(stderr, "FAIL: DELETE /v1/networks/%s, status=%d after %d passes\n", network_name,
	        status, TEST_CLEANUP_PASSES);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(client, "GET", "/v1/containers", NULL, &r) == 0 && r.json != NULL) {
		const struct json_value *arr = json_object_get(r.json, "containers");
		size_t j;

		if (arr != NULL && arr->type == JSON_ARRAY) {
			if (arr->u.array.count == 0)
				fprintf(stderr, "  no containers remain -- the network is held by something else\n");
			for (j = 0; j < arr->u.array.count; j++) {
				const struct json_value *c = arr->u.array.items[j];
				const struct json_value *nets = json_object_get(c, "networks");
				const char *nm = json_as_string(json_object_get(c, "name"));
				const char *st = json_as_string(json_object_get(c, "status"));
				size_t k;

				fprintf(stderr, "  still here: %s status=%s networks=", nm != NULL ? nm : "?",
				        st != NULL ? st : "?");
				if (nets != NULL && nets->type == JSON_ARRAY)
					for (k = 0; k < nets->u.array.count; k++)
						fprintf(stderr, "%s%s", k > 0 ? "," : "",
						        json_as_string(json_object_get(nets->u.array.items[k], "name")));
				fprintf(stderr, "\n");
			}
		}
	}
	cix_response_free(&r);
	return -1;
}
