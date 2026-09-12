#include "ldapclient.h"
#include "logstore.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

/*
 * A minimal hand-rolled BER (ITU-T X.690) encoder/decoder plus just
 * enough of LDAPv3 (RFC 4511) to bind and search -- see ldapclient.h
 * for the scope this is deliberately limited to. All tags used here
 * fit in a single tag octet (universal/application/context-specific,
 * tag number < 31) and every length this module ever writes fits
 * comfortably in the short or a one-octet long form; the reader
 * accepts long-form lengths up to sizeof(size_t) octets to tolerate
 * whatever a real server sends back (a diagnosticMessage can be long).
 */

#define BER_TAG_SEQUENCE    0x30
#define BER_TAG_INTEGER     0x02
#define BER_TAG_OCTETSTRING 0x04
#define BER_TAG_ENUMERATED  0x0A
#define BER_TAG_BOOLEAN     0x01

#define LDAP_TAG_BIND_REQUEST      0x60 /* [APPLICATION 0], constructed */
#define LDAP_TAG_BIND_RESPONSE     0x61 /* [APPLICATION 1], constructed */
#define LDAP_TAG_UNBIND_REQUEST    0x42 /* [APPLICATION 2], primitive, NULL content */
#define LDAP_TAG_SEARCH_REQUEST    0x63 /* [APPLICATION 3], constructed */
#define LDAP_TAG_SEARCH_RESULT_ENTRY 0x64 /* [APPLICATION 4], constructed */
#define LDAP_TAG_SEARCH_RESULT_DONE  0x65 /* [APPLICATION 5], constructed */

#define LDAP_TAG_AUTH_SIMPLE   0x80 /* [0], primitive -- BindRequest simple auth choice */
#define LDAP_TAG_FILTER_AND    0xA0 /* [0], constructed -- SET OF Filter */
#define LDAP_TAG_FILTER_EQUALITY 0xA3 /* [3], constructed -- AttributeValueAssertion */

#define BER_BUF_MAX 2048  /* one encoded request element -- plenty for any DN/filter this client builds */
#define LDAP_MSG_MAX 8192 /* one full wire LDAPMessage, send or receive */

struct ber_buf {
	unsigned char data[BER_BUF_MAX];
	size_t len;
};

struct ber_tlv {
	unsigned char tag;
	const unsigned char *content;
	size_t content_len;
	size_t total_len; /* tag + length octets + content_len, i.e. bytes consumed from the source */
};

static int ber_buf_append(struct ber_buf *b, const void *data, size_t len)
{
	if (len > 0 && b->len + len > sizeof(b->data))
		return -1;
	if (len > 0)
		memcpy(b->data + b->len, data, len);
	b->len += len;
	return 0;
}

static int ber_write_length(struct ber_buf *b, size_t len)
{
	unsigned char rev[sizeof(size_t)];
	unsigned char first;
	int n = 0;
	size_t tmp = len;

	if (len < 128) {
		unsigned char c = (unsigned char)len;
		return ber_buf_append(b, &c, 1);
	}

	while (tmp > 0) {
		rev[n++] = (unsigned char)(tmp & 0xFF);
		tmp >>= 8;
	}
	first = (unsigned char)(0x80 | n);
	if (ber_buf_append(b, &first, 1) != 0)
		return -1;
	while (n > 0) {
		n--;
		if (ber_buf_append(b, &rev[n], 1) != 0)
			return -1;
	}
	return 0;
}

static int ber_write_tlv(struct ber_buf *b, unsigned char tag, const void *content, size_t content_len)
{
	if (ber_buf_append(b, &tag, 1) != 0)
		return -1;
	if (ber_write_length(b, content_len) != 0)
		return -1;
	return ber_buf_append(b, content, content_len);
}

/* wraps the fully-encoded contents of `inner` as the content of a new TLV under `tag` */
static int ber_wrap(struct ber_buf *out, unsigned char tag, const struct ber_buf *inner)
{
	return ber_write_tlv(out, tag, inner->data, inner->len);
}

/* minimal-length big-endian two's-complement encoding -- every value this
 * client ever writes (message IDs, protocol version, scope/deref
 * enumerations, size/time limits) is small and non-negative */
