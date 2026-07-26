#ifndef STATICFILE_H
#define STATICFILE_H

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
void static_serve(int fd, const char *web_root, const char *req_path);

#endif /* STATICFILE_H */
