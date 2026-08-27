#include "test_cleanup.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

#define TEST_CLEANUP_MAX_CONTAINERS 128
#define TEST_CLEANUP_NAME_MAX 64

int test_cleanup_containers_and_network(const struct cix_client *client, const char *network_name)
{
	struct cix_response r;
	char names[TEST_CLEANUP_MAX_CONTAINERS][TEST_CLEANUP_NAME_MAX];
	int count = 0;
	int i;
	char path[128];
	int status;

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

	/*
	 * Status deliberately ignored: a container that is already gone is
	 * success as far as cleanup is concerned, and a test that has
	 * already failed should still leave the host clean.
	 */
	for (i = 0; i < count; i++) {
		snprintf(path, sizeof(path), "/v1/containers/%s", names[i]);
		memset(&r, 0, sizeof(r));
		cix_client_request(client, "DELETE", path, NULL, &r);
		cix_response_free(&r);
	}

	snprintf(path, sizeof(path), "/v1/networks/%s", network_name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(client, "DELETE", path, NULL, &r) != 0 || r.status != 204) {
		status = r.status;
		cix_response_free(&r);
		fprintf(stderr, "FAIL: DELETE /v1/networks/%s, status=%d\n", network_name, status);
		return -1;
	}
	cix_response_free(&r);
	return 0;
}
