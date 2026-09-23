#ifndef CIX_TEST_FLOOR_H
#define CIX_TEST_FLOOR_H

#include "httpclient.h"

/*
 * Installs every package in test_floor_install[] (test_image_fixture.h)
 * into the daemon behind c, one at a time, and waits up to three minutes
 * each for it to reach "installed". The packages come from the floor
 * test_image_fixture_seed_floor_packages() put in that daemon's cache
 * before it started (ADR-0209), so each install is a cache hit.
 *
 * Every failure is printed where it happens -- the refused request, or
 * the package that ended "failed" with the daemon's own error, or the
 * one that never finished -- because a floor package that did not
 * install surfaces much later as a build reporting a missing tool,
 * which names the wrong thing.
 *
 * Returns 0 when every package installed, -1 otherwise. Stops at the
 * first failure: nothing after it can be trusted to compose.
 */
int test_floor_install_all(const struct cix_client *c);

#endif