static size_t ber_encode_integer(long value, unsigned char *out)
{
	unsigned char tmp[sizeof(long)];
	unsigned long uv = (unsigned long)value;
	int n = 0;
	int i;
	size_t outlen = 0;

	if (uv == 0) {
		out[0] = 0;
		return 1;
	}
	while (uv > 0 && n < (int)sizeof(tmp)) {
		tmp[n++] = (unsigned char)(uv & 0xFF);
		uv >>= 8;
	}
	if (tmp[n - 1] & 0x80)
		out[outlen++] = 0x00;
	for (i = n - 1; i >= 0; i--)
		out[outlen++] = tmp[i];
	return outlen;
}

static long ber_parse_integer(const unsigned char *content, size_t len)
{
	long v = 0;
	size_t i;

	for (i = 0; i < len; i++)
		v = (v << 8) | content[i];
	return v;
}

static int ber_parse_tlv(const unsigned char *buf, size_t buflen, struct ber_tlv *out)
{
	unsigned char lenbyte;
	size_t pos = 2;
	size_t content_len;

	if (buflen < 2)
		return -1;
	lenbyte = buf[1];
	if (lenbyte < 0x80) {
		content_len = lenbyte;
	} else {
		int nbytes = lenbyte & 0x7F;
		int i;

		if (nbytes == 0 || (size_t)nbytes > sizeof(size_t) || (size_t)nbytes > buflen - 2)
			return -1;
		content_len = 0;
		for (i = 0; i < nbytes; i++)
			content_len = (content_len << 8) | buf[2 + i];
		pos = 2 + (size_t)nbytes;
	}
	if (pos + content_len > buflen)
		return -1;
	out->tag = buf[0];
	out->content = buf + pos;
	out->content_len = content_len;
	out->total_len = pos + content_len;
	return 0;
}

/* ---- TCP connect with a real connect-phase timeout (nonblocking connect
 * + poll), then back to blocking with SO_RCVTIMEO/SO_SNDTIMEO for the
 * rest of the exchange ---- */

/*
 * One LDAP connection, plaintext or LDAPS. Everything below that used
 * to take a bare `int fd` takes this instead, so TLS is a property of
 * the connection rather than a side table keyed by descriptor -- this
 * module owns its own, deliberately NOT tlsconn.c's map, which is the
 * HTTPS *server's*, capped at TLS_CONN_MAX and shaped for the epoll
 * loop. ssl/ctx are NULL on a plaintext connection and every helper
 * branches on ssl, so the plaintext path is unchanged code.
 */
struct ldap_conn {
	int fd;
	SSL *ssl;
	SSL_CTX *ctx;
};

/*
 * Writes why a TLS step failed to the log store, drained so a later
 * failure cannot inherit this one's text. stderr is not enough: cixd
 * never mirrors it into the log store, and a TLS failure here is a
 * connect-class error that falls back to local authentication -- an
 * unexplained fallback is indistinguishable from a wrong password.
 *
 * ssl/rc are the failing SSL_connect()'s own, or NULL/0 for a step
 * that has no SSL yet. They matter because an EMPTY error queue is a
 * real and common outcome, not a rarity: measured on 192.168.15.95,
 * 2026-09-12, pointing ldap_tls at glauth's PLAINTEXT port logged
 * "no OpenSSL error queued" twice and named nothing an operator could
 * act on. A plaintext peer answers a ClientHello with LDAP bytes (or
 * a close), which surfaces as SSL_ERROR_SYSCALL with the real cause
 * in errno and nothing in the queue at all. So the queue is reported
 * when it has something, and SSL_get_error()+errno when it does not.
 */
