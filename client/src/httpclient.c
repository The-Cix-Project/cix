#include "httpclient.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void kx_client_init(struct kx_client *c, const char *host, int port)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->host, sizeof(c->host), "%s", host);
	c->port = port;
}

static int connect_to(const struct kx_client *c)
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

	fd = connect_to(c);
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

	if (write_all(fd, header, (size_t)header_len) != 0) {
		close(fd);
		return -1;
	}
	if (body != NULL && write_all(fd, body, strlen(body)) != 0) {
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
	if (body_start != NULL) {
		body_start += 4;
		json_len = rb.len - (size_t)(body_start - rb.buf);
		if (json_len > 0)
			out->json = json_parse(body_start, json_len);
	}

	free(rb.buf);
	return 0;
}

void kx_response_free(struct kx_response *r)
{
	json_free(r->json);
	r->json = NULL;
}
