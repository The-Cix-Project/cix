#include "console.h"
/* ADR-0218: both WebSocket paths come from the contract too -- an
 * upgrade is still a declared API operation, and a path built here
 * by hand would drift exactly like any other. */
#include "generated/cix_api.h"
#include "iohelpers.h"
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

	rc = cix_write_all(fd, header, hlen);
	if (rc == 0)
		rc = cix_write_all(fd, mask, sizeof(mask));
	if (rc == 0 && len > 0)
		rc = cix_write_all(fd, masked, len);
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
 * Percent-encode one query-parameter VALUE.
 *
 * Only the unreserved set (RFC 3986) survives unescaped; everything
 * else, '/' included, becomes %XX. Encoding more than strictly
 * necessary is deliberate -- the daemon decodes unconditionally, and a
 * client that has to reason about which bytes happen to be safe in a
 * query string is a client that will eventually get it wrong.
 *
 * Returns -1 rather than truncating: a half-encoded command is a
 * different command.
 */
static int url_encode_component(const char *in, char *out, size_t out_size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t w = 0;
	size_t i;

	for (i = 0; in[i] != '\0'; i++) {
		unsigned char ch = (unsigned char)in[i];

		if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
			if (w + 1 >= out_size)
				return -1;
			out[w++] = (char)ch;
		} else {
			if (w + 3 >= out_size)
				return -1;
			out[w++] = '%';
			out[w++] = hex[ch >> 4];
			out[w++] = hex[ch & 0x0f];
		}
	}
	out[w] = '\0';
	return 0;
}

/*
 * Shared WS handshake for both cix_console_run() and
 * cix_pkg_build_log_run() -- label identifies which one for error
 * messages, path is the exact request-line target to send.
 *
 * Nothing about the request varies between them any more. The console
 * used to pass its command as an X-Cix-Exec-Cmd header, which is the
 * one thing the dashboard could never send (a browser's WebSocket
 * constructor sets no request headers), so the command is a query
 * parameter now and lives in `path` like every other one.
 */
