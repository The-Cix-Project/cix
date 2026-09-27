/*
 * One RFC 6455 client reader for the test tree. See test_ws.h for why
 * this is shared rather than written per test (#524).
 */
#include "test_ws.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ws_write_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = write(fd, p + sent, len - sent);

		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		sent += (size_t)n;
	}
	return 0;
}

int ws_reader_adopt_handshake(struct ws_reader *r, int fd, const char *resp, size_t got,
                               const char *headers_end)
{
	size_t off, len;

	memset(r, 0, sizeof(*r));
	r->fd = fd;
	if (headers_end == NULL)
		return 0;
	off = (size_t)(headers_end - resp) + 4;
	if (got <= off)
		return 0;
	len = got - off;
	if (len > sizeof(r->pending)) {
		/*
		 * Refused, not truncated. Keeping a prefix would leave the
		 * frame stream misaligned in exactly the way dropping the
		 * whole leftover does, and the failure would surface later
		 * as frames that never arrived rather than here as a reader
		 * that could not start.
		 */
		fprintf(stderr,
		        "    ws: handshake read carried %zu leftover byte(s), over this reader's %zu "
		        "-- the frame stream cannot be started without them\n",
		        len, sizeof(r->pending));
		r->stop = WS_STOP_OVERFLOW;
		return -1;
	}
	memcpy(r->pending, resp + off, len);
	r->pending_len = len;
	return 0;
}

int ws_read_full(struct ws_reader *r, void *buf, size_t want)
{
	unsigned char *p = buf;
	size_t got = 0;

	while (got < want) {
		ssize_t n;

		/* Anything the handshake over-read is consumed first, in
		 * order, before the socket is touched. */
		if (r->pending_pos < r->pending_len) {
			size_t avail = r->pending_len - r->pending_pos;
			size_t take = (avail > want - got) ? want - got : avail;

			memcpy(p + got, r->pending + r->pending_pos, take);
			r->pending_pos += take;
			got += take;
			continue;
		}
		n = read(r->fd, p + got, want - got);
		if (n == 0) {
			r->stop = WS_STOP_PEER_CLOSED;
			r->last_errno = 0;
			return -1;
		}
		if (n < 0) {
			if (errno == EINTR)
				continue;
			r->last_errno = errno;
			r->stop = (errno == EAGAIN || errno == EWOULDBLOCK) ? WS_STOP_TIMEOUT
			                                                     : WS_STOP_ERROR;
			return -1;
		}
		got += (size_t)n;
	}
	return 0;
}

int ws_recv_frame(struct ws_reader *r, int *out_opcode, unsigned char *out_buf, size_t out_cap,
                   size_t *out_len)
{
	unsigned char hdr[4];
	int opcode;
	size_t len7, payload_len;

	if (ws_read_full(r, hdr, 2) != 0)
		return -1;
	opcode = hdr[0] & 0x0f;
	len7 = hdr[1] & 0x7f;

	if (len7 == 126) {
		unsigned char ext[2];

		if (ws_read_full(r, ext, 2) != 0)
			return -1;
		payload_len = ((size_t)ext[0] << 8) | (size_t)ext[1];
	} else if (len7 == 127) {
		/*
		 * The 64-bit form. The daemon's own writer caps a payload at
		 * 64 KiB and never sends it, so meeting one is a protocol
		 * fault to report -- not a length to go on and read as 127
		 * bytes, which is what silently treating it as len7 would do.
		 */
		fprintf(stderr, "    ws: unexpected 64-bit length frame\n");
		r->stop = WS_STOP_PROTOCOL;
		return -1;
	} else {
		payload_len = len7;
	}
	if (payload_len > out_cap) {
		fprintf(stderr, "    ws: %zu-byte frame exceeds the caller's %zu-byte buffer\n",
		        payload_len, out_cap);
		r->stop = WS_STOP_OVERFLOW;
		return -1;
	}
	/* The same fill the header uses -- one implementation, not a
	 * second copy of it three lines further down. */
	if (payload_len > 0 && ws_read_full(r, out_buf, payload_len) != 0)
		return -1;
	*out_opcode = opcode;
	*out_len = payload_len;
	return 0;
}

int ws_send_frame(int fd, int opcode, const void *payload, size_t len)
{
	unsigned char header[8];
	unsigned char mask[4] = { 0x11, 0x22, 0x33, 0x44 };
	unsigned char *masked;
	size_t hlen;
	size_t i;
	int rc;

	header[0] = (unsigned char)(0x80 | opcode);
	if (len < 126) {
		header[1] = (unsigned char)(0x80 | len);
		hlen = 2;
	} else {
		header[1] = 0x80 | 126;
		header[2] = (unsigned char)((len >> 8) & 0xff);
		header[3] = (unsigned char)(len & 0xff);
		hlen = 4;
	}

	masked = malloc(len);
	if (masked == NULL && len > 0)
		return -1;
	for (i = 0; i < len; i++)
		masked[i] = ((const unsigned char *)payload)[i] ^ mask[i % 4];

	rc = ws_write_all(fd, header, hlen);
	if (rc == 0)
		rc = ws_write_all(fd, mask, 4);
	if (rc == 0 && len > 0)
		rc = ws_write_all(fd, masked, len);
	free(masked);
	return rc;
}

const char *ws_stop_str(const struct ws_reader *r)
{
	switch (r->stop) {
	case WS_STOP_NONE:
		return "no failure recorded";
	case WS_STOP_PEER_CLOSED:
		return "the peer closed the connection";
	case WS_STOP_TIMEOUT:
		return "nothing arrived before the receive timeout";
	case WS_STOP_ERROR:
		return strerror(r->last_errno);
	case WS_STOP_PROTOCOL:
		return "a frame this reader will not parse";
	case WS_STOP_OVERFLOW:
		return "a frame or the handshake leftover did not fit";
	}
	return "unknown";
}
