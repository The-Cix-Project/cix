#include "http.h"

#include <errno.h>
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
 * blank-line "\r\n" that terminates it. */
static long find_content_length(const char *buf, size_t headers_len)
{
	size_t i = 0;
	long result = -1;

	while (i < headers_len && !(buf[i] == '\r' && i + 1 < headers_len && buf[i + 1] == '\n'))
		i++;
	if (i < headers_len)
		i += 2;

	while (i < headers_len) {
		size_t line_start = i;
		size_t line_len;
		size_t colon;

		while (i < headers_len &&
		       !(buf[i] == '\r' && i + 1 < headers_len && buf[i + 1] == '\n'))
			i++;
		line_len = i - line_start;

		colon = 0;
		while (colon < line_len && buf[line_start + colon] != ':')
			colon++;

		if (colon < line_len && colon == 14 &&
		    strncasecmp(buf + line_start, "Content-Length", 14) == 0) {
			size_t vstart = colon + 1;
			char tmp[32];
			size_t vlen;

			while (vstart < line_len && buf[line_start + vstart] == ' ')
				vstart++;
			vlen = line_len - vstart;
			if (vlen < sizeof(tmp)) {
				memcpy(tmp, buf + line_start + vstart, vlen);
				tmp[vlen] = '\0';
				result = atol(tmp);
			}
		}

		i += 2;
	}
	return result;
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
	return 1;
}

static int write_all(int fd, const char *buf, size_t n)
{
	size_t written = 0;
	ssize_t w;

	while (written < n) {
		w = write(fd, buf + written, n - written);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		written += (size_t)w;
	}
	return 0;
}

int http_write_response(int fd, int status, const char *status_text,
                         const char *content_type, const char *body, size_t body_len)
{
	char header[256];
	int hlen;

	hlen = snprintf(header, sizeof(header),
	                "HTTP/1.1 %d %s\r\n"
	                "Content-Type: %s\r\n"
	                "Content-Length: %zu\r\n"
	                "Connection: close\r\n"
	                "\r\n",
	                status, status_text, content_type, body_len);
	if (hlen < 0 || (size_t)hlen >= sizeof(header))
		return -1;

	if (write_all(fd, header, (size_t)hlen) != 0)
		return -1;
	if (body_len > 0 && write_all(fd, body, body_len) != 0)
		return -1;
	return 0;
}
