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
 * response write) when fd is currently TLS-wrapped, or kx_write_all()
 * otherwise. The one place either case is decided, reused by
 * http_write_response() and the WebSocket frame sender rather than
 * each reimplementing the same branch. Returns 0 on success, -1 on
 * any failure -- same contract as kx_write_all().
 */
int tls_write_all(int fd, const void *buf, size_t n);

/*
 * Reads up to n bytes from fd the same way -- SSL_read() when TLS-
 * wrapped, plain read() otherwise. Returns exactly what read()/
 * SSL_read() would (bytes read, 0 on clean EOF/close, -1 on a real
 * error) so existing call sites need no other change.
 */
ssize_t tls_read(int fd, void *buf, size_t n);

#endif /* TLSCONN_H */