static void ldap_tls_log(const char *what, const char *host, int port, SSL *ssl, int rc)
{
	unsigned long e = ERR_get_error();
	char msg[256];

	if (e != 0) {
		ERR_error_string_n(e, msg, sizeof(msg));
		ERR_clear_error();
		logstore_write("ldap", "error", "%s to %s:%d failed: %s", what, host, port, msg);
		return;
	}
	if (ssl != NULL) {
		int se = SSL_get_error(ssl, rc);
		const char *name;

		switch (se) {
		case SSL_ERROR_SYSCALL:
			/* The usual shape of "that port is not speaking TLS":
			 * rc == 0 is a clean EOF from a peer that answered the
			 * ClientHello with something else and hung up. */
			logstore_write("ldap", "error",
			                "%s to %s:%d failed: %s -- is that port serving TLS?", what,
			                host, port,
			                rc == 0 ? "connection closed during the handshake"
			                         : strerror(errno));
			return;
		case SSL_ERROR_ZERO_RETURN: name = "peer closed the TLS session"; break;
		case SSL_ERROR_WANT_READ:
		case SSL_ERROR_WANT_WRITE: name = "timed out mid-handshake"; break;
		default: name = "unknown TLS error"; break;
		}
		logstore_write("ldap", "error", "%s to %s:%d failed: %s (SSL_get_error=%d)", what,
		                host, port, name, se);
		return;
	}
	logstore_write("ldap", "error", "%s to %s:%d failed, with no error reported", what, host,
	                port);
}

/* Adds every certificate in a PEM bundle to ctx's trust store. The
 * bundle is root, or root+intermediate (pki_trust_bundle_pem()), so
 * the loop matters -- reading one certificate and stopping would
 * silently drop the intermediate on an install that has one, and the
 * chain would then fail to verify against a correct server. Returns
 * the number added, or -1. */
static int ldap_tls_add_anchors(SSL_CTX *ctx, const char *ca_pem, size_t ca_pem_len)
{
	BIO *bio;
	X509_STORE *store = SSL_CTX_get_cert_store(ctx);
	int added = 0;

	if (store == NULL || ca_pem == NULL || ca_pem_len == 0 || ca_pem_len > INT_MAX)
		return -1;
	bio = BIO_new_mem_buf(ca_pem, (int)ca_pem_len);
	if (bio == NULL)
		return -1;
	for (;;) {
		X509 *x = PEM_read_bio_X509(bio, NULL, NULL, NULL);

		if (x == NULL)
			break;
		if (X509_STORE_add_cert(store, x) == 1)
			added++;
		X509_free(x);
	}
	BIO_free(bio);
	/* PEM_read_bio_X509 ends by queueing a "no start line" error when it
	 * runs out of input. That is the normal terminating condition, not a
	 * failure, and leaving it queued would make the next real error
	 * report it instead. */
	ERR_clear_error();
	return added > 0 ? added : -1;
}

/*
 * Starts TLS on an already-connected socket. Verification is pinned to
 * the string the operator CONFIGURED, not to whichever getaddrinfo
 * result answered -- and by the matching SAN type, because a TLS client
 * checks the name it dialled: an address dialled as an address is never
 * matched against a DNS: SAN, so X509_VERIFY_PARAM_set1_host() fails
 * against a correct certificate when the server was reached by IP.
 * hostauth's ldap_servers entries are documented as "a host or IP", so
 * both are real cases and the kind is decided by inet_pton rather than
 * assumed (#414 decides the SAN kind the same way when issuing).
 */
