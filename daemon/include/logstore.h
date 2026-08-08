#ifndef LOGSTORE_H
#define LOGSTORE_H

#include "json.h"

#include <stdint.h>

/*
 * A consolidated, API-accessible, size-bounded log (raised directly
 * by the user: kernel dmesg, kanxeod's own diagnostics, and a per-
 * request audit trail should all land in one place, retrievable over
 * the API like everything else in this project -- no SSH, no general
 * shell, ADR-0034, so a local log file an operator can't reach on a
 * real install isn't good enough).
 *
 * Every entry is one JSON-lines record: {"ts": <unix seconds>,
 * "source": "kernel"|"kanxeod"|"audit", "level": "...", "msg": "..."}.
 * "source" distinguishes the real Linux kernel ring buffer (read from
 * /dev/kmsg), kanxeod's own internal diagnostics (its existing
 * scattered stderr prints, now also captured here rather than
 * replaced -- stderr still matters during boot, before the API is
 * reachable), and a per-REST-request audit entry (one hook in
 * dispatch(), daemon/src/main.c's single top-level request router --
 * every kanxeoctl command and every web UI action already goes
 * through this exact function, API-First Mandate, so this is a
 * complete audit trail with zero client-side instrumentation needed).
 *
 * Storage is a small, fixed number of rotating segment files under a
 * dedicated directory, not a byte-exact single-file ring buffer --
 * the same practical tradeoff real bounded-log systems (journald,
 * logrotate) already make: an eviction is "delete the oldest whole
 * segment," O(1), not "rewrite the file minus its oldest bytes,"
 * O(n) per log line. The total byte cap an operator configures is
 * therefore enforced at segment granularity (LOGSTORE_SEGMENT_COUNT
 * segments, each capped at max_bytes / LOGSTORE_SEGMENT_COUNT), not
 * byte-exact -- a few percent of slop against the configured cap is
 * the accepted, ordinary cost of that tradeoff.
 */

#define LOGSTORE_SEGMENT_COUNT 8
#define LOGSTORE_DEFAULT_MAX_BYTES (100 * 1024 * 1024) /* 100 MB */
#define LOGSTORE_MIN_MAX_BYTES (1 * 1024 * 1024)       /* 1 MB */
#define LOGSTORE_MAX_MAX_BYTES ((int64_t)100 * 1024 * 1024 * 1024) /* 100 GB */

#define LOGSTORE_SOURCE_MAX 16
#define LOGSTORE_LEVEL_MAX 16
/*
 * Raised from an original 512 (2026-08-08, real deployment): a genuine
 * build failure's own captured output (pkg.c's PKG_BUILD_OUTPUT_CAPTURE_MAX)
 * routinely runs to several KB before the actual error line, and 512
 * silently discarded all but the first ~500 bytes of it -- hiding the
 * one piece of information this whole capture mechanism exists to
 * preserve. 4096 gives real diagnostic text room to breathe while
 * staying a fixed, bounded stack buffer (logstore_write()'s own
 * vsnprintf() truncates safely regardless of the exact value chosen).
 */
#define LOGSTORE_MSG_MAX 4096

enum logstore_error {
	LOGSTORE_OK = 0,
	LOGSTORE_ERR_INVALID_MAX_BYTES,
	LOGSTORE_ERR_INVALID_MIN_LEVEL,
	LOGSTORE_ERR_PERSIST_FAILED
};

/*
 * "settable so we do not over-log" (raised directly by the user,
 * 2026-08-08, alongside the LOGSTORE_MSG_MAX fix above): a persisted
 * minimum severity threshold, checked at write time -- an entry less
 * severe than the configured floor is dropped before ever touching a
 * segment file, not merely hidden from GET's own existing
 * level_filter (which only ever filters what's already stored).
 * Covers the full real syslog severity range kernel dmesg entries
 * already carry (emerg..debug, kmsg_level_name()'s own names) since
 * that's genuinely the noisiest source today; kanxeod/audit's own
 * "info"/"error" entries map onto the same scale. Default "debug"
 * (log everything) preserves this store's exact pre-existing
 * behavior for anyone who never touches the setting.
 */
#define LOGSTORE_DEFAULT_MIN_LEVEL "debug"

/* Loads persisted config (the max_bytes cap, if previously set) and
 * scans dir for any segment files already there (a daemon restart
 * picks up exactly where the log left off, never starts a fresh
 * store on every boot). dir is created if it doesn't exist. */
int logstore_init(const char *dir, const char *state_path);

/* Starts the /dev/kmsg reader -- registered into the caller's own
 * epoll loop (main.c's g_epfd) the same way every other fd this
 * daemon watches already is; returns the fd to add (POLLIN), or -1 if
 * /dev/kmsg couldn't be opened (non-fatal -- logged as a kanxeod-
 * source entry, kernel-source entries just won't be captured). Call
 * logstore_kmsg_readable() when that fd becomes readable.
 */
int logstore_kmsg_fd(void);
void logstore_kmsg_readable(void);

/* Appends one entry. printf-style; msg is truncated to
 * LOGSTORE_MSG_MAX - 1 bytes, never a buffer overrun. source/level
 * are truncated to their own _MAX -1 similarly. Safe to call from
 * anywhere in the daemon, including before logstore_init() (a no-op
 * until initialized, so early startup prints before ensure_dir()
 * has run don't crash). */
void logstore_write(const char *source, const char *level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

enum logstore_error logstore_set_max_bytes(int64_t max_bytes);
int64_t logstore_max_bytes(void);

/*
 * min_level must be one of the real syslog severity names
 * (kmsg_level_name()'s own set: "emerg", "alert", "crit", "err" or
 * "error" (both accepted -- kanxeod/audit's own convention is
 * "error", the kernel's is "err"), "warning" or "warn" (both
 * accepted), "notice", "info", "debug") -- anything else is
 * LOGSTORE_ERR_INVALID_MIN_LEVEL. Applied at logstore_write() time,
 * not just at GET time: an entry less severe than this floor is
 * dropped before ever touching a segment file.
 */
enum logstore_error logstore_set_min_level(const char *min_level);
const char *logstore_min_level(void);

/* Writes up to limit most-recent entries (newest last, matching
 * `tail`'s own convention) matching every given filter (NULL/0 means
 * "no filter" for that field) into w as a JSON array. limit <= 0
 * means LOGSTORE_DEFAULT limit (1000) -- never unbounded, this store
 * has no index and a very large limit means reading whole segments. */
void logstore_tail(const char *source_filter, const char *level_filter, int64_t since,
                    int limit, struct json_writer *w);

#endif /* LOGSTORE_H */
