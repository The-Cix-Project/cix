/*
 * OpenPGP clearsign verification. See pgpverify.h for why this exists
 * and for the deliberate split between framing (here) and cryptography
 * (OpenSSL's).
 *
 * Every read of attacker-influenced bytes goes through the `rd_*`
 * helpers, which carry the remaining length and refuse to read past it.
 * That is the whole safety argument for this file: there is no pointer
 * arithmetic on packet data outside them.
 */
#include "pgpverify.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PGP_TAG_SIGNATURE 2
#define PGP_TAG_PUBKEY 6
#define PGP_PK_RSA 1
#define PGP_HASH_SHA256 8
#define PGP_HASH_SHA384 9
#define PGP_HASH_SHA512 10
#define PGP_FPR_LEN 20

const char *pgp_verify_result_name(enum pgp_verify_result r)
{
	switch (r) {
	case PGP_VERIFY_OK:
		return "ok";
	case PGP_VERIFY_BAD_SIGNATURE:
		return "bad signature";
	case PGP_VERIFY_WRONG_KEY:
		return "wrong key";
	case PGP_VERIFY_MALFORMED:
		return "malformed";
	case PGP_VERIFY_UNSUPPORTED:
		return "unsupported";
	case PGP_VERIFY_INTERNAL:
		return "internal error";
	}
	return "unknown";
}

static void seterr(char *err, size_t n, const char *fmt, ...)
{
	va_list ap;

	if (err == NULL || n == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, n, fmt, ap);
	va_end(ap);
}

/* ---- bounds-checked reader ------------------------------------- */

struct rd {
	const unsigned char *p;
	size_t left;
};

static int rd_u8(struct rd *r, unsigned *out)
{
	if (r->left < 1)
		return -1;
	*out = r->p[0];
	r->p += 1;
	r->left -= 1;
	return 0;
}

static int rd_u16(struct rd *r, unsigned *out)
{
	if (r->left < 2)
		return -1;
	*out = ((unsigned)r->p[0] << 8) | r->p[1];
	r->p += 2;
	r->left -= 2;
	return 0;
}

static int rd_u32(struct rd *r, unsigned long *out)
{
	if (r->left < 4)
		return -1;
	*out = ((unsigned long)r->p[0] << 24) | ((unsigned long)r->p[1] << 16) |
	       ((unsigned long)r->p[2] << 8) | r->p[3];
	r->p += 4;
	r->left -= 4;
	return 0;
}

static int rd_skip(struct rd *r, size_t n)
{
	if (r->left < n)
		return -1;
	r->p += n;
	r->left -= n;
	return 0;
}

/* An OpenPGP MPI: a 2-byte bit count, then ceil(bits/8) bytes. */
static int rd_mpi(struct rd *r, const unsigned char **out, size_t *out_len)
{
	unsigned bits;
	size_t bytes;

	if (rd_u16(r, &bits) != 0)
		return -1;
	bytes = (bits + 7) / 8;
	if (r->left < bytes)
		return -1;
	*out = r->p;
	*out_len = bytes;
	r->p += bytes;
	r->left -= bytes;
	return 0;
}

/* ---- ASCII armor ------------------------------------------------ */