static int ldap_tls_start(struct ldap_conn *c, const char *host, int port, const char *ca_pem,
                           size_t ca_pem_len)
{
	struct in_addr a4;
	struct in6_addr a6;
	int is_ip = inet_pton(AF_INET, host, &a4) == 1 || inet_pton(AF_INET6, host, &a6) == 1;
	X509_VERIFY_PARAM *vp;

	c->ctx = SSL_CTX_new(TLS_client_method());
	if (c->ctx == NULL) {
		ldap_tls_log("TLS context setup", host, port, NULL, 0);
		return -1;
	}
	SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
	/* Without this, verification is advisory: the handshake completes
	 * and SSL_get_verify_result() has to be consulted separately. */
	SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
	if (ldap_tls_add_anchors(c->ctx, ca_pem, ca_pem_len) < 0) {
		logstore_write("ldap", "error",
		                "TLS to %s:%d: no usable certificate in the CA trust bundle", host,
		                port);
		return -1;
	}

	c->ssl = SSL_new(c->ctx);
	if (c->ssl == NULL || SSL_set_fd(c->ssl, c->fd) != 1) {
		ldap_tls_log("TLS setup", host, port, NULL, 0);
		return -1;
	}
	vp = SSL_get0_param(c->ssl);
	if (vp == NULL) {
		logstore_write("ldap", "error", "TLS to %s:%d: no verify parameters", host, port);
		return -1;
	}
	if (is_ip) {
		if (X509_VERIFY_PARAM_set1_ip_asc(vp, host) != 1) {
			ldap_tls_log("TLS address verification setup", host, port, NULL, 0);
			return -1;
		}
	} else {
		X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
		if (X509_VERIFY_PARAM_set1_host(vp, host, 0) != 1) {
			ldap_tls_log("TLS hostname verification setup", host, port, NULL, 0);
			return -1;
		}
		/* SNI only for a real name -- RFC 6066 forbids a literal
		 * address in server_name, and glauth would reject it. */
		SSL_set_tlsext_host_name(c->ssl, host);
	}
	{
		int rc = SSL_connect(c->ssl);

		if (rc != 1) {
			ldap_tls_log("TLS handshake", host, port, c->ssl, rc);
			return -1;
		}
	}
	return 0;
}

static void ldap_conn_close(struct ldap_conn *c)
{
	if (c->ssl != NULL) {
		/* One-way shutdown: the close() below ends the connection
		 * regardless, and waiting for the peer's close_notify would
		 * block for up to the socket timeout on every single login. */
		SSL_shutdown(c->ssl);
		SSL_free(c->ssl);
		c->ssl = NULL;
	}
	if (c->ctx != NULL) {
		SSL_CTX_free(c->ctx);
		c->ctx = NULL;
	}
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

static int ldapclient_connect(const char *host, int port, int timeout_ms)
{
	char portstr[16];
	struct addrinfo hints, *res, *rp;
	int fd = -1;
	struct timeval tv;

	snprintf(portstr, sizeof(portstr), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, portstr, &hints, &res) != 0)
		return -1;

	for (rp = res; rp != NULL; rp = rp->ai_next) {
		int flags;
		int rc;

		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;

		flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
		if (rc == 0)
			break; /* connected immediately (e.g. loopback) */
		if (errno != EINPROGRESS) {
			close(fd);
			fd = -1;
			continue;
		}

		{
			struct pollfd pfd;
			int pr;
			int so_error = 0;
			socklen_t so_len = sizeof(so_error);

			pfd.fd = fd;
			pfd.events = POLLOUT;
			pr = poll(&pfd, 1, timeout_ms);
			if (pr <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0) {
				close(fd);
				fd = -1;
				continue;
			}
		}
		break; /* connected */
	}
	freeaddrinfo(res);
	if (fd < 0)
		return -1;

	{
		int flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	}

	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	return fd;
}

/*
 * Opens a connection, plaintext or LDAPS. ca_pem non-NULL selects TLS;
 * it is the caller's trust anchor (hostauth passes the live chain from
 * pki_trust_bundle_pem(), read at dial time rather than staged to a
 * file that a CA reset or ADR-0281 import would leave stale). On
 * failure the connection is fully closed and -1 returned, so no caller
 * has to unwind a half-open one.
 */
static int ldap_conn_open(struct ldap_conn *c, const char *host, int port, int timeout_ms,
                           const char *ca_pem, size_t ca_pem_len)
{
	c->fd = -1;
	c->ssl = NULL;
	c->ctx = NULL;
	c->fd = ldapclient_connect(host, port, timeout_ms);
	if (c->fd < 0)
		return -1;
	if (ca_pem != NULL && ldap_tls_start(c, host, port, ca_pem, ca_pem_len) != 0) {
		ldap_conn_close(c);
		return -1;
	}
	return 0;
}

static int send_all(struct ldap_conn *c, const unsigned char *buf, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		ssize_t n;

		if (c->ssl != NULL) {
			/* SSL_write is all-or-nothing for the length given (no
			 * SSL_MODE_ENABLE_PARTIAL_WRITE set), so a short count is
			 * an error rather than a resumption point. */
			int w = SSL_write(c->ssl, buf + sent, (int)(len - sent));

			if (w <= 0)
				return -1;
			n = w;
		} else {
			n = send(c->fd, buf + sent, len - sent, MSG_NOSIGNAL);
			if (n <= 0) {
				if (n < 0 && errno == EINTR)
					continue;
				return -1;
			}
		}
		sent += (size_t)n;
	}
	return 0;
}

