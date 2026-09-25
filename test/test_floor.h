#ifndef CIX_TEST_FLOOR_H
#define CIX_TEST_FLOOR_H

#include "httpclient.h"

/*
 * Waits for the C library first, then installs every package in
 * test_floor_install[] (test_image_fixture.h) into the daemon behind c,
 * one at a time, waiting up to three minutes each for it to reach
 * "installed". The packages come from the floor
 * test_image_fixture_seed_floor_packages() put in that daemon's cache
 * before it started (ADR-0209), so each install is a cache hit.
 *
 * The C library is not one of them: the daemon installs glibc into the
 * default image itself when it finds none (#186), asynchronously, while
 * it comes up. Nothing waited for that and the floor raced it -- every
 * package here depends on glibc, so a floor installed alongside it
 * reports nothing trustworthy, and the failure lands much later and
 * elsewhere (#485).
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
