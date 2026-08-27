#ifndef TEST_CLEANUP_H
#define TEST_CLEANUP_H

#include "httpclient.h"

/*
 * Deletes every container the daemon currently knows about, then the
 * named network. Call this instead of deleting a network directly at
 * the end of a test.
 *
 * A network with an attached container cannot be deleted, so a test
 * naming its containers by hand leaks a real host bridge the moment
 * that list drifts behind the containers the test actually creates --
 * or the moment an assertion fails before its own cleanup runs.
 *
 * The leak is not a tidiness problem. The NEXT run's network creation
 * fails with 500 because the bridge already exists, and every later
 * assertion cascades into 400s and 404s that read exactly like a code
 * regression. Both test_dns and test_container_restart hit this, and
 * each cost a full suite run to diagnose.
 *
 * Enumerating rather than listing names is the whole point: a
 * hand-kept list is the thing that drifts. Lives in its own file
 * rather than test_image_fixture.c because that file is linked into
 * cixd itself, which has no business depending on the HTTP client.
 *
 * Returns 0 if the network was removed, -1 otherwise (already
 * reported to stderr).
 */
int test_cleanup_containers_and_network(const struct cix_client *client, const char *network_name);

#endif /* TEST_CLEANUP_H */
