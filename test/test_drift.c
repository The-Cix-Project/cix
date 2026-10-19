#include "test_drift.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

/* The daemon's own words for a drift (events_check_slot()), which the
 * events log line begins with after the read's operation id. */
#define DRIFT_LOG_MARK " changed without an announcement"

static int is_expected(const char *const *expected, const char *op, size_t op_len)
{
	size_t i;

	for (i = 0; expected != NULL && expected[i] != NULL; i++) {
		if (strlen(expected[i]) == op_len && memcmp(expected[i], op, op_len) == 0)
			return 1;
	}
	return 0;
}

/* The events log, printed with the failure: the daemon names each
 * drifted read there, and a test daemon's store goes with its data
 * directory when the test exits. */
static void print_events_log(const struct cix_client *client)
{
	struct cix_response lr;

	memset(&lr, 0, sizeof(lr));
	if (cix_client_request(client, "GET", "/v1/system/logs?source=events&tail=20", NULL, &lr) ==
	        0 &&
	    lr.body != NULL)
		fprintf(stderr, "      events log: %s\n", lr.body);
	cix_response_free(&lr);
}

/*
 * Every op the events log names as drifted, each checked against
 * expected. Returns how many were not expected, with the first in
 * first_bad.
 */
static int unexpected_logged_drift(const struct cix_client *client, const char *const *expected,
                                   char *first_bad, size_t first_bad_size)
{
	struct cix_response lr;
	size_t i;
	int bad = 0;

	memset(&lr, 0, sizeof(lr));
	if (cix_client_request(client, "GET", "/v1/system/logs?source=events&tail=200", NULL, &lr) !=
	        0 ||
	    lr.json == NULL || lr.json->type != JSON_ARRAY) {
		snprintf(first_bad, first_bad_size, "(the events log could not be read)");
		cix_response_free(&lr);
		return 1;
	}
	for (i = 0; i < lr.json->u.array.count; i++) {
		const char *msg = json_as_string(json_object_get(lr.json->u.array.items[i], "msg"));
		const char *mark = msg != NULL ? strstr(msg, DRIFT_LOG_MARK) : NULL;

		if (mark == NULL || is_expected(expected, msg, (size_t)(mark - msg)))
			continue;
		if (bad++ == 0)
			snprintf(first_bad, first_bad_size, "%.*s", (int)(mark - msg), msg);
	}
	cix_response_free(&lr);
	return bad;
}

int test_assert_no_drift(const struct cix_client *client, const char *const *expected)
{
	struct cix_response r;
	const struct json_value *drift, *checked;
	char bad[160] = "";
	int failed = 0;
	size_t i;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(client, "POST", "/v1/events/verify", NULL, &r) != 0 ||
	    r.status != 200 || r.json == NULL) {
		fprintf(stderr, "FAIL: drift check (ADR-0339): POST /v1/events/verify, status=%d\n",
		        r.status);
		cix_response_free(&r);
		return -1;
	}
	drift = json_object_get(r.json, "drift");
	checked = json_object_get(r.json, "checked");
	if (drift == NULL || drift->type != JSON_ARRAY || checked == NULL ||
	    json_as_number(checked) <= 0) {
		fprintf(stderr, "FAIL: drift check (ADR-0339): verify compared no state reads\n");
		failed = 1;
	} else {
		for (i = 0; i < drift->u.array.count; i++) {
			const char *op = json_as_string(drift->u.array.items[i]);

			if (op != NULL && !is_expected(expected, op, strlen(op))) {
				fprintf(stderr,
				        "FAIL: drift check (ADR-0339): %s changed without an announcement "
				        "(%d state reads compared)\n",
				        op, (int)json_as_number(checked));
				failed = 1;
			}
		}
	}
	cix_response_free(&r);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(client, "GET", "/v1/events/status", NULL, &r) != 0 || r.json == NULL ||
	    json_object_get(r.json, "drift") == NULL) {
		fprintf(stderr, "FAIL: drift check (ADR-0339): GET /v1/events/status, status=%d\n",
		        r.status);
		failed = 1;
	} else if (json_as_number(json_object_get(r.json, "drift")) != 0 &&
	           unexpected_logged_drift(client, expected, bad, sizeof(bad)) != 0) {
		fprintf(stderr,
		        "FAIL: drift check (ADR-0339): the rolling check found %s changed without an "
		        "announcement during the run\n",
		        bad);
		failed = 1;
	}
	cix_response_free(&r);

	if (failed)
		print_events_log(client);
	return failed ? -1 : 0;
}
