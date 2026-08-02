#include "httpclient.h"
#include "iohelpers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

void kx_client_init(struct kx_client *c, const char *host, int port)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->host, sizeof(c->host), "%s", host);
	c->port = port;
}

int kx_client_connect_raw(const struct kx_client *c)
{
	int fd;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)c->port);
	if (inet_pton(AF_INET, c->host, &addr.sin_addr) != 1) {
		close(fd);
		return -1;
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Dynamically-growing read buffer -- same doubling pattern already
 * used by daemon/src/http.c's http_conn_feed(), so a large container
 * listing is never silently truncated by a fixed-size cap. */
struct read_buf {
	char *buf;
	size_t len;
	size_t cap;
};

static int read_buf_grow(struct read_buf *rb, size_t want_extra)
{
	size_t ncap;
	char *nb;

	if (rb->len + want_extra <= rb->cap)
		return 0;

	ncap = rb->cap != 0 ? rb->cap * 2 : 4096;
	while (ncap < rb->len + want_extra)
		ncap *= 2;

	nb = realloc(rb->buf, ncap);
	if (nb == NULL)
		return -1;
	rb->buf = nb;
	rb->cap = ncap;
	return 0;
}

static int read_all_response(int fd, struct read_buf *rb)
{
	ssize_t n;

	rb->buf = NULL;
	rb->len = 0;
	rb->cap = 0;

	for (;;) {
		if (read_buf_grow(rb, 4096) != 0)
			return -1;
		n = read(fd, rb->buf + rb->len, rb->cap - rb->len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		rb->len += (size_t)n;
	}
	return 0;
}

/* headers spans the status line plus every header line (each ending
 * "\r\n"), i.e. the header block excluding the final blank-line
 * "\r\n" that terminates it -- same convention as daemon/src/http.c's
 * find_content_length(), applied here to a response instead of a
 * request. Writes an empty string to out if name isn't present. */
static void find_header_value(const char *headers, size_t headers_len, const char *name,
                               char *out, size_t out_size)
{
	size_t name_len = strlen(name);
	size_t i = 0;

	out[0] = '\0';

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
			size_t vlen;

			while (vstart < line_len && headers[line_start + vstart] == ' ')
				vstart++;
			vlen = line_len - vstart;
			if (vlen >= out_size)
				vlen = out_size - 1;
			memcpy(out, headers + line_start + vstart, vlen);
			out[vlen] = '\0';
			return;
		}

		i += 2;
	}
}

int kx_client_request(const struct kx_client *c, const char *method, const char *path,
                       const char *body, struct kx_response *out)
{
	int fd;
	char header[512];
	int header_len;
	struct read_buf rb;
	char *body_start;
	int status;
	size_t json_len;

	fd = kx_client_connect_raw(c);
	if (fd < 0)
		return -1;

	if (body != NULL) {
		header_len = snprintf(header, sizeof(header),
		                       "%s %s HTTP/1.1\r\nHost: %s\r\n"
		                       "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
		                       method, path, c->host, strlen(body));
	} else {
		header_len = snprintf(header, sizeof(header), "%s %s HTTP/1.1\r\nHost: %s\r\n\r\n",
		                       method, path, c->host);
	}
	if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
		close(fd);
		return -1;
	}

	if (kx_write_all(fd, header, (size_t)header_len) != 0) {
		close(fd);
		return -1;
	}
	if (body != NULL && kx_write_all(fd, body, strlen(body)) != 0) {
		close(fd);
		return -1;
	}

	if (read_all_response(fd, &rb) != 0) {
		close(fd);
		free(rb.buf);
		return -1;
	}
	close(fd);

	/* Guarantee a trailing NUL so sscanf()/strstr() below never read
	 * past what we actually received. */
	if (read_buf_grow(&rb, 1) != 0) {
		free(rb.buf);
		return -1;
	}
	rb.buf[rb.len] = '\0';

	if (sscanf(rb.buf, "HTTP/1.1 %d", &status) != 1) {
		free(rb.buf);
		return -1;
	}
	out->status = status;

	body_start = strstr(rb.buf, "\r\n\r\n");
	out->json = NULL;
	out->body = NULL;
	out->body_len = 0;
	out->content_type[0] = '\0';
	if (body_start != NULL) {
		size_t headers_len = (size_t)(body_start - rb.buf);

		find_header_value(rb.buf, headers_len, "Content-Type", out->content_type,
		                   sizeof(out->content_type));

		body_start += 4;
		json_len = rb.len - (size_t)(body_start - rb.buf);
		if (json_len > 0) {
			out->body = malloc(json_len + 1);
			if (out->body != NULL) {
				memcpy(out->body, body_start, json_len);
				out->body[json_len] = '\0';
				out->body_len = json_len;
			}
			out->json = json_parse(body_start, json_len);
		}
	}

	free(rb.buf);
	return 0;
}

void kx_response_free(struct kx_response *r)
{
	json_free(r->json);
	r->json = NULL;
	free(r->body);
	r->body = NULL;
	r->body_len = 0;
}
