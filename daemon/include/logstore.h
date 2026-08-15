#ifndef LOGSTORE_H
#define LOGSTORE_H

#include "json.h"

#include <stdint.h>

/*
 * A consolidated, API-accessible, size-bounded log (raised directly
 * by the user: kernel dmesg, thincd's own diagnostics, and a per-
 * request audit trail should all land in one place, retrievable over
 * the API like everything else in this project -- no SSH, no general
 * shell, ADR-0034, so a local log file an operator can't reach on a
 * real install isn't good enough).
 *
 * Every entry is one JSON-lines record: {"ts": <unix seconds>,
 * "source": "kernel"|"thincd"|"audit", "level": "...", "msg": "..."}.
 * "source" distinguishes the real Linux kernel ring buffer (read from
 * /dev/kmsg), thincd's own internal diagnostics (its existing
 * scattered stderr prints, now also captured here rather than
 * replaced -- stderr still matters during boot, before the API is
 * reachable), and a per-REST-request audit entry (one hook in
 * dispatch(), daemon/src/main.c's single top-level request router --
 * every thincctl command and every web UI action already goes
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
/* Matches REGISTRY_NAME_MAX by value, no header dependency -- same
 * precedent NTP_SERVER_NAME_MAX (daemon/include/ntp.h) already
 * established for this exact situation. */
#define LOGSTORE_CONTAINER_MAX 64
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
 * that's genuinely the noisiest source today; thincd/audit's own
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

/*
 * ADR-0141 Phase 3: repoints where future segment writes/config saves
 * land, for a live log-storage migration -- unlike every other
 * STATE_DIR-backed module's own *_repoint() (a bare path-buffer
 * update), this one first closes the currently-open segment file and
 * nulls g_current_fp, exactly as this module's own ADR-0141 design
 * note requires: g_current_fp is a persistently-open FILE* held across
 * writes (ensure_current_segment_open() only reopens when it's NULL,
 * not per write), so changing g_dir alone would leave already-open
 * writes still landing on the *old* disk. The next logstore_write()
 * call naturally reopens (in append mode, resuming the same logical
 * segment -- g_next_seq is deliberately untouched here) the already-
 * migrated segment file at new_dir via the existing ensure_current_
 * segment_open() path, no other change needed. g_max_bytes/g_min_level/
 * g_next_seq are all left exactly as they were -- this is a repoint,
 * never a reload.
 */
void logstore_repoint(const char *new_dir, const char *new_state_path);

/* Starts the /dev/kmsg reader -- registered into the caller's own
 * epoll loop (main.c's g_epfd) the same way every other fd this
 * daemon watches already is; returns the fd to add (POLLIN), or -1 if
 * /dev/kmsg couldn't be opened (non-fatal -- logged as a thincd-
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

/*
 * Transparent container-log capture (added alongside kernel/thincd/
 * audit): every container's stdout/stderr is always piped and drained
 * (main.c's handle_container_output_event(), unconditional since this
 * feature landed -- no longer gated behind the per-container
 * "capture_output" opt-in, which now controls only whether
 * GET /v1/containers/{name}'s own captured_output tail is populated,
 * a separate, still-opt-in feature fed from the same underlying pipe).
 * Always writes source="container" and the given container name into
 * its own dedicated "container" field -- a real structured filter for
 * logstore_tail()'s own container_filter, not a substring match against
 * "msg". level is fixed by the caller (main.c currently always passes
 * "info": stdout and stderr are merged onto one pipe, same as
 * captured_output's own long-standing merge, so there's no real signal
 * to derive a per-line severity from without a second pipe -- a stated,
 * documented v1 boundary, not silently assumed).
 */
void logstore_write_container(const char *container, const char *level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/*
 * Maps a level name (any of the real syslog severity names this store
 * already accepts, see logstore_set_min_level()'s own doc comment) to
 * its RFC 5424 numeric severity (0=emerg .. 7=debug, lower is more
 * severe) -- the exact scale syslogfwd.c's RFC 3164 PRI field needs.
 * A thin public wrapper over this file's own internal level_rank(),
 * so that scale is computed in exactly one place rather than a second
 * copy living in syslogfwd.c. Unrecognized input ranks as INFO (6),
 * same permissive-by-construction posture level_rank() already has.
 */
int logstore_level_severity(const char *level);

enum logstore_error logstore_set_max_bytes(int64_t max_bytes);
int64_t logstore_max_bytes(void);

/*
 * min_level must be one of the real syslog severity names
 * (kmsg_level_name()'s own set: "emerg", "alert", "crit", "err" or
 * "error" (both accepted -- thincd/audit's own convention is
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
 * has no index and a very large limit means reading whole segments.
 * Equivalent to logstore_tail_ex() with container_filter/msg_regex
 * both NULL -- kept as its own entry point since every pre-existing
 * caller only ever needed source/level/since/limit. */
void logstore_tail(const char *source_filter, const char *level_filter, int64_t since,
                    int limit, struct json_writer *w);

/*
 * Added for transparent container-log capture: container_filter
 * matches an entry's own "container" field exactly (only ever
 * non-empty on a source="container" entry); msg_regex is a POSIX
 * extended regular expression (REG_ICASE -- case-insensitive, same
 * "least surprise for an operator eyeballing log text" posture grep's
 * own -i default reflects) matched against "msg", NULL/"" meaning no
 * regex filter. A malformed msg_regex matches nothing rather than
 * crashing (main.c's own caller validates and 400s a bad pattern
 * before ever reaching here; this is defense in depth, not the
 * primary validation).
 */
void logstore_tail_ex(const char *source_filter, const char *level_filter,
                       const char *container_filter, const char *msg_regex, int64_t since,
                       int limit, struct json_writer *w);

#endif /* LOGSTORE_H */
