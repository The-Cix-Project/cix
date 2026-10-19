#ifndef TEST_DRIFT_H
#define TEST_DRIFT_H

#include "httpclient.h"

/*
 * ADR-0339 point 8 (#607): before a test stops its daemon, nothing it
 * changed went unannounced.
 *
 * Every state read the daemon announces is hashed at start and compared
 * again; a read whose answer changed while its generation did not is a
 * change the dashboard was never told about. Asking at the end of every
 * daemon-linked test makes each one that changes state also prove that
 * the change was announced, which no test written for its own subject
 * would otherwise check.
 *
 * Both halves are read, because they see different drift. POST
 * /v1/events/verify runs the full check now and lists what differs. The
 * rolling check that runs every second announces any drift it finds, so
 * a read it caught mid-test no longer differs by the end and appears
 * only in GET /v1/events/status's counter, and by name in the events
 * log. Either kind fails.
 *
 * `expected` is NULL, or a NULL-terminated list of operation ids this
 * test changes behind the daemon's back -- by writing into its data
 * directory, say -- and so legitimately drifts. Naming them is the
 * whole allowance: a read drifting that is not named is a missing
 * announcement or a misdeclared x-cix-read, to be fixed there.
 *
 * `client` must be one the daemon authorizes for system:write, which
 * every test daemon is unless the test turned host auth on.
 *
 * Returns 0, or -1 with one FAIL line (and the events log) on stderr.
 * Lives beside test_cleanup.c, not in test_image_fixture.c, which cixd
 * links and which must not depend on the HTTP client.
 */
int test_assert_no_drift(const struct cix_client *client, const char *const *expected);

#endif /* TEST_DRIFT_H */