static int recv_full(struct ldap_conn *c, unsigned char *buf, size_t need)
{
	size_t got = 0;

	while (got < need) {
		ssize_t n;

		if (c->ssl != NULL) {
			int r = SSL_read(c->ssl, buf + got, (int)(need - got));

			if (r <= 0)
				return -1;
			n = r;
		} else {
			n = recv(c->fd, buf + got, need - got, 0);
			if (n <= 0) {
				if (n < 0 && errno == EINTR)
					continue;
				return -1;
			}
		}
		got += (size_t)n;
	}
	return 0;
}

/* reads exactly one full LDAPMessage TLV off the wire (tag + definite-form
 * length + content), framing purely from the BER length -- LDAP over TCP
 * has no other message framing */
static int ber_read_message(struct ldap_conn *c, unsigned char *buf, size_t bufcap, size_t *out_len)
{
	unsigned char lenbyte;
	size_t pos = 2;
	size_t content_len;

	if (recv_full(c, buf, 2) != 0)
		return -1;
	lenbyte = buf[1];
	if (lenbyte < 0x80) {
		content_len = lenbyte;
	} else {
		int nbytes = lenbyte & 0x7F;

		if (nbytes == 0 || (size_t)nbytes > sizeof(size_t) || 2 + (size_t)nbytes > bufcap)
			return -1;
		if (recv_full(c, buf + 2, (size_t)nbytes) != 0)
			return -1;
		content_len = 0;
		{
			int i;

			for (i = 0; i < nbytes; i++)
				content_len = (content_len << 8) | buf[2 + i];
		}
		pos = 2 + (size_t)nbytes;
	}
	if (pos + content_len > bufcap)
		return -1;
	if (content_len > 0 && recv_full(c, buf + pos, content_len) != 0)
		return -1;
	*out_len = pos + content_len;
	return 0;
}

/* ---- message building ---- */

static int wrap_ldap_message(long message_id, const struct ber_buf *op, unsigned char *out, size_t outcap, size_t *outlen)
{
	struct ber_buf msgcontent, message;
	unsigned char idbytes[sizeof(long) + 1];
	size_t idlen;

	memset(&msgcontent, 0, sizeof(msgcontent));
	idlen = ber_encode_integer(message_id, idbytes);
	if (ber_write_tlv(&msgcontent, BER_TAG_INTEGER, idbytes, idlen) != 0)
		return -1;
	if (ber_buf_append(&msgcontent, op->data, op->len) != 0)
		return -1;

	memset(&message, 0, sizeof(message));
	if (ber_wrap(&message, BER_TAG_SEQUENCE, &msgcontent) != 0)
		return -1;

	if (message.len > outcap)
		return -1;
	memcpy(out, message.data, message.len);
	*outlen = message.len;
	return 0;
}

static int build_bind_request(long message_id, const char *dn, const char *password, unsigned char *out, size_t outcap, size_t *outlen)
{
	struct ber_buf content, op;
	unsigned char verbytes[sizeof(long) + 1];
	size_t verlen;

	memset(&content, 0, sizeof(content));
	verlen = ber_encode_integer(3, verbytes);
	if (ber_write_tlv(&content, BER_TAG_INTEGER, verbytes, verlen) != 0)
		return -1;
	if (ber_write_tlv(&content, BER_TAG_OCTETSTRING, dn, strlen(dn)) != 0)
		return -1;
	if (ber_write_tlv(&content, LDAP_TAG_AUTH_SIMPLE, password, strlen(password)) != 0)
		return -1;

	memset(&op, 0, sizeof(op));
	if (ber_wrap(&op, LDAP_TAG_BIND_REQUEST, &content) != 0)
		return -1;

	return wrap_ldap_message(message_id, &op, out, outcap, outlen);
}