static int do_ws_handshake(const struct cix_client *c, const char *label, const char *path)
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

	fd = cix_client_connect_raw(c);
	if (fd < 0) {
		fprintf(stderr, "%s: could not connect to %s:%d\n", label, c->host, c->port);
		return -1;
	}

	rlen = snprintf(req, sizeof(req),
	                 "GET %s HTTP/1.1\r\n"
	                 "Host: %s:%d\r\n"
	                 "Upgrade: websocket\r\n"
	                 "Connection: Upgrade\r\n"
	                 "Sec-WebSocket-Key: %s\r\n"
	                 "Sec-WebSocket-Version: 13\r\n"
	                 "\r\n",
	                 path, c->host, c->port, key);
	if (rlen < 0 || (size_t)rlen >= sizeof(req) || cix_write_all(fd, req, (size_t)rlen) != 0) {
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

/*
 * The size of the terminal this cixctl is itself running on, which is
 * the size the container's pty should be given. Tries each standard
 * stream in turn rather than assuming stdin: a console driven with
 * piped stdin (`echo cmd | cixctl console ...`) still usually has a
 * real terminal on stdout, and its size is still the right answer.
 *
 * Returns 0 when there is no terminal here at all, in which case the
 * caller sends no size and the daemon applies its own default -- the
 * honest outcome, since a pipe genuinely has no dimensions.
 */
static int local_winsize(unsigned short *cols, unsigned short *rows)
{
	static const int fds[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
	struct winsize wsz;
	size_t i;

	for (i = 0; i < 3; i++) {
		if (!isatty(fds[i]))
			continue;
		if (ioctl(fds[i], TIOCGWINSZ, &wsz) != 0)
			continue;
		if (wsz.ws_col == 0 || wsz.ws_row == 0)
			continue;
		*cols = wsz.ws_col;
		*rows = wsz.ws_row;
		return 1;
	}
	return 0;
}

/*
 * SIGWINCH tells us the local terminal was resized. The handler does
 * the only thing a signal handler safely can -- set a flag -- and the
 * relay loop below does the actual work.
 *
 * Deliberately installed WITHOUT SA_RESTART: the point is for the
 * signal to interrupt the loop's blocking poll() so the resize is
 * noticed immediately rather than whenever the next keystroke or byte
 * of output happens to arrive. relay() already treats EINTR as an
 * ordinary continue (it has since it was written), so the interruption
 * needs no new handling of its own.
 */
static volatile sig_atomic_t g_winch_pending;

static void on_sigwinch(int sig)
{
	(void)sig;
	g_winch_pending = 1;
}

/* Sends the current local size as a control message. The daemon reads
 * these from TEXT frames; keystrokes stay binary (see its own
 * handle_console_ws_event()). Failure is not fatal -- a resize that
 * does not arrive leaves the remote terminal at its previous size,
 * which is a cosmetic loss, and the write error will surface on the
 * next real I/O if the connection is genuinely gone. */
static void send_resize(int ws_fd)
{
	unsigned short cols, rows;
	char msg[64];
	int len;

	if (!local_winsize(&cols, &rows))
		return;
	len = snprintf(msg, sizeof(msg), "{\"type\":\"resize\",\"cols\":%u,\"rows\":%u}",
	               (unsigned)cols, (unsigned)rows);
	if (len < 0 || (size_t)len >= sizeof(msg))
		return;
	send_masked_frame(ws_fd, 0x1, msg, (size_t)len);
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

		if (g_winch_pending) {
			/* Cleared before sending, not after: if another resize
			 * lands while this one is in flight, the flag it sets
			 * must survive to trigger a further send rather than
			 * being wiped by this iteration. */
			g_winch_pending = 0;
			send_resize(ws_fd);
		}

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
					if (cix_write_all(STDOUT_FILENO, payload, payload_len) != 0)
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

int cix_console_run(const struct cix_client *c, const char *container_name,
                    const char *console_name)
{
	int fd;
	struct termios saved;
	int have_saved;
	struct sigaction sa, saved_winch;
	int have_saved_winch;

	{
		char path[512];
		size_t used;
		const char *sep = "?";
		unsigned short cols, rows;
		const char *term = getenv("TERM");

		snprintf(path, sizeof(path), CIX_API_consoleContainer, container_name);
		used = strlen(path);

		/* #248: select one of the container's declared consoles. Left
		 * off entirely when unset, so the daemon applies its own
		 * "first declared" rule rather than this client guessing. */
		if (console_name != NULL && console_name[0] != '\0') {
			used += (size_t)snprintf(path + used, sizeof(path) - used, "%sconsole=%s", sep,
			                          console_name);
			sep = "&";
		}

		/*
		 * What this terminal is and how big it is, so a full-screen
		 * program in the container (htop, vim) has a real screen to
		 * draw on. Each is sent only when actually known: no $TERM
		 * set, or no terminal at all because stdio is piped, means
		 * saying nothing and letting the daemon apply its documented
		 * default, rather than inventing a value on the server's
		 * behalf that would be indistinguishable from a measured one.
		 */
		if (term != NULL && term[0] != '\0') {
			used += (size_t)snprintf(path + used, sizeof(path) - used, "%sterm=%s", sep, term);
			sep = "&";
		}
		if (local_winsize(&cols, &rows)) {
			used += (size_t)snprintf(path + used, sizeof(path) - used, "%scols=%u&rows=%u", sep,
			                          (unsigned)cols, (unsigned)rows);
			sep = "&";
		}

		(void)sep;

		fd = do_ws_handshake(c, "console", path);
	}
	if (fd < 0)
		return -1;

	have_saved = set_raw_mode(STDIN_FILENO, &saved) == 0;

	/* No SA_RESTART -- see on_sigwinch()'s own note: interrupting
	 * poll() is exactly what makes a resize take effect promptly. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigwinch;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	have_saved_winch = sigaction(SIGWINCH, &sa, &saved_winch) == 0;

	relay(fd);

	if (have_saved_winch)
		sigaction(SIGWINCH, &saved_winch, NULL);
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
 * arrives, same server-frame parser cix_console_run()'s own relay()
 * already uses.
 */
int cix_pkg_build_log_run(const struct cix_client *c, const char *name, const char *image)
{
	int fd;
	struct client_ws_buf inbuf;
	unsigned char iobuf[4096];
	char path[512];

	/*
	 * The daemon takes ?name= (required if given) and an optional
	 * ?image=, and falls back to the single in-progress build when
	 * neither is supplied. Sending no query at all in that case keeps
	 * the request byte-identical to what every earlier client sent.
	 */
	if (name != NULL && name[0] != '\0') {
		if (image != NULL && image[0] != '\0')
			snprintf(path, sizeof(path), "%s?name=%s&image=%s",
			         CIX_API_pkgBuildLog, name, image);
		else
			snprintf(path, sizeof(path), "%s?name=%s", CIX_API_pkgBuildLog, name);
	} else {
		snprintf(path, sizeof(path), "%s", CIX_API_pkgBuildLog);
	}

	fd = do_ws_handshake(c, "pkg build-log", path);
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
				if (cix_write_all(STDOUT_FILENO, payload, payload_len) != 0)
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