static int b64val(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

/*
 * Decodes the base64 body of an armored block: everything between the
 * BEGIN line and the END line, skipping armor headers ("Key: value"),
 * the blank line that ends them, and the "=CRC" checksum line.
 *
 * The CRC24 is deliberately not checked. It detects transmission
 * damage, not tampering, and anything it would catch the signature
 * catches properly.
 */
static int armor_decode(const char *doc, size_t len, const char *begin, unsigned char **out,
                        size_t *out_len)
{
	const char *s = NULL, *e, *line;
	unsigned char *buf;
	size_t cap = 0, n = 0;
	unsigned acc = 0;
	int bits = 0;
	int seen_blank = 0;
	size_t blen = strlen(begin);
	size_t i;

	for (i = 0; i + blen <= len; i++) {
		if (memcmp(doc + i, begin, blen) == 0) {
			s = doc + i + blen;
			break;
		}
	}
	if (s == NULL)
		return -1;
	while (s < doc + len && *s != '\n')
		s++;
	if (s >= doc + len)
		return -1;
	s++;

	e = s;
	while (e < doc + len && !(e + 5 <= doc + len && memcmp(e, "-----", 5) == 0))
		e++;
	if (e >= doc + len)
		return -1;

	cap = (size_t)(e - s);
	buf = malloc(cap + 1);
	if (buf == NULL)
		return -1;

	line = s;
	while (line < e) {
		const char *eol = line;
		size_t llen;

		while (eol < e && *eol != '\n')
			eol++;
		llen = (size_t)(eol - line);
		while (llen > 0 && (line[llen - 1] == '\r' || line[llen - 1] == ' '))
			llen--;

		if (llen == 0) {
			seen_blank = 1;
		} else if (!seen_blank && memchr(line, ':', llen) != NULL) {
			/* armor header, skip */
		} else if (line[0] == '=') {
			/* CRC line: end of data */
			break;
		} else {
			size_t k;

			for (k = 0; k < llen; k++) {
				int v;

				if (line[k] == '=')
					break;
				v = b64val((unsigned char)line[k]);
				if (v < 0) {
					free(buf);
					return -1;
				}
				acc = (acc << 6) | (unsigned)v;
				bits += 6;
				if (bits >= 8) {
					bits -= 8;
					if (n >= cap) {
						free(buf);
						return -1;
					}
					buf[n++] = (unsigned char)((acc >> bits) & 0xff);
				}
			}
		}
		line = (eol < e) ? eol + 1 : e;
	}

	*out = buf;
	*out_len = n;
	return 0;
}

/* ---- packet header ---------------------------------------------- */

/*
 * Returns the tag and points body/body_len at the packet contents.
 * Partial body lengths (new-format, 224 <= len < 255) are refused
 * rather than partially handled -- they do not occur in the documents
 * this verifies, and half-supporting a framing feature is how parsers
 * become exploitable.
 */
static int pkt_read(struct rd *r, unsigned *tag, const unsigned char **body, size_t *body_len)
{
	unsigned c, l1;
	unsigned long len;

	if (rd_u8(r, &c) != 0)
		return -1;
	if ((c & 0x80) == 0)
		return -1;

	if (c & 0x40) {
		*tag = c & 0x3f;
		if (rd_u8(r, &l1) != 0)
			return -1;
		if (l1 < 192) {
			len = l1;
		} else if (l1 < 224) {
			unsigned l2;

			if (rd_u8(r, &l2) != 0)
				return -1;
			len = ((unsigned long)(l1 - 192) << 8) + l2 + 192;
		} else if (l1 == 255) {
			if (rd_u32(r, &len) != 0)
				return -1;
		} else {
			return -1; /* partial body length */
		}
	} else {
		unsigned lt = c & 0x03;

		*tag = (c >> 2) & 0x0f;
		if (lt == 0) {
			if (rd_u8(r, &l1) != 0)
				return -1;
			len = l1;
		} else if (lt == 1) {
			unsigned v;

			if (rd_u16(r, &v) != 0)
				return -1;
			len = v;
		} else if (lt == 2) {
			if (rd_u32(r, &len) != 0)
				return -1;
		} else {
			return -1; /* indeterminate length */
		}
	}

	if (len > r->left)
		return -1;
	*body = r->p;
	*body_len = (size_t)len;
	r->p += len;
	r->left -= (size_t)len;
	return 0;
}

/* ---- public key -------------------------------------------------- */

struct pgp_key {
	const unsigned char *n;
	size_t n_len;
	const unsigned char *e;
	size_t e_len;
	unsigned char fpr[PGP_FPR_LEN];
};

/*
 * A v4 fingerprint is SHA-1 over 0x99, the 2-byte packet length, and
 * the packet body. SHA-1 here is an identifier, not a security
 * decision: it is how OpenPGP names a v4 key, and the trust decision
 * is the caller comparing that name against a fingerprint a human
 * pinned. Nothing is authenticated by this hash.
 */
static int key_fingerprint(const unsigned char *body, size_t len, unsigned char out[PGP_FPR_LEN])
{
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	unsigned char hdr[3];
	unsigned int outlen = 0;
	int ok = 0;

	if (ctx == NULL)
		return -1;
	hdr[0] = 0x99;
	hdr[1] = (unsigned char)((len >> 8) & 0xff);
	hdr[2] = (unsigned char)(len & 0xff);
	if (EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) == 1 && EVP_DigestUpdate(ctx, hdr, 3) == 1 &&
	    EVP_DigestUpdate(ctx, body, len) == 1 && EVP_DigestFinal_ex(ctx, out, &outlen) == 1 &&
	    outlen == PGP_FPR_LEN)
		ok = 1;
	EVP_MD_CTX_free(ctx);
	return ok ? 0 : -1;
}

