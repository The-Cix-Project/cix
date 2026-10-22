#ifndef ROOTFSDISCARD_H
#define ROOTFSDISCARD_H

/*
 * ADR-0341's one delete queue, made visible outside main.c.
 *
 * Sets `path` aside in `trash_parent`/.discard with one rename and
 * queues its delete in a helper child, so a whole tree leaves without
 * an rmtree on the event loop. `trash_parent` must be on the same
 * filesystem as `path`. Returns 0 when nothing is left at `path`: it
 * was absent, or it is now in the trash.
 *
 * The definition stays in main.c beside the queue and helper_run(),
 * the way daemonpaths.h makes main.c's paths visible. pkg.c is the
 * second caller (#626): a build slot's previous tree goes the same way
 * a deleted container's base does. Never add a second delete queue.
 */
int rootfs_discard(const char *name, const char *path, const char *trash_parent);

#endif /* ROOTFSDISCARD_H */