static void send_unbind(struct ldap_conn *c, long message_id)
{
	struct ber_buf msgcontent, message;
	unsigned char idbytes[sizeof(long) + 1];
	size_t idlen;
	unsigned char unbind_tlv[2] = { LDAP_TAG_UNBIND_REQUEST, 0x00 };

	memset(&msgcontent, 0, sizeof(msgcontent));
	idlen = ber_encode_integer(message_id, idbytes);
	if (ber_write_tlv(&msgcontent, BER_TAG_INTEGER, idbytes, idlen) != 0)
		return;
	if (ber_buf_append(&msgcontent, unbind_tlv, sizeof(unbind_tlv)) != 0)
		return;

	memset(&message, 0, sizeof(message));
	if (ber_wrap(&message, BER_TAG_SEQUENCE, &msgcontent) != 0)
		return;

	/* best-effort per RFC 4511 4.3 -- the immediately-following close() is
	 * what actually ends the session either way */
	send_all(c, message.data, message.len);
}

static int build_equality_filter(struct ber_buf *out, const char *attr, const char *value)
{
	struct ber_buf content;

	memset(&content, 0, sizeof(content));
	if (ber_write_tlv(&content, BER_TAG_OCTETSTRING, attr, strlen(attr)) != 0)
		return -1;
	if (ber_write_tlv(&content, BER_TAG_OCTETSTRING, value, strlen(value)) != 0)
		return -1;
	return ber_wrap(out, LDAP_TAG_FILTER_EQUALITY, &content);
}

static int build_filter(struct ber_buf *out, const char *const attrs[][2], int attr_count)
{
	struct ber_buf andcontent;
	int i;

	if (attr_count == 1)
		return build_equality_filter(out, attrs[0][0], attrs[0][1]);

	memset(&andcontent, 0, sizeof(andcontent));
	for (i = 0; i < attr_count; i++) {
		struct ber_buf one;

		memset(&one, 0, sizeof(one));
		if (build_equality_filter(&one, attrs[i][0], attrs[i][1]) != 0)
			return -1;
		if (ber_buf_append(&andcontent, one.data, one.len) != 0)
			return -1;
	}
	return ber_wrap(out, LDAP_TAG_FILTER_AND, &andcontent);
}

static int build_search_request(long message_id, const char *base_dn, const char *const attrs[][2], int attr_count,
                                 unsigned char *out, size_t outcap, size_t *outlen)
{
	struct ber_buf req, filter, attrsel, attrseq, op;
	unsigned char scopebyte = 2;   /* wholeSubtree */
	unsigned char derefbyte = 0;   /* neverDerefAliases */
	unsigned char zerobyte = 0;    /* sizeLimit / timeLimit: 0 = server-default bound */
	unsigned char falsebyte = 0x00; /* typesOnly = FALSE */

	memset(&req, 0, sizeof(req));
	if (ber_write_tlv(&req, BER_TAG_OCTETSTRING, base_dn, strlen(base_dn)) != 0)
		return -1;
	if (ber_write_tlv(&req, BER_TAG_ENUMERATED, &scopebyte, 1) != 0)
		return -1;
	if (ber_write_tlv(&req, BER_TAG_ENUMERATED, &derefbyte, 1) != 0)
		return -1;
	if (ber_write_tlv(&req, BER_TAG_INTEGER, &zerobyte, 1) != 0)
		return -1;
	if (ber_write_tlv(&req, BER_TAG_INTEGER, &zerobyte, 1) != 0)
		return -1;
	if (ber_write_tlv(&req, BER_TAG_BOOLEAN, &falsebyte, 1) != 0)
		return -1;

	memset(&filter, 0, sizeof(filter));
	if (build_filter(&filter, attrs, attr_count) != 0)
		return -1;
	if (ber_buf_append(&req, filter.data, filter.len) != 0)
		return -1;

	/* attributes: "1.1" (RFC 4511 4.5.1's reserved OID for "no attributes")
	 * -- this client only ever needs entry counts, never attribute values */
	memset(&attrsel, 0, sizeof(attrsel));
	if (ber_write_tlv(&attrsel, BER_TAG_OCTETSTRING, "1.1", 3) != 0)
		return -1;
	memset(&attrseq, 0, sizeof(attrseq));
	if (ber_wrap(&attrseq, BER_TAG_SEQUENCE, &attrsel) != 0)
		return -1;
	if (ber_buf_append(&req, attrseq.data, attrseq.len) != 0)
		return -1;

	memset(&op, 0, sizeof(op));
	if (ber_wrap(&op, LDAP_TAG_SEARCH_REQUEST, &req) != 0)
		return -1;

	return wrap_ldap_message(message_id, &op, out, outcap, outlen);
}