/*
 * Finds the first RSA public-key packet. A transferable public key
 * begins with its primary key, and that is the one whose fingerprint
 * the caller pins -- subkeys are deliberately not searched, so a
 * keyring cannot smuggle in an unpinned signing subkey.
 */
static enum pgp_verify_result key_parse(const unsigned char *buf, size_t len, struct pgp_key *k,
                                         char *err, size_t errsz)
{
	struct rd r;

	r.p = buf;
	r.left = len;
	for (;;) {
		const unsigned char *body;
		size_t body_len;
		unsigned tag, ver, algo;
		struct rd b;

		if (pkt_read(&r, &tag, &body, &body_len) != 0) {
			seterr(err, errsz, "no public-key packet in the armored key");
			return PGP_VERIFY_MALFORMED;
		}
		if (tag != PGP_TAG_PUBKEY)
			continue;

		b.p = body;
		b.left = body_len;
		if (rd_u8(&b, &ver) != 0) {
			seterr(err, errsz, "truncated public-key packet");
			return PGP_VERIFY_MALFORMED;
		}
		if (ver != 4) {
			seterr(err, errsz, "public key is version %u, only version 4 is supported", ver);
			return PGP_VERIFY_UNSUPPORTED;
		}
		if (rd_skip(&b, 4) != 0 || rd_u8(&b, &algo) != 0) {
			seterr(err, errsz, "truncated public-key packet");
			return PGP_VERIFY_MALFORMED;
		}
		if (algo != PGP_PK_RSA) {
			seterr(err, errsz, "public key algorithm %u is not RSA, which is all this verifies",
			       algo);
			return PGP_VERIFY_UNSUPPORTED;
		}
		if (rd_mpi(&b, &k->n, &k->n_len) != 0 || rd_mpi(&b, &k->e, &k->e_len) != 0) {
			seterr(err, errsz, "truncated RSA key material");
			return PGP_VERIFY_MALFORMED;
		}
		if (key_fingerprint(body, body_len, k->fpr) != 0) {
			seterr(err, errsz, "could not compute the key fingerprint");
			return PGP_VERIFY_INTERNAL;
		}
		return PGP_VERIFY_OK;
	}
}

/* ---- cleartext canonicalisation (RFC 4880 section 7.1) ----------- */

/*
 * The signed text is NOT the bytes between the headers. Two
 * transformations are mandatory and getting either wrong makes valid
 * documents fail and, worse, could make altered ones pass:
 *
 *   - dash-unescaping: a line beginning "- " has those two characters
 *     removed. That is what lets a signed document contain a line that
 *     itself looks like an armor header.
 *   - trailing whitespace on each line is not signed, because mail
 *     transports add and remove it.
 *
 * Lines are joined with CRLF and the final line has no terminator.
 */
