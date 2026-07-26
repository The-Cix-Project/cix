#ifndef TEST_NET_CLEANUP_H
#define TEST_NET_CLEANUP_H

/*
 * Deletes the named link, retrying briefly. Every test that starts a
 * real kanxeod unconditionally gets the "kanxeo0" default bridge
 * created at startup (ensure_default_network(), the same way
 * ensure_dir() unconditionally creates the storage directories) --
 * this is shared, not duplicated per test file, so every one of them
 * cleans it up the same way regardless of whether that specific test
 * exercised networking directly.
 *
 * Retried, not a single attempt: network namespace/veth teardown runs
 * on a kernel workqueue (docs/ROADMAP.md Phase 6 part 2), so a
 * just-removed networked container's veth may still be settling,
 * transiently blocking the bridge delete. Prints a warning (not a
 * hard test failure) if every attempt fails.
 */
void test_cleanup_bridge(const char *name);

#endif /* TEST_NET_CLEANUP_H */
