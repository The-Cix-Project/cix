#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include "json.h"

/*
 * A small, reusable HTTP/1.1 client for talking to the Kanxeo REST
 * daemon (docs/api/openapi.yaml) -- used by kanxeoctl (cli/) and by
 * the daemon's own test suite (test/test_daemon.c), so there is one
 * implementation of "how to talk to the API," not two.
 */

struct kx_client {
	char host[64];
	int port;
};

#define KX_CONTENT_TYPE_MAX 64

struct kx_response {
	int status;
	char content_type[KX_CONTENT_TYPE_MAX]; /* empty string if no Content-Type header was present */
	char *body;                             /* raw response body, NUL-terminated; NULL if empty */
	size_t body_len;
	struct json_value *json; /* NULL if the body was empty or not valid JSON (e.g. a 204) */
};

void kx_client_init(struct kx_client *c, const char *host, int port);

/*
 * Opens a raw, connected TCP socket to c's host/port -- the same
 * connect logic kx_client_request() uses internally, exposed for
 * callers that need the fd itself rather than one request/response
 * round trip (currently only client/src/console.c's WebSocket upgrade,
 * which kx_client_request() has no way to express: the connection
 * outlives a single response). Returns the fd, or -1 on failure.
 */
int kx_client_connect_raw(const struct kx_client *c);

/*
 * Performs one request/response round trip: connects, sends method+
 * path+body (body may be NULL for no request body), reads the full
 * response (dynamically-sized -- no fixed cap on response size). On
 * transport failure (connect/write/read error, or a malformed status
 * line) returns -1 and *out is untouched. On success returns 0 and
 * fills *out; caller must kx_response_free() it.
 */
int kx_client_request(const struct kx_client *c, const char *method, const char *path,
                       const char *body, struct kx_response *out);

void kx_response_free(struct kx_response *r);

#endif /* HTTPCLIENT_H */
