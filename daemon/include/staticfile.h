#ifndef STATICFILE_H
#define STATICFILE_H

#include <stddef.h> /* size_t */

/*
 * Serves req_path from web_root, writing the response directly to fd
 * (200 + file content, or a plain-text 400/404/500). These status
 * responses aren't part of docs/api/openapi.yaml's contract, so
 * they're plain text rather than the API's JSON error envelope --
 * this is asset serving for the dashboard, not an API endpoint.
 *
 * "/" maps to "index.html". Any req_path containing ".." is rejected
 * with 400 before touching the filesystem (no path traversal outside
 * web_root). Self-contained like respond_json() in main.c: puts fd
 * into blocking mode itself before writing, so callers never need to
 * remember to.
 */
/*
 * Serves one file under web_root. req_headers/req_headers_len are the
 * request's own header block (may be NULL/0), read only to honour
 * If-None-Match: every response carries an ETag and Cache-Control:
 * no-cache, so a browser revalidates rather than guessing at freshness
 * (#230), and an unchanged file costs a 304 instead of a download.
 */
void static_serve(int fd, const char *web_root, const char *req_path, const char *req_headers,
                  size_t req_headers_len);

#endif /* STATICFILE_H */