/* ---- response parsing ---- */

static int ber_parse_message_op(const unsigned char *buf, size_t len, struct ber_tlv *out_op)
{
	struct ber_tlv outer, msgid;
	const unsigned char *rest;
	size_t rest_len;

	if (ber_parse_tlv(buf, len, &outer) != 0 || outer.tag != BER_TAG_SEQUENCE)
		return -1;
	if (ber_parse_tlv(outer.content, outer.content_len, &msgid) != 0 || msgid.tag != BER_TAG_INTEGER)
		return -1;
	rest = outer.content + msgid.total_len;
	rest_len = outer.content_len - msgid.total_len;
	return ber_parse_tlv(rest, rest_len, out_op);
}

/* parses an LDAPResult-shaped protocolOp (BindResponse or SearchResultDone
 * -- both start with resultCode ENUMERATED, matchedDN, diagnosticMessage;
 * only resultCode is needed here) */
static enum ldapclient_error parse_ldap_result_message(const unsigned char *buf, size_t len, unsigned char expected_op_tag, int *out_result_code)
{
	struct ber_tlv op, resultcode;

	if (ber_parse_message_op(buf, len, &op) != 0)
		return LDAPCLIENT_ERR_PROTOCOL;
	if (op.tag != expected_op_tag)
		return LDAPCLIENT_ERR_PROTOCOL;
	if (ber_parse_tlv(op.content, op.content_len, &resultcode) != 0 || resultcode.tag != BER_TAG_ENUMERATED)
		return LDAPCLIENT_ERR_PROTOCOL;
	*out_result_code = (int)ber_parse_integer(resultcode.content, resultcode.content_len);
	return LDAPCLIENT_OK;
}

/* ---- public API ---- */

enum ldapclient_error ldapclient_bind(const char *host, int port, const char *dn,
                                       const char *password, int timeout_ms, const char *ca_pem,
                                       size_t ca_pem_len, int *out_ldap_result_code)
{
	struct ldap_conn c;
	unsigned char sendbuf[LDAP_MSG_MAX];
	unsigned char recvbuf[LDAP_MSG_MAX];
	size_t sendlen, recvlen;
	int result_code = -1;
	enum ldapclient_error err;

	if (out_ldap_result_code)
		*out_ldap_result_code = -1;

	if (ldap_conn_open(&c, host, port, timeout_ms, ca_pem, ca_pem_len) != 0)
		return LDAPCLIENT_ERR_CONNECT;

	if (build_bind_request(1, dn, password, sendbuf, sizeof(sendbuf), &sendlen) != 0) {
		ldap_conn_close(&c);
		return LDAPCLIENT_ERR_PROTOCOL;
	}
	if (send_all(&c, sendbuf, sendlen) != 0) {
		ldap_conn_close(&c);
		return LDAPCLIENT_ERR_CONNECT;
	}
	if (ber_read_message(&c, recvbuf, sizeof(recvbuf), &recvlen) != 0) {
		ldap_conn_close(&c);
		return LDAPCLIENT_ERR_CONNECT;
	}

	err = parse_ldap_result_message(recvbuf, recvlen, LDAP_TAG_BIND_RESPONSE, &result_code);

	send_unbind(&c, 2);
	ldap_conn_close(&c);

	if (err != LDAPCLIENT_OK)
		return err;
	if (out_ldap_result_code)
		*out_ldap_result_code = result_code;
	return (result_code == 0) ? LDAPCLIENT_OK : LDAPCLIENT_ERR_LDAP_RESULT;
}

