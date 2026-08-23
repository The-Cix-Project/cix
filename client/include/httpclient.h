#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include "json.h"

/*
 * A small, reusable HTTP/1.1 client for talking to the thinC REST
 * daemon (docs/api/openapi.yaml) -- used by thincctl (cli/) and by
 * the daemon's own test suite (test/test_daemon.c), so there is one
 * implementation of "how to talk to the API," not two.
 */

struct thinc_client {
	char host[64];
	int port;
	/* ADR-0144: an optional session token, attached automatically by
	 * plain thinc_client_request() (see thinc_client_set_token() below) once
	 * set -- 64 comfortably fits the daemon's own real 48-hex-char
	 * token (HOSTAUTH_TOKEN_LEN, daemon/include/hostauth.h) without
	 * this client-side header needing to depend on that daemon-only
	 * one. Empty means "no session," identical to today's behavior. */
	char token[64];
};

#define THINC_CONTENT_TYPE_MAX 64

struct thinc_response {
	int status;
	char content_type[THINC_CONTENT_TYPE_MAX]; /* empty string if no Content-Type header was present */
	char *body;                             /* raw response body, NUL-terminated; NULL if empty */
	size_t body_len;
	struct json_value *json; /* NULL if the body was empty or not valid JSON (e.g. a 204) */
};

void thinc_client_init(struct thinc_client *c, const char *host, int port);

/*
 * ADR-0144: sets (or, with token == NULL/"", clears) the session token
 * every subsequent plain thinc_client_request() call on c automatically
 * attaches as its own Authorization header -- callers that already
 * have a session (thincctl's own persisted-token load at startup, the
 * web dashboard's login flow) call this once instead of switching
 * every call site over to thinc_client_request_with_auth() themselves.
 */
void thinc_client_set_token(struct thinc_client *c, const char *token);

/*
 * Opens a raw, connected TCP socket to c's host/port -- the same
 * connect logic thinc_client_request() uses internally, exposed for
 * callers that need the fd itself rather than one request/response
 * round trip (currently only client/src/console.c's WebSocket upgrade,
 * which thinc_client_request() has no way to express: the connection
 * outlives a single response). Returns the fd, or -1 on failure.
 */
int thinc_client_connect_raw(const struct thinc_client *c);

/*
 * Performs one request/response round trip: connects, sends method+
 * path+body (body may be NULL for no request body), reads the full
 * response (dynamically-sized -- no fixed cap on response size). On
 * transport failure (connect/write/read error, or a malformed status
 * line) returns -1 and *out is untouched. On success returns 0 and
 * fills *out; caller must thinc_response_free() it.
 *
 * issue #20: internally retries a transport-level failure up to
 * THINC_CLIENT_MAX_ATTEMPTS times with a short fixed backoff before
 * finally giving up and returning -1 -- a single transient blip (the
 * daemon mid-restart, one dropped packet) no longer looks identical to
 * a genuinely down daemon on the very first attempt. Never retries
 * after a real HTTP response of any status -- an error status is a
 * real, deterministic answer, returned immediately, not retried.
 */
int thinc_client_request(const struct thinc_client *c, const char *method, const char *path,
                       const char *body, struct thinc_response *out);

/*
 * Same contract as thinc_client_request() (including its retry behavior),
 * with a real "Authorization: Bearer <token>" header attached --
 * ADR-0144's own host-auth work (thincctl's own login/logout, and
 * every write command once a session is active, plus this daemon's
 * own test suite). token may be NULL (identical to a plain
 * thinc_client_request() call in that case) -- callers that don't yet
 * have a session use this directly rather than needing two
 * near-duplicate call sites.
 */
int thinc_client_request_with_auth(const struct thinc_client *c, const char *method, const char *path,
                                 const char *token, const char *body, struct thinc_response *out);

void thinc_response_free(struct thinc_response *r);

#endif /* HTTPCLIENT_H */
