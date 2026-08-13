#include "ldapclient.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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

static int send_all(int fd, const unsigned char *buf, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);

		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		sent += (size_t)n;
	}
	return 0;
}

static int recv_full(int fd, unsigned char *buf, size_t need)
{
	size_t got = 0;

	while (got < need) {
		ssize_t n = recv(fd, buf + got, need - got, 0);

		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		got += (size_t)n;
	}
	return 0;
}

/* reads exactly one full LDAPMessage TLV off the wire (tag + definite-form
 * length + content), framing purely from the BER length -- LDAP over TCP
 * has no other message framing */
static int ber_read_message(int fd, unsigned char *buf, size_t bufcap, size_t *out_len)
{
	unsigned char lenbyte;
	size_t pos = 2;
	size_t content_len;

	if (recv_full(fd, buf, 2) != 0)
		return -1;
	lenbyte = buf[1];
	if (lenbyte < 0x80) {
		content_len = lenbyte;
	} else {
		int nbytes = lenbyte & 0x7F;

		if (nbytes == 0 || (size_t)nbytes > sizeof(size_t) || 2 + (size_t)nbytes > bufcap)
			return -1;
		if (recv_full(fd, buf + 2, (size_t)nbytes) != 0)
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
	if (content_len > 0 && recv_full(fd, buf + pos, content_len) != 0)
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

static void send_unbind(int fd, long message_id)
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
	send_all(fd, message.data, message.len);
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

enum ldapclient_error ldapclient_bind(const char *host, int port, const char *dn, const char *password, int timeout_ms, int *out_ldap_result_code)
{
	int fd;
	unsigned char sendbuf[LDAP_MSG_MAX];
	unsigned char recvbuf[LDAP_MSG_MAX];
	size_t sendlen, recvlen;
	int result_code = -1;
	enum ldapclient_error err;

	if (out_ldap_result_code)
		*out_ldap_result_code = -1;

	fd = ldapclient_connect(host, port, timeout_ms);
	if (fd < 0)
		return LDAPCLIENT_ERR_CONNECT;

	if (build_bind_request(1, dn, password, sendbuf, sizeof(sendbuf), &sendlen) != 0) {
		close(fd);
		return LDAPCLIENT_ERR_PROTOCOL;
	}
	if (send_all(fd, sendbuf, sendlen) != 0) {
		close(fd);
		return LDAPCLIENT_ERR_CONNECT;
	}
	if (ber_read_message(fd, recvbuf, sizeof(recvbuf), &recvlen) != 0) {
		close(fd);
		return LDAPCLIENT_ERR_CONNECT;
	}

	err = parse_ldap_result_message(recvbuf, recvlen, LDAP_TAG_BIND_RESPONSE, &result_code);

	send_unbind(fd, 2);
	close(fd);

	if (err != LDAPCLIENT_OK)
		return err;
	if (out_ldap_result_code)
		*out_ldap_result_code = result_code;
	return (result_code == 0) ? LDAPCLIENT_OK : LDAPCLIENT_ERR_LDAP_RESULT;
}

enum ldapclient_error ldapclient_bind_and_search(const char *host, int port, const char *bind_dn, const char *bind_password,
                                                  const char *base_dn, const char *const attrs[][2], int attr_count,
                                                  int timeout_ms, int *out_match_count, int *out_ldap_result_code)
{
	int fd;
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

	fd = ldapclient_connect(host, port, timeout_ms);
	if (fd < 0)
		return LDAPCLIENT_ERR_CONNECT;

	if (build_bind_request(1, bind_dn, bind_password, sendbuf, sizeof(sendbuf), &sendlen) != 0) {
		close(fd);
		return LDAPCLIENT_ERR_PROTOCOL;
	}
	if (send_all(fd, sendbuf, sendlen) != 0) {
		close(fd);
		return LDAPCLIENT_ERR_CONNECT;
	}
	if (ber_read_message(fd, recvbuf, sizeof(recvbuf), &recvlen) != 0) {
		close(fd);
		return LDAPCLIENT_ERR_CONNECT;
	}
	err = parse_ldap_result_message(recvbuf, recvlen, LDAP_TAG_BIND_RESPONSE, &bind_result);
	if (err != LDAPCLIENT_OK) {
		send_unbind(fd, 3);
		close(fd);
		return err;
	}
	if (bind_result != 0) {
		if (out_ldap_result_code)
			*out_ldap_result_code = bind_result;
		send_unbind(fd, 3);
		close(fd);
		return LDAPCLIENT_ERR_LDAP_RESULT;
	}

	if (build_search_request(2, base_dn, attrs, attr_count, sendbuf, sizeof(sendbuf), &sendlen) != 0) {
		send_unbind(fd, 3);
		close(fd);
		return LDAPCLIENT_ERR_PROTOCOL;
	}
	if (send_all(fd, sendbuf, sendlen) != 0) {
		close(fd);
		return LDAPCLIENT_ERR_CONNECT;
	}

	err = LDAPCLIENT_OK;
	for (;;) {
		struct ber_tlv op;

		if (ber_read_message(fd, recvbuf, sizeof(recvbuf), &recvlen) != 0) {
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

	send_unbind(fd, 3);
	close(fd);

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