static int canonicalise(const char *text, size_t len, unsigned char **out, size_t *out_len)
{
	/*
	 * Two bytes per input byte, not len + 2.
	 *
	 * An LF-terminated input line becomes a CRLF-separated output
	 * line, so each line can grow by one byte and a k-line document by
	 * up to k. `len + 2` was therefore an UNDERESTIMATE for anything
	 * over two lines, and kernel.org's own sha256sums.asc -- about two
	 * hundred lines -- overran the allocation by about two hundred
	 * bytes and corrupted the heap. Found by running this against the
	 * real document rather than only against fixtures written here,
	 * which is the argument for testing against real inputs.
	 *
	 * len * 2 + 2 cannot be exceeded: each input byte contributes at
	 * most itself plus one CR, and the +2 covers a lone final line.
	 */
	unsigned char *buf;
	size_t cap;
	size_t n = 0;
	const char *p = text;
	const char *end = text + len;
	int first = 1;

	if (len > (size_t)-1 / 2 - 2)
		return -1;
	cap = len * 2 + 2;
	buf = malloc(cap);
	if (buf == NULL)
		return -1;
	while (p < end) {
		const char *eol = p;
		size_t llen;

		while (eol < end && *eol != '\n')
			eol++;
		llen = (size_t)(eol - p);
		if (llen > 0 && p[llen - 1] == '\r')
			llen--;
		while (llen > 0 && (p[llen - 1] == ' ' || p[llen - 1] == '\t'))
			llen--;

		if (!first) {
			if (n + 2 > cap) {
				free(buf);
				return -1;
			}
			buf[n++] = '\r';
			buf[n++] = '\n';
		}
		first = 0;

		if (llen >= 2 && p[0] == '-' && p[1] == ' ') {
			if (n + (llen - 2) > cap) {
				free(buf);
				return -1;
			}
			memcpy(buf + n, p + 2, llen - 2);
			n += llen - 2;
		} else if (llen > 0) {
			if (n + llen > cap) {
				free(buf);
				return -1;
			}
			memcpy(buf + n, p, llen);
			n += llen;
		}
		p = (eol < end) ? eol + 1 : end;
	}
	*out = buf;
	*out_len = n;
	return 0;
}

/* ---- signature verification -------------------------------------- */

static const EVP_MD *md_for(unsigned hash_algo)
{
	switch (hash_algo) {
	case PGP_HASH_SHA256:
		return EVP_sha256();
	case PGP_HASH_SHA384:
		return EVP_sha384();
	case PGP_HASH_SHA512:
		return EVP_sha512();
	default:
		return NULL;
	}
}

static EVP_PKEY *rsa_pubkey(const struct pgp_key *k)
{
	OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
	OSSL_PARAM *params = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *pkey = NULL;
	BIGNUM *n = NULL, *e = NULL;

	if (bld == NULL)
		return NULL;
	n = BN_bin2bn(k->n, (int)k->n_len, NULL);
	e = BN_bin2bn(k->e, (int)k->e_len, NULL);
	if (n != NULL && e != NULL && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) == 1 &&
	    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e) == 1) {
		params = OSSL_PARAM_BLD_to_param(bld);
	}
	if (params != NULL) {
		ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
		if (ctx != NULL && EVP_PKEY_fromdata_init(ctx) == 1)
			(void)EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);
	}
	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(bld);
	BN_free(n);
	BN_free(e);
	return pkey;
}

/*
 * The hash a v4 signature commits to is the canonical text followed by
 * the signature's own metadata: version, type, algorithms, and the
 * whole hashed-subpacket area, then a six-byte trailer giving the
 * length of that metadata.
 *
 * The trailer is the part that is easy to get wrong and expensive to
 * get wrong: without it, an attacker could alter the signature's own
 * claims (its type, or a hashed subpacket such as the issuer) while
 * the signature still verified over the same text.
 */
