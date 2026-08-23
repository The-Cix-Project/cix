#include "console.h"
#include "iohelpers.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define WS_MAX_FRAME_PAYLOAD (64 * 1024)

static const char BASE64_ALPHABET[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Not a security-sensitive operation (RFC 6455's own Sec-WebSocket-Key
 * is a protocol nonce, not a secret) -- plain data encoding, unlike
 * this project's own "never implement crypto, shell to openssl"
 * convention, which is specifically about hashes/ciphers/signatures. */
static void base64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_size)
{
	size_t i, o = 0;
	unsigned int v;

	for (i = 0; i + 3 <= in_len && o + 4 < out_size; i += 3) {
		v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8) | in[i + 2];
		out[o++] = BASE64_ALPHABET[(v >> 18) & 0x3f];
		out[o++] = BASE64_ALPHABET[(v >> 12) & 0x3f];
		out[o++] = BASE64_ALPHABET[(v >> 6) & 0x3f];
		out[o++] = BASE64_ALPHABET[v & 0x3f];
	}
	if (i < in_len && o + 4 < out_size) {
		unsigned int b0 = in[i];
		unsigned int b1 = (i + 1 < in_len) ? in[i + 1] : 0;

		v = (b0 << 16) | (b1 << 8);
		out[o++] = BASE64_ALPHABET[(v >> 18) & 0x3f];
		out[o++] = BASE64_ALPHABET[(v >> 12) & 0x3f];
		out[o++] = (i + 1 < in_len) ? BASE64_ALPHABET[(v >> 6) & 0x3f] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
}

static int make_ws_key(char *out, size_t out_size)
{
	unsigned char raw[16];
	int fd;
	ssize_t n;
	size_t total = 0;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;
	while (total < sizeof(raw)) {
		n = read(fd, raw + total, sizeof(raw) - total);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		total += (size_t)n;
	}
	close(fd);
	base64_encode(raw, sizeof(raw), out, out_size);
	return 0;
}

/* Client-to-server frames MUST be masked (RFC 6455 5.1) -- the
 * daemon-side ws_write_frame() (daemon/src/websocket.c) deliberately
 * never masks, since a server never does; this is the client's own,
 * genuinely different role, not a duplicate of that function. */
static int send_masked_frame(int fd, int opcode, const void *payload, size_t len)
{
	unsigned char header[8];
	unsigned char mask[4];
	unsigned char *masked;
	size_t hlen, i;
	int rc;

	if (len > WS_MAX_FRAME_PAYLOAD)
		return -1;
	{
		int rfd = open("/dev/urandom", O_RDONLY);

		if (rfd < 0)
			return -1;
		if (read(rfd, mask, sizeof(mask)) != (ssize_t)sizeof(mask)) {
			close(rfd);
			return -1;
		}
		close(rfd);
	}

	header[0] = (unsigned char)(0x80 | (opcode & 0x0f));
	if (len < 126) {
		header[1] = (unsigned char)(0x80 | len);
		hlen = 2;
	} else {
		header[1] = 0x80 | 126;
		header[2] = (unsigned char)((len >> 8) & 0xff);
		header[3] = (unsigned char)(len & 0xff);
		hlen = 4;
	}

	masked = len > 0 ? malloc(len) : NULL;
	if (len > 0 && masked == NULL)
		return -1;
	for (i = 0; i < len; i++)
		masked[i] = ((const unsigned char *)payload)[i] ^ mask[i % 4];

	rc = thinc_write_all(fd, header, hlen);
	if (rc == 0)
		rc = thinc_write_all(fd, mask, sizeof(mask));
	if (rc == 0 && len > 0)
		rc = thinc_write_all(fd, masked, len);
	free(masked);
	return rc;
}

/* Server-to-client frames are never masked (the daemon's own
 * ws_write_frame() guarantees this); tolerant of a masked frame too
 * (unmasks it) purely for robustness, not because one is expected. */
struct client_ws_buf {
	unsigned char buf[WS_MAX_FRAME_PAYLOAD + 32];
	size_t len;
};

/* Returns 1 and fills *opcode/*payload/*payload_len (payload points
 * into b->buf, still valid until the next call) plus *frame_len on a
 * complete frame, 0 if more data is needed, -1 on a malformed frame. */
