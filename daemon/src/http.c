#include "http.h"
#include "iohelpers.h"
#include "tlsconn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>

void http_conn_init(struct http_conn *c)
{
	c->buf = NULL;
	c->len = 0;
	c->cap = 0;
	c->headers_end = -1;
	c->content_length = -1;
	c->method[0] = '\0';
	c->path[0] = '\0';
}

void http_conn_free(struct http_conn *c)
{
	free(c->buf);
	c->buf = NULL;
	c->len = 0;
	c->cap = 0;
}

int http_conn_feed(struct http_conn *c, const char *data, size_t n)
{
	size_t ncap;
	char *nb;

	if (c->len + n > HTTP_MAX_REQUEST_SIZE)
		return -1;

	if (c->len + n > c->cap) {
		ncap = c->cap != 0 ? c->cap * 2 : 4096;
		while (ncap < c->len + n)
			ncap *= 2;
		nb = realloc(c->buf, ncap);
		if (nb == NULL)
			return -1;
		c->buf = nb;
		c->cap = ncap;
	}

	memcpy(c->buf + c->len, data, n);
	c->len += n;
	return 0;
}

static const char *find_headers_end(const char *buf, size_t len, size_t *end_off)
{
	size_t i;

	for (i = 0; i + 4 <= len; i++) {
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
		    buf[i + 3] == '\n') {
			*end_off = i + 4;
			return buf + i;
		}
	}
	return NULL;
}

static int parse_request_line(const char *buf, size_t line_len, char *method, char *path)
{
	size_t i = 0, start;
	size_t method_len, path_len;

	start = i;
	while (i < line_len && buf[i] != ' ')
		i++;
	method_len = i - start;
	if (method_len == 0 || method_len >= HTTP_MAX_METHOD || i >= line_len)
		return -1;
	memcpy(method, buf + start, method_len);
	method[method_len] = '\0';

	i++;
	start = i;
	while (i < line_len && buf[i] != ' ')
		i++;
	path_len = i - start;
	if (path_len == 0 || path_len >= HTTP_MAX_PATH)
		return -1;
	memcpy(path, buf + start, path_len);
	path[path_len] = '\0';

	/* Remainder is expected to be "HTTP/1.1"; not otherwise validated. */
	return 0;
}

/* headers_len spans the request line plus every header line, each
 * ending in "\r\n" -- i.e. the full header block excluding the final
 * blank-line "\r\n" that terminates it. Shared by find_content_length()
 * below and every external caller that needs a specific header (e.g.
 * the WebSocket upgrade handshake's Upgrade/Connection/Sec-WebSocket-*
 * headers) -- one header scanner, not one per header name. */
long http_find_header(const char *headers, size_t headers_len, const char *name,
                       char *out, size_t out_size)
{
	size_t i = 0;
	size_t name_len = strlen(name);

	while (i < headers_len && !(headers[i] == '\r' && i + 1 < headers_len && headers[i + 1] == '\n'))
		i++;
	if (i < headers_len)
		i += 2;

	while (i < headers_len) {
		size_t line_start = i;
		size_t line_len;
		size_t colon;

		while (i < headers_len &&
		       !(headers[i] == '\r' && i + 1 < headers_len && headers[i + 1] == '\n'))
			i++;
		line_len = i - line_start;

		colon = 0;
		while (colon < line_len && headers[line_start + colon] != ':')
			colon++;

		if (colon < line_len && colon == name_len &&
		    strncasecmp(headers + line_start, name, name_len) == 0) {
			size_t vstart = colon + 1;
			size_t vend = line_len;

			while (vstart < line_len && headers[line_start + vstart] == ' ')
				vstart++;
			while (vend > vstart && headers[line_start + vend - 1] == ' ')
				vend--;

			if (vend - vstart + 1 <= out_size) {
				memcpy(out, headers + line_start + vstart, vend - vstart);
				out[vend - vstart] = '\0';
				return (long)(vend - vstart);
			}
			return -1;
		}

		i += 2;
	}
	return -1;
}

static long find_content_length(const char *buf, size_t headers_len)
{
	char tmp[32];

	if (http_find_header(buf, headers_len, "Content-Length", tmp, sizeof(tmp)) < 0)
		return -1;
	return atol(tmp);
}

int http_conn_try_parse(struct http_conn *c, struct http_request *req)
{
	if (c->headers_end < 0) {
		size_t hdr_end_off;
		size_t headers_block_len;
		size_t line_len = 0;

		if (find_headers_end(c->buf, c->len, &hdr_end_off) == NULL)
			return 0;

		c->headers_end = (int)hdr_end_off;
		headers_block_len = hdr_end_off - 2;

		while (line_len + 1 < headers_block_len &&
		       !(c->buf[line_len] == '\r' && c->buf[line_len + 1] == '\n'))
			line_len++;

		if (parse_request_line(c->buf, line_len, c->method, c->path) != 0)
			return -1;

		c->content_length = find_content_length(c->buf, headers_block_len);
		if (c->content_length < 0)
			c->content_length = 0;
	}

	if (c->len < (size_t)c->headers_end + (size_t)c->content_length)
		return 0;

	memcpy(req->method, c->method, sizeof(req->method));
	memcpy(req->path, c->path, sizeof(req->path));
	req->content_length = c->content_length;
	req->body = c->buf + c->headers_end;
	req->body_len = (size_t)c->content_length;
	req->headers = c->buf;
	req->headers_len = (size_t)c->headers_end - 2;
	return 1;
}

int http_write_response(int fd, int status, const char *status_text,
                         const char *content_type, const char *body, size_t body_len)
{
	return http_write_response_hdrs(fd, status, status_text, content_type, NULL, body, body_len);
}

/*
 * Issue #139: the same response, plus caller-supplied extra header
 * lines. Exists because a raw byte body can carry no metadata about
 * itself, and that turned out to matter: GET /containers/{name}/files
 * returned a file's CONTENT with no way to learn its mode, so a tool
 * copying an installed tree through this API produced non-executable
 * binaries, checksummed them successfully, cached them, distributed
 * them, and only failed much later at execve.
 *
 * Headers rather than a JSON envelope, deliberately: the body stays
 * exactly the bytes it always was, so every existing consumer is
 * untouched, and a new one can ask for what it needs. Same shape the
 * artifact server's own X-Cix-Sha256 already uses.
 *
 * extra_headers, when non-NULL, must already be well-formed and
 * CRLF-terminated ("X-A: 1\r\nX-B: 2\r\n"); it is caller-built from
 * fixed formats, never from request input.
 */
int http_write_response_hdrs(int fd, int status, const char *status_text,
                              const char *content_type, const char *extra_headers,
                              const char *body, size_t body_len)
{
	char header[512];
	int hlen;

	hlen = snprintf(header, sizeof(header),
	                "HTTP/1.1 %d %s\r\n"
	                "Content-Type: %s\r\n"
	                "Content-Length: %zu\r\n"
	                "%s"
	                "Connection: close\r\n"
	                "\r\n",
	                status, status_text, content_type, body_len,
	                extra_headers != NULL ? extra_headers : "");
	if (hlen < 0 || (size_t)hlen >= sizeof(header))
		return -1;

	if (tls_write_all(fd, header, (size_t)hlen) != 0)
		return -1;
	if (body_len > 0 && tls_write_all(fd, body, body_len) != 0)
		return -1;
	return 0;
}

int http_set_blocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}
