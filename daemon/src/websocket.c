#include "websocket.h"
#include "iohelpers.h"
#include "tlsconn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#define WS_MAGIC_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/*
 * #351: the handshake needs one SHA-1 of a fixed-format string and its
 * base64 -- cixd already links -lcrypto, so this computed it in-process
 * against the linked library instead of forking two openssl CLI
 * invocations (dgst -sha1, then base64) to get there. EVP_EncodeBlock()
 * is openssl's own "no line wrapping" encoder, the in-process twin of
 * the CLI's -A flag this replaced.
 */
int ws_compute_accept(const char *client_key, char *out, size_t out_size)
{
	char concat[256];
	int clen;
	unsigned char sha1[SHA_DIGEST_LENGTH];
	unsigned char b64[64]; /* SHA_DIGEST_LENGTH (20) -> base64 is exactly 28 chars, plenty of slack */
	int b64_len;

	clen = snprintf(concat, sizeof(concat), "%s%s", client_key, WS_MAGIC_GUID);
	if (clen < 0 || (size_t)clen >= sizeof(concat))
		return -1;

	SHA1((const unsigned char *)concat, (size_t)clen, sha1);

	b64_len = EVP_EncodeBlock(b64, sha1, sizeof(sha1));
	if (b64_len < 0 || (size_t)b64_len + 1 > out_size)
		return -1;
	memcpy(out, b64, (size_t)b64_len);
	out[b64_len] = '\0';
	return 0;
}

int ws_write_frame(int fd, enum ws_opcode opcode, const void *payload, size_t len)
{
	unsigned char header[4];
	size_t hlen;

	if (len > WS_MAX_FRAME_PAYLOAD)
		return -1;

	/* FIN=1, RSV1-3=0, no extensions negotiated. Servers MUST NOT mask
	 * their own frames (RFC 6455 5.1). len is capped at
	 * WS_MAX_FRAME_PAYLOAD (64KiB), so the 16-bit extended-length form
	 * always suffices -- the 64-bit form is never needed for anything
	 * this daemon itself sends. */
	header[0] = (unsigned char)(0x80 | (opcode & 0x0f));
	if (len < 126) {
		header[1] = (unsigned char)len;
		hlen = 2;
	} else {
		header[1] = 126;
		header[2] = (unsigned char)((len >> 8) & 0xff);
		header[3] = (unsigned char)(len & 0xff);
		hlen = 4;
	}

	if (tls_write_all(fd, header, hlen) != 0)
		return -1;
	if (len > 0 && tls_write_all(fd, payload, len) != 0)
		return -1;
	return 0;
}

void ws_conn_init(struct ws_conn *c)
{
	c->buf = NULL;
	c->len = 0;
	c->cap = 0;
}

void ws_conn_free(struct ws_conn *c)
{
	free(c->buf);
	c->buf = NULL;
	c->len = 0;
	c->cap = 0;
}

int ws_conn_feed(struct ws_conn *c, const void *data, size_t n)
{
	size_t ncap;
	unsigned char *nb;

	/* A single frame's header (at most 14 bytes: 2 base + 8 extended
	 * length + 4 mask key) plus its capped payload is the most this
	 * buffer should ever need to hold -- bound it the same defensive
	 * way struct http_conn bounds itself against HTTP_MAX_REQUEST_SIZE. */
	if (c->len + n > WS_MAX_FRAME_PAYLOAD + 14)
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

int ws_conn_try_parse(struct ws_conn *c, struct ws_frame *frame)
{
	unsigned char b0, b1;
	int fin, opcode, masked;
	size_t len7, header_len, payload_len;
	unsigned char mask_key[4];
	size_t i;

	if (c->len < 2)
		return 0;

	b0 = c->buf[0];
	b1 = c->buf[1];

	fin = (b0 & 0x80) != 0;
	if (b0 & 0x70) /* RSV1-3 must be 0 -- no extensions negotiated */
		return -1;
	if (!fin) /* fragmented messages are never sent or expected here (see websocket.h) */
		return -1;
	opcode = b0 & 0x0f;

	masked = (b1 & 0x80) != 0;
	len7 = b1 & 0x7f;

	header_len = 2;
	if (len7 == 126)
		header_len += 2;
	else if (len7 == 127)
		header_len += 8;
	if (masked)
		header_len += 4;

	if (c->len < header_len)
		return 0;

	if (len7 < 126) {
		payload_len = len7;
	} else if (len7 == 126) {
		payload_len = ((size_t)c->buf[2] << 8) | (size_t)c->buf[3];
	} else {
		payload_len = 0;
		for (i = 0; i < 8; i++)
			payload_len = (payload_len << 8) | (size_t)c->buf[2 + i];
	}

	if (payload_len > WS_MAX_FRAME_PAYLOAD)
		return -1;
	if (!masked) /* RFC 6455 5.1: a server MUST reject an unmasked client frame, never treat it as if masked */
		return -1;

	if (c->len < header_len + payload_len)
		return 0;

	memcpy(mask_key, c->buf + header_len - 4, 4);

	frame->payload = c->buf + header_len;
	frame->payload_len = payload_len;
	for (i = 0; i < payload_len; i++)
		frame->payload[i] ^= mask_key[i % 4];

	frame->opcode = (enum ws_opcode)opcode;
	frame->fin = fin;
	frame->frame_len = header_len + payload_len;
	return 1;
}

void ws_conn_consume(struct ws_conn *c, size_t frame_len)
{
	if (frame_len >= c->len) {
		c->len = 0;
		return;
	}
	memmove(c->buf, c->buf + frame_len, c->len - frame_len);
	c->len -= frame_len;
}