static int try_parse_server_frame(struct client_ws_buf *b, int *opcode,
                                   unsigned char **payload, size_t *payload_len, size_t *frame_len)
{
	unsigned char b0, b1;
	int masked;
	size_t len7, header_len, plen;

	if (b->len < 2)
		return 0;
	b0 = b->buf[0];
	b1 = b->buf[1];
	masked = (b1 & 0x80) != 0;
	len7 = b1 & 0x7f;

	header_len = 2;
	if (len7 == 126)
		header_len += 2;
	else if (len7 == 127)
		header_len += 8;
	if (masked)
		header_len += 4;

	if (b->len < header_len)
		return 0;

	if (len7 < 126) {
		plen = len7;
	} else if (len7 == 126) {
		plen = ((size_t)b->buf[2] << 8) | (size_t)b->buf[3];
	} else {
		size_t i;

		plen = 0;
		for (i = 0; i < 8; i++)
			plen = (plen << 8) | (size_t)b->buf[2 + i];
	}
	if (plen > WS_MAX_FRAME_PAYLOAD)
		return -1;
	if (b->len < header_len + plen)
		return 0;

	if (masked) {
		unsigned char mask[4];
		size_t i;

		memcpy(mask, b->buf + header_len - 4, 4);
		for (i = 0; i < plen; i++)
			b->buf[header_len + i] ^= mask[i % 4];
	}

	*opcode = b0 & 0x0f;
	*payload = b->buf + header_len;
	*payload_len = plen;
	*frame_len = header_len + plen;
	return 1;
}

static void consume(struct client_ws_buf *b, size_t frame_len)
{
	if (frame_len >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->buf, b->buf + frame_len, b->len - frame_len);
	b->len -= frame_len;
}

/*
 * Shared WS handshake for both thinc_console_run() (path always
 * /v1/containers/{name}/console, optional X-thinC-Exec-Cmd header)
 * and thinc_pkg_build_log_run() below (fixed path, no such header) --
 * label identifies which one for error messages, path is the exact
 * request-line target to send.
 */
static int do_ws_handshake(const struct thinc_client *c, const char *label, const char *path,
                            const char *exec_cmd_header)
{
	int fd;
	char key[64];
	char req[1024];
	int rlen;
	char resp[2048];
	size_t got = 0;
	ssize_t n;

	if (make_ws_key(key, sizeof(key)) != 0) {
		fprintf(stderr, "%s: could not generate a WebSocket key (/dev/urandom): %s\n", label,
		        strerror(errno));
		return -1;
	}

	fd = thinc_client_connect_raw(c);
	if (fd < 0) {
		fprintf(stderr, "%s: could not connect to %s:%d\n", label, c->host, c->port);
		return -1;
	}

	if (exec_cmd_header != NULL && exec_cmd_header[0] != '\0') {
		rlen = snprintf(req, sizeof(req),
		                 "GET %s HTTP/1.1\r\n"
		                 "Host: %s:%d\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "X-thinC-Exec-Cmd: %s\r\n"
		                 "\r\n",
		                 path, c->host, c->port, key, exec_cmd_header);
	} else {
		rlen = snprintf(req, sizeof(req),
		                 "GET %s HTTP/1.1\r\n"
		                 "Host: %s:%d\r\n"
		                 "Upgrade: websocket\r\n"
		                 "Connection: Upgrade\r\n"
		                 "Sec-WebSocket-Key: %s\r\n"
		                 "Sec-WebSocket-Version: 13\r\n"
		                 "\r\n",
		                 path, c->host, c->port, key);
	}
	if (rlen < 0 || (size_t)rlen >= sizeof(req) || thinc_write_all(fd, req, (size_t)rlen) != 0) {
		fprintf(stderr, "%s: failed to send the upgrade request\n", label);
		close(fd);
		return -1;
	}

	while (got < sizeof(resp) - 1) {
		n = read(fd, resp + got, sizeof(resp) - 1 - got);
		if (n <= 0) {
			fprintf(stderr, "%s: connection closed before a handshake response arrived\n", label);
			close(fd);
			return -1;
		}
		got += (size_t)n;
		resp[got] = '\0';
		if (strstr(resp, "\r\n\r\n") != NULL)
			break;
	}

	if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
		/* task #764: a non-101 response here is respond_error()'s own
		 * JSON body ({"error":"..."}), not a bare status line -- this
		 * used to print only the status line, discarding the actual
		 * reason (e.g. exec_into_container()'s own strerror(errno))
		 * that made a live-only console failure impossible to
		 * diagnose without host shell access. body_start may not
		 * include the full body if it arrived in a later TCP segment
		 * than the headers did (this loop only reads up to the blank
		 * line, never past it) -- best-effort: fall back to the bare
		 * status line if nothing parses, same as before. */
		char *line_end = strstr(resp, "\r\n");
		char *body_start = strstr(resp, "\r\n\r\n");
		const char *detail = NULL;
		struct json_value *errjson = NULL;

		if (line_end != NULL)
			*line_end = '\0';
		if (body_start != NULL) {
			body_start += 4;
			errjson = json_parse(body_start, strlen(body_start));
			if (errjson != NULL) {
				const struct json_value *ev = json_object_get(errjson, "error");

				if (ev != NULL && ev->type == JSON_STRING)
					detail = ev->u.string;
			}
		}
		fprintf(stderr, "%s: %s%s%s\n", label, resp, detail != NULL ? ": " : "",
		        detail != NULL ? detail : "");
		json_free(errjson);
		close(fd);
		return -1;
	}
	return fd;
}

