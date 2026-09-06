#ifndef PERSIST_H
#define PERSIST_H

#include <stddef.h>

/*
 * Shared durable-state helpers (ADR-0012): every daemon-owned resource
 * that must survive a restart (networks so far; DNS records too) uses
 * this same atomic-rewrite-plus-reload pattern rather than each
 * reimplementing it -- exactly the kind of duplication "No Parallel
 * Implementations" forbids.
 */

/*
 * Atomically rewrites path with data (len bytes): write to
 * path+".tmp", fsync, then rename() over path (atomic on the same
 * filesystem). A crash mid-write must never leave a caller's durable
 * state file corrupted or partially written.
 */
int persist_atomic_write(const char *path, const char *data, size_t len);

/*
 * Same, but keeps the inode rather than renaming a new file over the
 * target -- required for any file inside a RUNNING container, where a
 * rename into an overlay upper layer is invisible through the merged
 * mount the container reads (#276). Not all-or-nothing; only use it
 * for derived files that regenerate, never for authoritative state.
 */
int persist_write_file_inplace(const char *path, const char *data, size_t len);

/*
 * Reads path fully into a malloc'd, NUL-terminated buffer (*out_len
 * excludes the added NUL; caller frees *out_buf). If path doesn't
 * exist, returns 0 with *out_buf = NULL, *out_len = 0 -- the "no
 * persisted state yet" case every caller needs to handle identically
 * (first-ever startup, nothing to load). Returns -1 on any other
 * error (caller should treat this as a hard failure, not silently
 * proceed with an empty table -- see docs/roadmap/ROADMAP.md Phase 7 part 1
 * on why silently forgetting persisted state is exactly the bug this
 * module exists to prevent).
 */
int persist_read_file(const char *path, char **out_buf, size_t *out_len);

/*
 * mkdir -p equivalent: creates dir_path and every missing parent
 * component (EEXIST tolerated throughout). Needed before writing into
 * an arbitrary caller-supplied path whose parent directories aren't
 * guaranteed to exist yet -- e.g. a minimal container image with no
 * /etc at all.
 */
int persist_mkdir_p(const char *dir_path);

/*
 * rm -rf equivalent: recursively removes path and everything under it
 * (a physical, post-order nftw() walk -- never follows a symlink, only
 * ever removes the link itself; every directory's own children are
 * removed before the directory itself). Extracted from image.c's own
 * previously-private remove_tree() (image deletion) so container
 * deletion (task #738 -- DELETE /v1/containers never removed its own
 * upper/work/merged directories, a real, pre-existing disk-space leak)
 * reuses the exact same primitive instead of a second, parallel copy of
 * the identical nftw() callback -- exactly the duplication "No Parallel
 * Implementations" forbids. Returns 0 on success, -1 (errno set by
 * whichever unlink()/rmdir() call failed) otherwise; ENOENT on path
 * itself is tolerated (nothing to remove is not a failure -- matches
 * persist_read_file()'s own "no persisted state yet" tolerance).
 */
int persist_remove_tree(const char *path);

/*
 * ADR-0253: a build output tree is created fresh, never inherited.
 *
 * Removes whatever is at path and recreates it empty, so what a build
 * produces is exactly what that build put there. This is one call
 * rather than a remove-then-create at each site because the sites that
 * got it wrong got it wrong by omission, and an omission is invisible.
 *
 * Returns 0 on success, -1 with errno set otherwise.
 */
int persist_fresh_output_dir(const char *path);

#endif /* PERSIST_H */
