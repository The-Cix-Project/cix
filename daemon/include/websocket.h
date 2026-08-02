#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stddef.h>

/*
 * Minimal RFC 6455 support -- exactly what the container console
 * needs (small keystroke/output frames) and nothing else: no
 * extensions, no subprotocol negotiation, no fragmented-message
 * reassembly (every frame this daemon ever sends or expects is a
 * single complete FIN=1 frame). Payloads are capped defensively, the
 * same "bound it, don't trust the peer" posture HTTP_MAX_REQUEST_SIZE
 * already established for the plain HTTP path.
 */
#define WS_MAX_FRAME_PAYLOAD (64 * 1024)

/*
 * Computes the Sec-WebSocket-Accept value (RFC 6455 4.2.2) for a
 * client's Sec-WebSocket-Key by shelling out to /usr/bin/openssl
 * (sha1 of key+GUID, then base64) -- this codebase's own established
 * convention (daemon/src/pki.c's run_openssl()) is "never link a
 * crypto library into this daemon, always shell to the real openssl
 * binary." Writes a NUL-terminated base64 string (no trailing
 * newline) into out. Returns 0, or -1 on any failure (fork/exec/pipe,
 * or openssl itself exiting nonzero) -- verified against the RFC's
 * own worked example ("dGhlIHNhbXBsZSBub25jZQ==" ->
 * "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=").
 */
int ws_compute_accept(const char *client_key, char *out, size_t out_size);

enum ws_opcode {
	WS_OPCODE_CONTINUATION = 0x0,
	WS_OPCODE_TEXT = 0x1,
	WS_OPCODE_BINARY = 0x2,
	WS_OPCODE_CLOSE = 0x8,
	WS_OPCODE_PING = 0x9,
	WS_OPCODE_PONG = 0xa,
};

/*
 * Writes one complete, unmasked (server-to-client) WS frame -- RFC
 * 6455 5.1 forbids a server from masking its own frames. Returns 0,
 * or -1 on a write error or len > WS_MAX_FRAME_PAYLOAD.
 */
int ws_write_frame(int fd, enum ws_opcode opcode, const void *payload, size_t len);

/*
 * Incremental client-frame parser -- client frames are always masked
 * (RFC 6455 5.1) and, like HTTP requests (see struct http_conn,
 * daemon/include/http.h), can arrive split across many read()s. Unlike
 * http_conn (exactly one request per connection's whole life), a
 * ws_conn keeps parsing frame after frame for as long as the WS
 * session is open -- call ws_conn_consume() after handling each parsed
 * frame before feeding more bytes.
 */
struct ws_conn {
	unsigned char *buf;
	size_t len;
	size_t cap;
};

struct ws_frame {
	enum ws_opcode opcode;
	int fin;
	unsigned char *payload;        /* points into the owning ws_conn's buffer, already unmasked */
	size_t payload_len;
	size_t frame_len;              /* total bytes this frame occupied -- pass to ws_conn_consume() */
};

void ws_conn_init(struct ws_conn *c);
void ws_conn_free(struct ws_conn *c);

/* Appends n bytes to the connection buffer. Returns -1 if a single
 * frame's declared payload would exceed WS_MAX_FRAME_PAYLOAD, 0
 * otherwise. */
int ws_conn_feed(struct ws_conn *c, const void *data, size_t n);

/*
 * Returns 1 and fills *frame (payload already unmasked in place) once
 * a complete frame is buffered, 0 if more data is needed, -1 on a
 * malformed frame or a client frame missing its mandatory mask bit.
 */
int ws_conn_try_parse(struct ws_conn *c, struct ws_frame *frame);

/*
 * Removes frame_len bytes (a just-parsed frame) from the front of the
 * buffer, shifting any remaining already-buffered bytes (e.g. the
 * start of the next frame, arrived in the same read()) down to
 * offset 0.
 */
void ws_conn_consume(struct ws_conn *c, size_t frame_len);

#endif /* WEBSOCKET_H */
