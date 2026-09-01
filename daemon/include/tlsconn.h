#ifndef TLSCONN_H
#define TLSCONN_H

#include <openssl/ssl.h>

#include <sys/types.h>

/*
 * A tiny fd -> SSL* side table (Part 0.5's HTTPS listener). Exists
 * because http.c's http_write_response() and websocket.c's frame
 * sender only ever receive a raw fd, never main.c's own struct conn*
 * -- threading that type through every one of this daemon's ~100
 * request handlers just to make two low-level writers TLS-aware would
 * be a far larger, riskier change than this small, additive lookup.
 * main.c registers/unregisters an fd's SSL* here exactly when it
 * creates/tears down a TLS-wrapped connection; every other fd (the
 * overwhelming majority -- every plain HTTP connection) is simply
 * absent, and a lookup miss costs one small linear scan.
 */
#define TLS_CONN_MAX 64

/*
 * Marks every slot free (fd = -1). MUST be called once before any
 * register/unregister/lookup -- the table is a plain static array,
 * zero-initialized by the C runtime like any other, and 0 is a real
 * fd number, not a safe "empty" sentinel; skipping this leaves every
 * slot looking permanently "in use" (fd == 0) and tls_register()
 * silently registers nothing at all.
 */
void tls_init(void);

void tls_register(int fd, SSL *ssl);
void tls_unregister(int fd);
SSL *tls_lookup(int fd);

/*
 * Writes n bytes to fd, transparently using SSL_write() in a loop
 * (retrying on WANT_READ/WANT_WRITE -- fd is always blocking-mode by
 * the time these are called, same as the existing http_set_blocking()
 * convention this daemon already applies before every plain-fd
 * response write) when fd is currently TLS-wrapped, or cix_write_all()
 * otherwise. The one place either case is decided, reused by
 * http_write_response() and the WebSocket frame sender rather than
 * each reimplementing the same branch. Returns 0 on success, -1 on
 * any failure -- same contract as cix_write_all().
 */
int tls_write_all(int fd, const void *buf, size_t n);

/*
 * Writes as much of n as the socket will take right now and returns
 * immediately -- the non-blocking counterpart of tls_write_all(),
 * added for issue #237 so a response can be drained across several
 * EPOLLOUT events instead of blocking the whole event loop until the
 * peer has read it.
 *
 * Returns the number of bytes actually written (> 0), 0 if the write
 * would block and should be retried when the socket is writable again,
 * or -1 on a genuine error.
 *
 * The TLS case matters here: OpenSSL requires that a WANT_WRITE retry
 * repeat the *same* call arguments, so a caller must retry with the
 * identical (buf, n) pair it was given back a 0 for -- passing a
 * deterministic slice of a stable buffer, as conn_out_drain() does,
 * satisfies that.
 */
ssize_t tls_write_some(int fd, const void *buf, size_t n);

/*
 * Reads up to n bytes from fd the same way -- SSL_read() when TLS-
 * wrapped, plain read() otherwise. Returns exactly what read()/
 * SSL_read() would (bytes read, 0 on clean EOF/close, -1 on a real
 * error) so existing call sites need no other change.
 */
ssize_t tls_read(int fd, void *buf, size_t n);

#endif /* TLSCONN_H */