enum ldapclient_error ldapclient_bind_and_search(const char *host, int port, const char *bind_dn, const char *bind_password,
                                                  const char *base_dn, const char *const attrs[][2], int attr_count,
                                                  int timeout_ms, const char *ca_pem, size_t ca_pem_len,
                                                  int *out_match_count, int *out_ldap_result_code)
{
	struct ldap_conn c;
	unsigned char sendbuf[LDAP_MSG_MAX];
	unsigned char recvbuf[LDAP_MSG_MAX];
	size_t sendlen, recvlen;
	int bind_result = -1;
	int result_code = -1;
	int match_count = 0;
	enum ldapclient_error err;

	if (out_match_count)
		*out_match_count = 0;
	if (out_ldap_result_code)
		*out_ldap_result_code = -1;

	if (attr_count < 1 || attr_count > 8)
		return LDAPCLIENT_ERR_PROTOCOL;

	if (ldap_conn_open(&c, host, port, timeout_ms, ca_pem, ca_pem_len) != 0)
		return LDAPCLIENT_ERR_CONNECT;

	if (build_bind_request(1, bind_dn, bind_password, sendbuf, sizeof(sendbuf), &sendlen) != 0) {
		ldap_conn_close(&c);
		return LDAPCLIENT_ERR_PROTOCOL;
	}
	if (send_all(&c, sendbuf, sendlen) != 0) {
		ldap_conn_close(&c);
		return LDAPCLIENT_ERR_CONNECT;
	}
	if (ber_read_message(&c, recvbuf, sizeof(recvbuf), &recvlen) != 0) {
		ldap_conn_close(&c);
		return LDAPCLIENT_ERR_CONNECT;
	}
	err = parse_ldap_result_message(recvbuf, recvlen, LDAP_TAG_BIND_RESPONSE, &bind_result);
	if (err != LDAPCLIENT_OK) {
		send_unbind(&c, 3);
		ldap_conn_close(&c);
		return err;
	}
	if (bind_result != 0) {
		if (out_ldap_result_code)
			*out_ldap_result_code = bind_result;
		send_unbind(&c, 3);
		ldap_conn_close(&c);
		return LDAPCLIENT_ERR_LDAP_RESULT;
	}

	if (build_search_request(2, base_dn, attrs, attr_count, sendbuf, sizeof(sendbuf), &sendlen) != 0) {
		send_unbind(&c, 3);
		ldap_conn_close(&c);
		return LDAPCLIENT_ERR_PROTOCOL;
	}
	if (send_all(&c, sendbuf, sendlen) != 0) {
		ldap_conn_close(&c);
		return LDAPCLIENT_ERR_CONNECT;
	}

	err = LDAPCLIENT_OK;
	for (;;) {
		struct ber_tlv op;

		if (ber_read_message(&c, recvbuf, sizeof(recvbuf), &recvlen) != 0) {
			err = LDAPCLIENT_ERR_CONNECT;
			break;
		}
		if (ber_parse_message_op(recvbuf, recvlen, &op) != 0) {
			err = LDAPCLIENT_ERR_PROTOCOL;
			break;
		}
		if (op.tag == LDAP_TAG_SEARCH_RESULT_ENTRY) {
			match_count++;
			continue;
		}
		if (op.tag == LDAP_TAG_SEARCH_RESULT_DONE) {
			struct ber_tlv resultcode;

			if (ber_parse_tlv(op.content, op.content_len, &resultcode) != 0 || resultcode.tag != BER_TAG_ENUMERATED) {
				err = LDAPCLIENT_ERR_PROTOCOL;
				break;
			}
			result_code = (int)ber_parse_integer(resultcode.content, resultcode.content_len);
			break;
		}
		/* anything else (e.g. a referral) is outside this client's
		 * deliberately narrow scope -- see ldapclient.h */
		err = LDAPCLIENT_ERR_PROTOCOL;
		break;
	}

	send_unbind(&c, 3);
	ldap_conn_close(&c);

	if (err != LDAPCLIENT_OK)
		return err;
	if (result_code != 0) {
		if (out_ldap_result_code)
			*out_ldap_result_code = result_code;
		return LDAPCLIENT_ERR_LDAP_RESULT;
	}
	if (out_match_count)
		*out_match_count = match_count;
	return LDAPCLIENT_OK;
}
