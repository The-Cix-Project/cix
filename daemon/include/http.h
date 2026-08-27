#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define HTTP_MAX_METHOD 8
#define HTTP_MAX_PATH 256

/* Total request size (headers + body) this server will accept; larger
 * requests are rejected before they can exhaust memory. */
#define HTTP_MAX_REQUEST_SIZE (1 << 20) /* 1 MiB */

struct http_request {
	char method[HTTP_MAX_METHOD];
	char path[HTTP_MAX_PATH];
	long content_length;
	char *body;     /* points into the owning http_conn's buffer */
	size_t body_len;
	char *headers;      /* points into the owning http_conn's buffer; request line + every header line, no trailing blank line */
	size_t headers_len;
};

/*
 * Per-connection incremental parse state. The caller appends raw
 * socket bytes via http_conn_feed() (as they arrive, possibly across
 * many recv() calls) and calls http_conn_try_parse() after each feed
 * to check whether a complete request is available yet.
 *
 * v1 is HTTP/1.1 request-line + headers + fixed-length body only:
 * no chunked transfer-encoding, no keep-alive (every response is
 * followed by closing the connection) -- deliberate scope boundaries
 * for a REST control-plane daemon, not oversights. The one exception
 * is a WebSocket Upgrade request (see websocket.h): the connection
 * survives past this one request/response pair, but that's the
 * caller's own doing once it recognizes the Upgrade header via
 * http_find_header() below -- this parser itself still only ever
 * produces exactly one request per connection.
 */
struct http_conn {
	char *buf;
	size_t len;
	size_t cap;
	int headers_end;       /* offset just past the blank line, or -1 if not found yet */
	long content_length;   /* valid once headers_end >= 0 */
	char method[HTTP_MAX_METHOD];
	char path[HTTP_MAX_PATH];
};

void http_conn_init(struct http_conn *c);
void http_conn_free(struct http_conn *c);

/* Appends n bytes to the connection buffer. Returns -1 if this would
 * exceed HTTP_MAX_REQUEST_SIZE, 0 otherwise. */
int http_conn_feed(struct http_conn *c, const char *data, size_t n);

/*
 * Returns 1 and fills *req (pointers into c's own buffer, valid until
 * c is next touched) once a complete request has been buffered, 0 if
 * more data is needed, -1 on a malformed request line/headers.
 */
int http_conn_try_parse(struct http_conn *c, struct http_request *req);

/*
 * Scans a raw header block (request-line + header lines, no trailing
 * blank line -- exactly struct http_request's own headers/headers_len,
 * or http_conn's equivalent internal state) for a header named `name`
 * (case-insensitive, matched by exact length so "X-Foo" never matches
 * "X-Foo-Bar"). On a match, copies the value (leading/trailing spaces
 * trimmed) plus a NUL into out and returns its length; returns -1 if
 * the header is absent or its value doesn't fit in out_size.
 */
long http_find_header(const char *headers, size_t headers_len, const char *name,
                       char *out, size_t out_size);

/*
 * Writes a full HTTP/1.1 response (status line, Content-Type,
 * Content-Length, Connection: close, body) to fd. Assumes fd will not
 * return EAGAIN on write() -- the reactor is expected to put the
 * client fd back into blocking mode before calling this, since
 * responses here are small (at most a modest container listing) and
 * the connection closes immediately after; a slow/malicious reader
 * stalling this write is an accepted v1 limitation, not handled by
 * buffering + EPOLLOUT. Returns 0, or -1 on a real write error.
 */
int http_write_response(int fd, int status, const char *status_text,
                         const char *content_type, const char *body, size_t body_len);

/*
 * As above, plus caller-supplied extra header lines (issue #139) --
 * for a raw-bytes response that needs to describe itself, e.g. a
 * file's mode alongside its content. extra_headers must be
 * well-formed and CRLF-terminated, or NULL.
 */
int http_write_response_hdrs(int fd, int status, const char *status_text,
                              const char *content_type, const char *extra_headers,
                              const char *body, size_t body_len);

/*
 * Puts fd back into blocking mode. Every response-writing call site
 * (respond_json() in main.c, static_serve() in staticfile.c) must call
 * this before http_write_response(), per that function's contract
 * above -- one implementation of the fcntl() call, not one per caller.
 */
int http_set_blocking(int fd);

#endif /* HTTP_H */
