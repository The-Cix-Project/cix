#include "tlsconn.h"
#include "iohelpers.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct tls_entry {
	int fd; /* -1: free slot */
	SSL *ssl;
};

static struct tls_entry g_tls_conns[TLS_CONN_MAX];

void tls_init(void)
{
	int i;

	for (i = 0; i < TLS_CONN_MAX; i++) {
		g_tls_conns[i].fd = -1;
		g_tls_conns[i].ssl = NULL;
	}
}

void tls_register(int fd, SSL *ssl)
{
	int i;

	for (i = 0; i < TLS_CONN_MAX; i++) {
		if (g_tls_conns[i].fd == -1) {
			g_tls_conns[i].fd = fd;
			g_tls_conns[i].ssl = ssl;
			return;
		}
	}
	/* Table full -- this daemon has no way to signal that back to the
	 * caller here (accept_loop() already committed to this fd). Not
	 * expected in practice (TLS_CONN_MAX comfortably exceeds any
	 * realistic concurrent-HTTPS-connection count for a single-host
	 * orchestration daemon); if it ever happens, the connection simply
	 * behaves as if plain (tls_lookup() finds nothing), which fails
	 * safely -- a broken handshake, not a silent plaintext downgrade
	 * of already-negotiated TLS. */
}

void tls_unregister(int fd)
{
	int i;

	for (i = 0; i < TLS_CONN_MAX; i++) {
		if (g_tls_conns[i].fd == fd) {
			g_tls_conns[i].fd = -1;
			g_tls_conns[i].ssl = NULL;
			return;
		}
	}
}

SSL *tls_lookup(int fd)
{
	int i;

	for (i = 0; i < TLS_CONN_MAX; i++) {
		if (g_tls_conns[i].fd == fd)
			return g_tls_conns[i].ssl;
	}
	return NULL;
}

int tls_write_all(int fd, const void *buf, size_t n)
{
	SSL *ssl = tls_lookup(fd);
	const unsigned char *p = buf;
	size_t written = 0;

	if (ssl == NULL)
		return cix_write_all(fd, buf, n);

	while (written < n) {
		int wrote = SSL_write(ssl, p + written, (int)(n - written));

		if (wrote > 0) {
			written += (size_t)wrote;
			continue;
		}
		/* fd is always blocking-mode by the time this is called
		 * (http_set_blocking(), same convention the plain-fd path
		 * already relies on) -- WANT_READ/WANT_WRITE here would mean
		 * OpenSSL itself decided a retry was needed (e.g. a mid-stream
		 * renegotiation), not "no data yet"; looping is correct and
		 * won't busy-spin since the underlying I/O still blocks. */
		{
			int err = SSL_get_error(ssl, wrote);

			if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
				continue;
		}
		return -1;
	}
	return 0;
}

ssize_t tls_read(int fd, void *buf, size_t n)
{
	SSL *ssl = tls_lookup(fd);
	int r;
	int err;

	if (ssl == NULL)
		return read(fd, buf, n);

	r = SSL_read(ssl, buf, (int)n);
	if (r > 0)
		return r;

	err = SSL_get_error(ssl, r);
	if (err == SSL_ERROR_ZERO_RETURN)
		return 0; /* clean TLS-level close, same shape as read()'s EOF */
	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		errno = EAGAIN;
		return -1;
	}
	return -1;
}
