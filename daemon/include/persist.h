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

#endif /* PERSIST_H */