static int set_raw_mode(int fd, struct termios *saved)
{
	struct termios raw;

	if (!isatty(fd))
		return -1;
	if (tcgetattr(fd, saved) != 0)
		return -1;
	raw = *saved;
	cfmakeraw(&raw);
	if (tcsetattr(fd, TCSANOW, &raw) != 0)
		return -1;
	return 0;
}

static void relay(int ws_fd)
{
	struct client_ws_buf inbuf;
	struct pollfd fds[2];
	unsigned char iobuf[4096];

	inbuf.len = 0;

	fds[0].fd = STDIN_FILENO;
	fds[0].events = POLLIN;
	fds[1].fd = ws_fd;
	fds[1].events = POLLIN;

	for (;;) {
		/* Always 2 -- poll(2) already ignores a negative fd entry on
		 * its own (revents left 0), so shrinking nfds once stdin hits
		 * EOF would stop poll() from ever looking at fds[1] (the
		 * actual WebSocket socket) again, hanging forever. Confirmed
		 * the hard way: this exact bug hung a real session after
		 * piped stdin closed, caught via strace before shipping. */
		int pr = poll(fds, 2, -1);

		if (pr < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (fds[0].fd >= 0 && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t n = read(STDIN_FILENO, iobuf, sizeof(iobuf));

			if (n <= 0) {
				/* Local stdin closed -- stop watching it, but keep
				 * relaying output until the remote side closes too. */
				fds[0].fd = -1;
			} else if (send_masked_frame(ws_fd, 0x2, iobuf, (size_t)n) != 0) {
				break;
			}
		}

		if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			ssize_t n;
			int opcode;
			unsigned char *payload;
			size_t payload_len, frame_len;

			n = read(ws_fd, iobuf, sizeof(iobuf));
			if (n <= 0)
				break;
			if (inbuf.len + (size_t)n > sizeof(inbuf.buf))
				break;
			memcpy(inbuf.buf + inbuf.len, iobuf, (size_t)n);
			inbuf.len += (size_t)n;

			for (;;) {
				int r = try_parse_server_frame(&inbuf, &opcode, &payload, &payload_len, &frame_len);

				if (r == 0)
					break;
				if (r < 0)
					return;
				if (opcode == 0x1 || opcode == 0x2) {
					if (thinc_write_all(STDOUT_FILENO, payload, payload_len) != 0)
						return;
				} else if (opcode == 0x8) {
					consume(&inbuf, frame_len);
					return;
				}
				consume(&inbuf, frame_len);
			}
		}
	}
}

int thinc_console_run(const struct thinc_client *c, const char *container_name, const char *cmd)
{
	int fd;
	struct termios saved;
	int have_saved;

	{
		char path[256];

		snprintf(path, sizeof(path), "/v1/containers/%s/console", container_name);
		fd = do_ws_handshake(c, "console", path, cmd);
	}
	if (fd < 0)
		return -1;

	have_saved = set_raw_mode(STDIN_FILENO, &saved) == 0;

	relay(fd);

	if (have_saved)
		tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	printf("\r\nconsole session ended\n");

	close(fd);
	return 0;
}

/*
 * task #676: one-way live-tail relay for GET /v1/pkg/build/log -- no
 * stdin, no raw mode, no send_masked_frame() call at all (this
 * daemon-side stream never expects anything FROM the client but a
 * close frame), just print every TEXT frame straight to stdout as it
 * arrives, same server-frame parser thinc_console_run()'s own relay()
 * already uses.
 */
int thinc_pkg_build_log_run(const struct thinc_client *c)
{
	int fd;
	struct client_ws_buf inbuf;
	unsigned char iobuf[4096];

	fd = do_ws_handshake(c, "pkg build-log", "/v1/pkg/build/log", NULL);
	if (fd < 0)
		return -1;

	inbuf.len = 0;
	for (;;) {
		ssize_t n = read(fd, iobuf, sizeof(iobuf));
		int opcode;
		unsigned char *payload;
		size_t payload_len, frame_len;

		if (n <= 0)
			break;
		if (inbuf.len + (size_t)n > sizeof(inbuf.buf))
			break;
		memcpy(inbuf.buf + inbuf.len, iobuf, (size_t)n);
		inbuf.len += (size_t)n;

		for (;;) {
			int r = try_parse_server_frame(&inbuf, &opcode, &payload, &payload_len, &frame_len);

			if (r == 0)
				break;
			if (r < 0)
				goto done;
			if (opcode == 0x1 || opcode == 0x2) {
				if (thinc_write_all(STDOUT_FILENO, payload, payload_len) != 0)
					goto done;
			} else if (opcode == 0x8) {
				consume(&inbuf, frame_len);
				goto done;
			}
			consume(&inbuf, frame_len);
		}
	}

done:
	printf("\n--- build log stream ended ---\n");
	close(fd);
	return 0;
}
