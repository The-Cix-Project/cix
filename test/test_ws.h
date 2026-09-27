#ifndef TEST_WS_H
#define TEST_WS_H

/*
 * One RFC 6455 client reader for the test tree (#524).
 *
 * WHY THIS EXISTS, rather than each test speaking the protocol itself:
 * two of them did, and the same bug was paid for twice.
 *
 * `do_ws_handshake()` in the daemon, and every hand-rolled client,
 * share one hazard: the read that finds `\r\n\r\n` OWNS THE FRONT OF
 * THE FRAME STREAM. Loopback TCP coalesces the 101 with whatever the
 * daemon writes next, so the bytes past the header terminator are not
 * spare -- they are the first frame. Dropping them does not cost one
 * frame, it MISALIGNS the stream: the next two bytes read are payload
 * parsed as a frame header, and every later frame including the close
 * is swallowed.
 *
 * #331 fixed that in the daemon and in test_console_exec. #519 paid
 * for it again two weeks later in test_pkg_build_log, which had copied
 * the pattern but not the fix -- measured on 192.168.15.95,
 * 2026-09-25 (probe-cix-testreport@38-1): all ten runs carried
 * leftover bytes and three carried a split frame header, which
 * presented as `0 frames, 0 bytes` from a daemon doing everything
 * right. A third test starting from either copy would inherit
 * whichever gaps that copy still had.
 *
 * So: one implementation, and the next hand-rolled reader is a reuse
 * rather than a fourth copy.
 */

#include <stddef.h>
#include <sys/types.h>

/*
 * RFC 6455's own worked example. The daemon's accept value is derived
 * from this key, so a test that sends it can assert the exact string
 * back rather than re-deriving SHA-1 and base64 to check the answer it
 * already knows.
 */
#define TEST_WS_KEY "dGhlIHNhbXBsZSBub25jZQ=="
#define TEST_WS_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

/*
 * Why a read stopped. "The peer closed" and "nothing arrived in time"
 * are different bugs and were both reported as -1 before this existed,
 * which is what made a timing flake read as a daemon fault.
 */
enum ws_stop {
	WS_STOP_NONE = 0,
	WS_STOP_PEER_CLOSED,   /* read() returned 0 */
	WS_STOP_TIMEOUT,       /* EAGAIN/EWOULDBLOCK against SO_RCVTIMEO */
	WS_STOP_ERROR,         /* any other read() failure */
	WS_STOP_PROTOCOL,      /* a frame this reader will not parse */
	WS_STOP_OVERFLOW       /* a frame, or the handshake leftover, did not fit */
};

struct ws_reader {
	int fd;
	/*
	 * Holds the whole of a coalesced handshake read. Sized above the
	 * 2048-byte response buffers both callers use, and an over-length
	 * leftover is a hard FAILURE rather than a truncation -- see
	 * ws_reader_adopt_handshake().
	 */
	unsigned char pending[8192];
	size_t pending_len;
	size_t pending_pos;
	enum ws_stop stop;
	int last_errno;
};

/*
 * Adopt whatever the handshake read took past `\r\n\r\n` as the front
 * of the frame stream. `got` is a byte count and not strlen: these are
 * frame bytes and may contain NUL.
 *
 * Returns 0, or -1 when the leftover does not fit -- which is a real
 * failure and not a thing to truncate. Both previous copies of this
 * lost the excess instead, one silently, each relying on a comment
 * about buffer sizes elsewhere in its own file to argue it could not
 * happen. Losing any of it misaligns the stream exactly as dropping
 * all of it does, so the reader refuses rather than continuing into a
 * ghost failure.
 *
 * headers_end may be NULL (no complete header block was read), in
 * which case there is no leftover and the reader is simply empty.
 */
int ws_reader_adopt_handshake(struct ws_reader *r, int fd, const char *resp, size_t got,
                               const char *headers_end);

/*
 * Fill exactly `want` bytes, pending buffer first, then the socket.
 *
 * A frame header is two bytes and read() is entitled to hand back one.
 * That is not pedantry: treating a one-byte read as a dead connection
 * discarded frames that were perfectly good and only late, and it
 * showed up in a busy build container and never in a quiet hand-run.
 */
int ws_read_full(struct ws_reader *r, void *buf, size_t want);

/* One frame. Sets *out_opcode and *out_len on success. */
int ws_recv_frame(struct ws_reader *r, int *out_opcode, unsigned char *out_buf, size_t out_cap,
                   size_t *out_len);

/* A client frame: FIN set, masked as RFC 6455 requires of a client. */
int ws_send_frame(int fd, int opcode, const void *payload, size_t len);

/* Why the last read stopped, for a message a reader can act on. */
const char *ws_stop_str(const struct ws_reader *r);

#endif /* TEST_WS_H */
