#ifndef CURLFETCH_H
#define CURLFETCH_H

#include <stddef.h>

/*
 * One in-process libcurl transfer, replacing the twelve
 * execve(PKG_CURL_BIN, ...) call sites this covered (#410/#469).
 *
 * Deliberately synchronous: every call site already forks a child for
 * exactly this reason (#285 -- an unbounded fetch on the reactor thread
 * stalls every other request for as long as it runs), so curlfetch_
 * perform() is meant to be called FROM INSIDE that already-forked
 * child, never from the daemon's own event-loop thread. That preserves
 * today's async shape (pidfd registered on the fork, not a new one)
 * without a second async model (libcurl's multi interface) alongside
 * the epoll loop cixd already has.
 *
 * The three shapes every call site actually used, collapsed into one
 * struct rather than three functions: a plain GET (four sites), a GET
 * with resume-on-retry for a large source tarball across real,
 * documented mid-transfer resets (ADR-0056, one site), and an upload
 * that reports the HTTP status back rather than treating anything but
 * a transport failure as an error (two sites, matching curl's own
 * --write-out "%{http_code}" contract).
 */
struct curlfetch_opts {
	const char *url;
	const char *path; /* GET: written to. PUT: read from. Always required. */
	int upload;        /* 0 = GET url into path; 1 = PUT path's contents to url */

	/* Up to two extra request headers (e.g. an Authorization: line and
	 * an X-Cix-Sha256: line) -- NULL when not needed. Never logged or
	 * placed anywhere an error message could echo them back whole;
	 * see curlfetch_perform()'s own doc comment on redaction. */
	const char *header1;
	const char *header2;

	/* 0 disables the corresponding libcurl option (library default:
	 * unbounded). connect_timeout/max_time are CURLOPT_CONNECTTIMEOUT/
	 * CURLOPT_TIMEOUT; low_speed_limit+low_speed_time together are
	 * CURLOPT_LOW_SPEED_LIMIT/_TIME (abort if the transfer runs below
	 * low_speed_limit bytes/sec for low_speed_time seconds). */
	long connect_timeout;
	long max_time;
	long low_speed_limit;
	long low_speed_time;

	/*
	 * retry_count is the number of RETRIES, so total attempts is
	 * 1 + retry_count -- matching curl(1)'s own --retry N. 0 means a
	 * single attempt, no retry loop at all.
	 *
	 * retry_all_errors mirrors curl(1)'s --retry-all-errors: without
	 * it, only a curated set of transient failures (timeout, connect
	 * failure, 5xx/408/429) is retried; with it, every failure is.
	 * curl(1)'s own default set does NOT cover a raw connection reset,
	 * which is why the one site using this (the large-source-tarball
	 * fetch) needs it explicitly -- see that call site's own comment.
	 *
	 * retry_delay is seconds between attempts; 0 means an exponential
	 * backoff (1s, 2s, 4s, ..., capped at 30s), matching curl(1)'s own
	 * default when --retry-delay is not given.
	 *
	 * resume mirrors curl(1)'s -C -: on a retry, resume from the local
	 * file's current size instead of starting over, so a transfer that
	 * got most of the way through before a reset does not re-fetch
	 * bytes it already has. Only meaningful with upload == 0.
	 */
	int retry_count;
	int retry_all_errors;
	long retry_delay;
	int resume;
};

/*
 * Performs the transfer opts describes. Returns 0 on success.
 *
 * On failure, returns -1 and writes a human-readable description to
 * err (truncated to err_size) -- the real libcurl error, not a bare
 * exit code, which is the whole reason this replaces a subprocess
 * (#410). Never call with a URL whose credentials should stay secret
 * and then pass err on to a log or API response unredacted: this
 * function does not redact anything itself, the same as the argv it
 * replaces never did -- callers already redact via redact_repo_token()
 * before anything reaches a log, and must keep doing so here.
 *
 * out_http_status, when non-NULL, receives the final HTTP status code
 * REGARDLESS of whether curlfetch_perform() returns 0 or -1 for an
 * upload -- matching curl(1)'s own --write-out "%{http_code}" always
 * reporting what the server said even on what curl itself calls a
 * "failure". Left unset (0) for a GET, where CURLOPT_FAILONERROR
 * already turns a bad HTTP status into a transport-level failure.
 */
int curlfetch_perform(const struct curlfetch_opts *opts, long *out_http_status, char *err,
                      size_t err_size);

#endif /* CURLFETCH_H */