static enum pgp_verify_result sig_hash(const EVP_MD *md, const unsigned char *text, size_t text_len,
                                        const unsigned char *sigbody, size_t meta_len,
                                        unsigned char *out, unsigned int *out_len)
{
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	unsigned char trailer[6];
	enum pgp_verify_result rc = PGP_VERIFY_INTERNAL;

	if (ctx == NULL)
		return rc;
	trailer[0] = 0x04;
	trailer[1] = 0xff;
	trailer[2] = (unsigned char)((meta_len >> 24) & 0xff);
	trailer[3] = (unsigned char)((meta_len >> 16) & 0xff);
	trailer[4] = (unsigned char)((meta_len >> 8) & 0xff);
	trailer[5] = (unsigned char)(meta_len & 0xff);

	if (EVP_DigestInit_ex(ctx, md, NULL) == 1 && EVP_DigestUpdate(ctx, text, text_len) == 1 &&
	    EVP_DigestUpdate(ctx, sigbody, meta_len) == 1 && EVP_DigestUpdate(ctx, trailer, 6) == 1 &&
	    EVP_DigestFinal_ex(ctx, out, out_len) == 1)
		rc = PGP_VERIFY_OK;
	EVP_MD_CTX_free(ctx);
	return rc;
}

static int fpr_matches(const unsigned char *fpr, const char *expect)
{
	char hex[PGP_FPR_LEN * 2 + 1];
	size_t i, j = 0;
	char want[PGP_FPR_LEN * 2 + 1];

	for (i = 0; i < PGP_FPR_LEN; i++)
		snprintf(hex + i * 2, 3, "%02x", fpr[i]);
	for (i = 0; expect[i] != '\0'; i++) {
		char c = expect[i];

		if (c == ' ' || c == ':')
			continue;
		if (j >= sizeof(want) - 1)
			return 0;
		if (c >= 'A' && c <= 'F')
			c = (char)(c - 'A' + 'a');
		want[j++] = c;
	}
	want[j] = '\0';
	return j == PGP_FPR_LEN * 2 && strcmp(hex, want) == 0;
}

