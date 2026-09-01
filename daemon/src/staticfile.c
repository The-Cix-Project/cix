#include "staticfile.h"
#include "http.h"

#include <inttypes.h>

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *content_type_for(const char *path)
{
	size_t len = strlen(path);

	if (len >= 5 && strcmp(path + len - 5, ".html") == 0)
		return "text/html";
	if (len >= 3 && strcmp(path + len - 3, ".js") == 0)
		return "application/javascript";
	if (len >= 4 && strcmp(path + len - 4, ".css") == 0)
		return "text/css";
	if (len >= 4 && strcmp(path + len - 4, ".svg") == 0)
		return "image/svg+xml";
	return "application/octet-stream";
}

static void respond_plain(int fd, int status, const char *status_text, const char *msg)
{
	http_set_blocking(fd);
	http_write_response(fd, status, status_text, "text/plain", msg, strlen(msg));
}

/*
 * Issue #230: the dashboard was served with no caching information of
 * any kind -- no Cache-Control, no ETag, no Last-Modified -- and its
 * assets are referenced without a version marker. A browser given no
 * information is free to invent a freshness lifetime and keep what it
 * has, so a deployed change could stay invisible. That cost real time:
 * a stale app.js made LED behaviour look broken while the source said
 * otherwise, and the report was investigated against code the browser
 * was not running.
 *
 * The answer is validation, not expiry. `no-cache` does not mean "do
 * not store" -- it means "store, but revalidate before reuse", so the
 * browser keeps the file and asks each time whether it still holds.
 * With an ETag that question is answered by a 304 carrying no body, so
 * being always-correct costs a round trip rather than a download.
 *
 * The tag is mtime and size rather than a content hash. A hash would be
 * stronger, and would mean reading and hashing every file on every
 * request to answer a question that is almost always "unchanged". Two
 * files that share an mtime to the second AND a byte count are the case
 * this misses, which for a deployed asset -- rewritten wholesale by an
 * install, never edited in place -- is not a case that arises.
 */
static void build_etag(const struct stat *st, char *out, size_t out_size)
{
	snprintf(out, out_size, "\"%llx-%llx\"", (unsigned long long)st->st_mtime,
	         (unsigned long long)st->st_size);
}

/*
 * An intermediary is allowed to weaken a strong validator, so a tag
 * this server sent as "abc" can come back as W/"abc". Skipping the
 * prefix costs two lines; not skipping it means every such request
 * quietly re-downloads a file the client already has, which is the
 * failure this whole change exists to remove -- and it would be
 * invisible, since serving a correct 200 looks like working.
 *
 * A comma-separated list of tags is not handled: this server sends one
 * tag per file, so a client has one to echo. A longer value fails
 * http_find_header's buffer check, which returns -1 and falls through
 * to a plain 200 -- correct, just not optimal.
 */
static const char *etag_match_start(const char *v)
{
	if (v[0] == 'W' && v[1] == '/')
		return v + 2;
	return v;
}

void static_serve(int fd, const char *web_root, const char *req_path, const char *req_headers,
                  size_t req_headers_len)
{
	char full_path[PATH_MAX];
	char etag[64];
	char inm[128];
	char extra[192];
	const char *rel = req_path;
	int file_fd;
	struct stat st;
	char *buf;
	size_t total;
	ssize_t n;

	if (strstr(req_path, "..") != NULL) {
		respond_plain(fd, 400, "Bad Request", "bad path\n");
		return;
	}

	if (strcmp(rel, "/") == 0)
		rel = "/index.html";

	if (snprintf(full_path, sizeof(full_path), "%s%s", web_root, rel) >= (int)sizeof(full_path)) {
		respond_plain(fd, 400, "Bad Request", "path too long\n");
		return;
	}

	file_fd = open(full_path, O_RDONLY);
	if (file_fd < 0) {
		respond_plain(fd, 404, "Not Found", "not found\n");
		return;
	}

	if (fstat(file_fd, &st) != 0) {
		close(file_fd);
		respond_plain(fd, 500, "Internal Server Error", "error\n");
		return;
	}

	build_etag(&st, etag, sizeof(etag));
	snprintf(extra, sizeof(extra), "ETag: %s\r\nCache-Control: no-cache\r\n", etag);

	/*
	 * A matching validator means the browser already has these exact
	 * bytes, so 304 and nothing else. The ETag is repeated because a
	 * 304 is a full replacement for the response the client would
	 * otherwise have cached, and dropping it there would leave the
	 * next request with nothing to revalidate against.
	 */
	if (req_headers != NULL &&
	    http_find_header(req_headers, req_headers_len, "If-None-Match", inm, sizeof(inm)) >= 0 &&
	    strcmp(etag_match_start(inm), etag) == 0) {
		close(file_fd);
		http_set_blocking(fd);
		http_write_response_hdrs(fd, 304, "Not Modified", content_type_for(full_path), extra,
		                          NULL, 0);
		return;
	}

	buf = st.st_size > 0 ? malloc((size_t)st.st_size) : NULL;
	if (st.st_size > 0 && buf == NULL) {
		close(file_fd);
		respond_plain(fd, 500, "Internal Server Error", "error\n");
		return;
	}

	total = 0;
	while (total < (size_t)st.st_size) {
		n = read(file_fd, buf + total, (size_t)st.st_size - total);
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	close(file_fd);

	http_set_blocking(fd);
	http_write_response_hdrs(fd, 200, "OK", content_type_for(full_path), extra, buf, total);
	free(buf);
}
