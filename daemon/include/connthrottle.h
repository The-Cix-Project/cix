#ifndef CONNTHROTTLE_H
#define CONNTHROTTLE_H

#include "json.h"

/*
 * Per-source-IP connection throttling, closing a real gap found live
 * on a deployed box: a sustained flood of failed HTTPS handshakes
 * (an untrusting client repeatedly hitting the HTTPS listener, several
 * hundred/minute) had no peer IP in its own log line (nothing to
 * identify the source) and no way to stop the daemon spending a real
 * accept4()+SSL_new()+SSL_accept() attempt on every single one of
 * them -- each one a brand-new TCP+TLS connection, since this daemon
 * deliberately never does HTTP keep-alive.
 *
 * Tracking is in-memory only, not persisted -- the same "an in-flight
 * thing the daemon restarts through is simply lost" posture every
 * other transient daemon state already has (g_iso_build_state,
 * g_pkg_current_job, etc.): a restart is itself a legitimate way to
 * reset a stuck block, and there is no real cost to starting fresh.
 * The *configuration* (whether this is on, and its thresholds) is
 * persisted like every other config resource (ADR-0012's
 * atomic-rewrite convention, via persist.h).
 */

#define CONNTHROTTLE_MAX_ENTRIES 256
#define CONNTHROTTLE_IP_MAX 16 /* "255.255.255.255" + NUL */

struct throttle_config {
	int enabled;
	int threshold;          /* failures within window_seconds before blocking */
	int window_seconds;     /* rolling window the threshold is counted over */
	int block_seconds;      /* how long a tripped IP stays blocked */
	int log_interval_seconds; /* at most one "handshake failed" log line per
	                            * source per this many seconds -- 0 logs every
	                            * single failure (see connthrottle_should_log_
	                            * failure()'s own doc comment for why this
	                            * exists independently of threshold/blocking) */
};

#define CONNTHROTTLE_THRESHOLD_MIN 1
#define CONNTHROTTLE_THRESHOLD_MAX 100000
#define CONNTHROTTLE_WINDOW_MIN 1
#define CONNTHROTTLE_WINDOW_MAX 86400
#define CONNTHROTTLE_BLOCK_MIN 1
#define CONNTHROTTLE_BLOCK_MAX 604800
#define CONNTHROTTLE_LOG_INTERVAL_MIN 0
#define CONNTHROTTLE_LOG_INTERVAL_MAX 3600

/*
 * Loads persisted config from path (defaults -- enabled, 20 failures
 * per 60s, a 5-minute block -- if nothing was ever saved). Must be
 * called once at startup before any other connthrottle_*() call.
 */
int connthrottle_config_init(const char *path);

struct throttle_config connthrottle_config_get(void);

/*
 * Partial update, same convention as pkg_repo_set_config(): -1 on any
 * int field (or enabled_flag) means "leave this field unchanged".
 * Returns 0 on success, -1 if any given field is out of range or the
 * persist write fails.
 */
int connthrottle_config_set(int enabled_flag, int threshold, int window_seconds, int block_seconds,
                             int log_interval_seconds);

/* 1 if ip is currently blocked (and throttling is enabled), 0 otherwise.
 * Enforced by the caller against the HTTPS listener only (ADR-0137,
 * accept_loop()'s is_tls guard) -- only a failed TLS handshake can ever
 * cause a block in the first place, so this is never checked against
 * the plain HTTP listener. */
int connthrottle_should_block(const char *ip);

/*
 * Records a failed TLS handshake from ip -- starts (or continues) that
 * IP's rolling failure window; once threshold is reached within
 * window_seconds, the IP is blocked for block_seconds from the moment
 * threshold was reached. A no-op when throttling is disabled. When the
 * tracking table is full and ip isn't already in it, the attempt is
 * silently not tracked (a real, accepted limit -- 256 simultaneously
 * distinct failing sources is already an extreme case, and evicting an
 * existing tracked/blocked entry to make room for a brand-new one
 * would be actively counterproductive).
 */
void connthrottle_record_failure(const char *ip);

/*
 * 1 if a "handshake failed" log line for ip should actually be written
 * right now (and, as a side effect, marks that a line was just logged
 * for it), 0 if one was already logged for this same source within
 * log_interval_seconds. Independent of connthrottle_record_failure()
 * and of whether throttling itself is enabled -- found live on
 * 192.168.15.95: a real, legitimate desktop's own browser repeatedly
 * failing TLS (an untrusted self-signed cert, not a hostile source)
 * flooded the consolidated log store at 10+ lines/sec, crowding out
 * everything else in its rotation window, well before enough failures
 * from that source would ever justify a block. The failure is still
 * counted every time via connthrottle_record_failure() regardless of
 * this function's own return value -- only the *logging* is throttled,
 * never the accounting a real block still needs to be accurate.
 * Always creates a tracking entry for ip if one doesn't exist yet
 * (unlike connthrottle_record_failure(), not gated on `enabled`), so
 * log rate-limiting works the same whether or not blocking itself is
 * turned on. Table-full is the one case this fails open on (returns 1,
 * logs anyway) -- better to log a few extra lines than silently drop
 * all logging once the tracking table is already saturated.
 */
int connthrottle_should_log_failure(const char *ip);

/*
 * Clears ip's failure count -- called on a successful TLS handshake,
 * and also on any complete, well-formed HTTP request on either
 * listener (main.c's handle_client_event()): real evidence a source
 * isn't currently misbehaving. Still credited from either listener
 * even though enforcement itself is HTTPS-only as of ADR-0137 -- a
 * clean plain-HTTP request is just as real evidence of non-malicious
 * behavior as a clean HTTPS one, and there is no reason to make a
 * source's HTTPS block outlive proof it's behaving normally just
 * because that proof happened to arrive over the other listener. A
 * client that had a handful of transient failures and then behaved
 * normally shouldn't stay one failure away from a block. No-op if ip
 * isn't tracked.
 */
void connthrottle_record_success(const char *ip);

/* {"entries": [{"ip":"...", "fail_count":N, "blocked":bool, "blocked_until":unix-ts-or-0}, ...]} */
void connthrottle_write_status_json(struct json_writer *w);

#endif /* CONNTHROTTLE_H */