enum pgp_verify_result pgp_clearsign_verify(const char *doc, size_t doc_len,
                                             const char *pubkey_armored, size_t pubkey_len,
                                             const char *expect_fingerprint, char **out_text,
                                             size_t *out_len, char *err, size_t err_size)
{
	static const char BEGIN_MSG[] = "-----BEGIN PGP SIGNED MESSAGE-----";
	static const char BEGIN_SIG[] = "-----BEGIN PGP SIGNATURE-----";
	const char *text_start, *text_end, *p;
	unsigned char *keybuf = NULL, *sigbuf = NULL, *canon = NULL;
	size_t keylen = 0, siglen = 0, canon_len = 0;
	struct pgp_key key;
	struct rd r, b;
	const unsigned char *sigbody, *mpi;
	size_t sigbody_len, mpi_len, meta_len;
	unsigned tag, ver, sigtype, pkalgo, hashalgo, hlen, ulen;
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;
	const EVP_MD *md;
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *vctx = NULL;
	enum pgp_verify_result rc;
	size_t i;

	if (out_text != NULL)
		*out_text = NULL;
	if (out_len != NULL)
		*out_len = 0;

	if (expect_fingerprint == NULL || expect_fingerprint[0] == '\0') {
		seterr(err, err_size,
		       "no fingerprint to verify against -- checking a signature without pinning the key "
		       "that made it is not verification");
		return PGP_VERIFY_WRONG_KEY;
	}

	/* The signed text: after the blank line that ends the armor
	 * headers, up to the signature block. */
	p = NULL;
	for (i = 0; i + sizeof(BEGIN_MSG) - 1 <= doc_len; i++) {
		if (memcmp(doc + i, BEGIN_MSG, sizeof(BEGIN_MSG) - 1) == 0) {
			p = doc + i + sizeof(BEGIN_MSG) - 1;
			break;
		}
	}
	if (p == NULL) {
		seterr(err, err_size, "not a clearsigned document (no BEGIN PGP SIGNED MESSAGE)");
		return PGP_VERIFY_MALFORMED;
	}
	while (p < doc + doc_len && *p != '\n')
		p++;
	if (p < doc + doc_len)
		p++;
	/* skip armor headers up to and including the blank line */
	while (p < doc + doc_len) {
		const char *eol = p;

		while (eol < doc + doc_len && *eol != '\n')
			eol++;
		if (eol == p || (eol == p + 1 && *p == '\r')) {
			p = (eol < doc + doc_len) ? eol + 1 : doc + doc_len;
			break;
		}
		p = (eol < doc + doc_len) ? eol + 1 : doc + doc_len;
	}
	text_start = p;

	text_end = NULL;
	for (i = (size_t)(text_start - doc); i + sizeof(BEGIN_SIG) - 1 <= doc_len; i++) {
		if (memcmp(doc + i, BEGIN_SIG, sizeof(BEGIN_SIG) - 1) == 0) {
			text_end = doc + i;
			break;
		}
	}
	if (text_end == NULL) {
		seterr(err, err_size, "clearsigned document has no signature block");
		return PGP_VERIFY_MALFORMED;
	}
	/* the newline before the signature armor is a delimiter, not text */
	if (text_end > text_start && text_end[-1] == '\n')
		text_end--;
	if (text_end > text_start && text_end[-1] == '\r')
		text_end--;

	if (armor_decode(doc, doc_len, BEGIN_SIG, &sigbuf, &siglen) != 0) {
		seterr(err, err_size, "could not decode the signature armor");
		return PGP_VERIFY_MALFORMED;
	}
	if (armor_decode(pubkey_armored, pubkey_len, "-----BEGIN PGP PUBLIC KEY BLOCK-----", &keybuf,
	                 &keylen) != 0) {
		free(sigbuf);
		seterr(err, err_size, "could not decode the public key armor");
		return PGP_VERIFY_MALFORMED;
	}

	rc = key_parse(keybuf, keylen, &key, err, err_size);
	if (rc != PGP_VERIFY_OK)
		goto out;

	if (!fpr_matches(key.fpr, expect_fingerprint)) {
		char got[PGP_FPR_LEN * 2 + 1];

		for (i = 0; i < PGP_FPR_LEN; i++)
			snprintf(got + i * 2, 3, "%02X", key.fpr[i]);
		seterr(err, err_size, "key fingerprint %s is not the pinned %s", got, expect_fingerprint);
		rc = PGP_VERIFY_WRONG_KEY;
		goto out;
	}

	r.p = sigbuf;
	r.left = siglen;
	if (pkt_read(&r, &tag, &sigbody, &sigbody_len) != 0 || tag != PGP_TAG_SIGNATURE) {
		seterr(err, err_size, "signature armor does not contain a signature packet");
		rc = PGP_VERIFY_MALFORMED;
		goto out;
	}

	b.p = sigbody;
	b.left = sigbody_len;
	if (rd_u8(&b, &ver) != 0 || rd_u8(&b, &sigtype) != 0 || rd_u8(&b, &pkalgo) != 0 ||
	    rd_u8(&b, &hashalgo) != 0 || rd_u16(&b, &hlen) != 0) {
		seterr(err, err_size, "truncated signature packet");
		rc = PGP_VERIFY_MALFORMED;
		goto out;
	}
	if (ver != 4) {
		seterr(err, err_size, "signature is version %u, only version 4 is supported", ver);
		rc = PGP_VERIFY_UNSUPPORTED;
		goto out;
	}
	if (sigtype != 0x01 && sigtype != 0x00) {
		seterr(err, err_size, "signature type 0x%02x is not a document signature", sigtype);
		rc = PGP_VERIFY_UNSUPPORTED;
		goto out;
	}
	if (pkalgo != PGP_PK_RSA) {
		seterr(err, err_size, "signature algorithm %u is not RSA, which is all this verifies",
		       pkalgo);
		rc = PGP_VERIFY_UNSUPPORTED;
		goto out;
	}
	md = md_for(hashalgo);
	if (md == NULL) {
		seterr(err, err_size, "hash algorithm %u is not supported", hashalgo);
		rc = PGP_VERIFY_UNSUPPORTED;
		goto out;
	}
	if (rd_skip(&b, hlen) != 0 || rd_u16(&b, &ulen) != 0 || rd_skip(&b, ulen) != 0 ||
	    rd_skip(&b, 2) != 0 || rd_mpi(&b, &mpi, &mpi_len) != 0) {
		seterr(err, err_size, "truncated signature packet");
		rc = PGP_VERIFY_MALFORMED;
		goto out;
	}
	meta_len = 6 + (size_t)hlen;

	if (canonicalise(text_start, (size_t)(text_end - text_start), &canon, &canon_len) != 0) {
		seterr(err, err_size, "out of memory canonicalising the signed text");
		rc = PGP_VERIFY_INTERNAL;
		goto out;
	}
	rc = sig_hash(md, canon, canon_len, sigbody, meta_len, digest, &digest_len);
	if (rc != PGP_VERIFY_OK) {
		seterr(err, err_size, "could not hash the signed text");
		goto out;
	}

	pkey = rsa_pubkey(&key);
	if (pkey == NULL) {
		seterr(err, err_size, "could not load the RSA public key");
		rc = PGP_VERIFY_INTERNAL;
		goto out;
	}
	vctx = EVP_PKEY_CTX_new(pkey, NULL);
	if (vctx == NULL || EVP_PKEY_verify_init(vctx) != 1 ||
	    EVP_PKEY_CTX_set_rsa_padding(vctx, RSA_PKCS1_PADDING) != 1 ||
	    EVP_PKEY_CTX_set_signature_md(vctx, md) != 1) {
		seterr(err, err_size, "could not set up RSA verification");
		rc = PGP_VERIFY_INTERNAL;
		goto out;
	}
	if (EVP_PKEY_verify(vctx, mpi, mpi_len, digest, digest_len) != 1) {
		seterr(err, err_size, "signature does not verify against the pinned key");
		rc = PGP_VERIFY_BAD_SIGNATURE;
		goto out;
	}

	if (out_text != NULL) {
		char *t = malloc(canon_len + 1);

		if (t == NULL) {
			seterr(err, err_size, "out of memory returning the verified text");
			rc = PGP_VERIFY_INTERNAL;
			goto out;
		}
		memcpy(t, canon, canon_len);
		t[canon_len] = '\0';
		*out_text = t;
		if (out_len != NULL)
			*out_len = canon_len;
	}
	seterr(err, err_size, "ok");
	rc = PGP_VERIFY_OK;

out:
	EVP_PKEY_CTX_free(vctx);
	EVP_PKEY_free(pkey);
	free(canon);
	free(keybuf);
	free(sigbuf);
	return rc;
}

int pgp_checksum_lookup(const char *verified_text, const char *filename, char *out_sha256,
                        size_t out_size)
{
	const char *p = verified_text;
	size_t namelen = strlen(filename);

	if (out_size < 65)
		return -1;
	while (*p != '\0') {
		const char *eol = strchr(p, '\n');
		size_t llen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
		size_t i = 0;

		/* The verified text is CRLF-canonical (RFC 4880 section 7.1),
		 * so a line ends "\r\n" and the CR is part of the line here.
		 * Missing this made every lookup fail by exactly one byte. */
		if (llen > 0 && p[llen - 1] == '\r')
			llen--;

		while (i < llen && p[i] != ' ')
			i++;
		if (i == 64) {
			size_t j = i;

			while (j < llen && p[j] == ' ')
				j++;
			if (llen - j == namelen && memcmp(p + j, filename, namelen) == 0) {
				memcpy(out_sha256, p, 64);
				out_sha256[64] = '\0';
				return 0;
			}
		}
		if (eol == NULL)
			break;
		p = eol + 1;
	}
	return -1;
}
