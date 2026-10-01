/*
 * cix#351: cixd's certificate, export and signing work, done in the
 * libcrypto it already links instead of by forking /usr/bin/openssl.
 * See pkicrypto.h for what each function reproduces; every choice below
 * that matters for compatibility is taken from the CLI's own output on
 * 192.168.15.95 (probe-pki-cli@1, 2026-10-01, OpenSSL 3.0.20), not from
 * memory of what the CLI does.
 */
#include "pkicrypto.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

/* The export cipher's parameters: openssl enc -aes-256-cbc -pbkdf2
 * -iter 600000 -md sha256 -salt. Changing any of these makes every
 * existing export bundle undecryptable. */
#define EXPORT_ITERATIONS 600000
#define EXPORT_MAGIC "Salted__"
#define EXPORT_SALT_LEN 8

/* -CAcreateserial's serial: 159 random bits, so the DER INTEGER is
 * positive and at most 20 bytes (apps' SERIAL_RAND_BITS). */
#define SERIAL_RAND_BITS 159

static void fail(char *err, size_t err_size, const char *what)
{
	unsigned long e = ERR_get_error();

	if (err != NULL && err_size > 0) {
		if (e != 0) {
			char reason[200];

			ERR_error_string_n(e, reason, sizeof(reason));
			snprintf(err, err_size, "%s: %s", what, reason);
		} else {
			snprintf(err, err_size, "%s", what);
		}
	}
	ERR_clear_error();
}

static EVP_PKEY *load_key(const char *path, char *err, size_t err_size)
{
	FILE *f = fopen(path, "r");
	EVP_PKEY *key;

	if (f == NULL) {
		fail(err, err_size, "cannot open private key");
		return NULL;
	}
	key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
	fclose(f);
	if (key == NULL)
		fail(err, err_size, "cannot read private key");
	return key;
}

static X509 *load_cert(const char *path, char *err, size_t err_size)
{
	FILE *f = fopen(path, "r");
	X509 *cert;

	if (f == NULL) {
		fail(err, err_size, "cannot open certificate");
		return NULL;
	}
	cert = PEM_read_X509(f, NULL, NULL, NULL);
	fclose(f);
	if (cert == NULL)
		fail(err, err_size, "cannot read certificate");
	return cert;
}

/* Opens path for writing with exactly mode, whatever an existing file
 * had: a private key must never be readable for a moment by anyone
 * else, which `genpkey -out` followed by chmod allowed. */
static FILE *open_with_mode(const char *path, mode_t mode, const char *how)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	FILE *f;

	if (fd < 0)
		return NULL;
	if (fchmod(fd, mode) != 0) {
		close(fd);
		return NULL;
	}
	f = fdopen(fd, how);
	if (f == NULL)
		close(fd);
	return f;
}

static int write_cert(const char *path, X509 *cert, char *err, size_t err_size)
{
	FILE *f = open_with_mode(path, 0644, "w");
	int ok;

	if (f == NULL) {
		fail(err, err_size, "cannot create certificate file");
		return -1;
	}
	ok = PEM_write_X509(f, cert);
	if (fclose(f) != 0)
		ok = 0;
	if (!ok) {
		fail(err, err_size, "cannot write certificate");
		unlink(path);
		return -1;
	}
	return 0;
}

int pkicrypto_rsa_key_create(const char *key_path, char *err, size_t err_size)
{
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	EVP_PKEY *key = NULL;
	FILE *f;
	int ok;

	if (ctx == NULL || EVP_PKEY_keygen_init(ctx) <= 0 ||
	    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0 || EVP_PKEY_keygen(ctx, &key) <= 0) {
		fail(err, err_size, "RSA key generation failed");
		EVP_PKEY_CTX_free(ctx);
		return -1;
	}
	EVP_PKEY_CTX_free(ctx);

	f = open_with_mode(key_path, 0600, "w");
	if (f == NULL) {
		fail(err, err_size, "cannot create private key file");
		EVP_PKEY_free(key);
		return -1;
	}
	/* PKCS#8, unencrypted: "BEGIN PRIVATE KEY", as genpkey wrote. */
	ok = PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL);
	if (fclose(f) != 0)
		ok = 0;
	EVP_PKEY_free(key);
	if (!ok) {
		fail(err, err_size, "cannot write private key");
		unlink(key_path);
		return -1;
	}
	return 0;
}

/* Version, random serial, validity and subject: what every certificate
 * here shares. The subject's CN is a UTF8String, which is what the CLI's
 * -subj produced (the live root's DER, 0x0c). */
static X509 *cert_skeleton(EVP_PKEY *subject_key, const char *common_name, int days)
{
	X509 *cert = X509_new();
	X509_NAME *name = X509_NAME_new();
	BIGNUM *serial = BN_new();
	time_t now = time(NULL);
	int ok = cert != NULL && name != NULL && serial != NULL;

	ok = ok && X509_set_version(cert, 2) == 1;
	ok = ok && BN_rand(serial, SERIAL_RAND_BITS, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 1;
	ok = ok && BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(cert)) != NULL;
	/* One clock reading for both ends, so the validity is exactly days. */
	ok = ok && X509_time_adj_ex(X509_getm_notBefore(cert), 0, 0, &now) != NULL;
	ok = ok && X509_time_adj_ex(X509_getm_notAfter(cert), days, 0, &now) != NULL;
	ok = ok && X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_UTF8,
	                                      (const unsigned char *)common_name, -1, -1, 0) == 1;
	ok = ok && X509_set_subject_name(cert, name) == 1;
	ok = ok && X509_set_pubkey(cert, subject_key) == 1;
	BN_free(serial);
	X509_NAME_free(name);
	if (!ok) {
		X509_free(cert);
		return NULL;
	}
	return cert;
}

/* One extension in -addext's "name=value" form, through the same
 * libcrypto parser -addext used, so "critical," and every value syntax
 * mean what they meant on the command line. */
static int add_ext(X509 *cert, X509V3_CTX *ctx, const char *spec, char *err, size_t err_size)
{
	const char *eq = strchr(spec, '=');
	char name[64];
	X509_EXTENSION *ext;
	int ok;

	if (eq == NULL || (size_t)(eq - spec) >= sizeof(name)) {
		if (err != NULL && err_size > 0)
			snprintf(err, err_size, "extension \"%s\" is not name=value", spec);
		return -1;
	}
	memcpy(name, spec, (size_t)(eq - spec));
	name[eq - spec] = '\0';
	ext = X509V3_EXT_nconf(NULL, ctx, name, eq + 1);
	if (ext == NULL) {
		fail(err, err_size, "invalid extension");
		return -1;
	}
	ok = X509_add_ext(cert, ext, -1);
	X509_EXTENSION_free(ext);
	if (!ok) {
		fail(err, err_size, "cannot add extension");
		return -1;
	}
	return 0;
}

int pkicrypto_ca_create(const char *key_path, const char *common_name, int days,
                        const char *cert_path, char *err, size_t err_size)
{
	EVP_PKEY *key = load_key(key_path, err, err_size);
	X509 *cert;
	X509V3_CTX ctx;
	int rc = -1;

	if (key == NULL)
		return -1;
	cert = cert_skeleton(key, common_name, days);
	if (cert == NULL) {
		fail(err, err_size, "cannot build the CA certificate");
		EVP_PKEY_free(key);
		return -1;
	}
	if (X509_set_issuer_name(cert, X509_get_subject_name(cert)) != 1) {
		fail(err, err_size, "cannot set the CA issuer");
		goto out;
	}
	/* req -x509's defaults, in its order: SKID, AKID (key id only),
	 * basicConstraints critical CA:TRUE. */
	X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
	X509V3_set_ctx_nodb(&ctx);
	if (add_ext(cert, &ctx, "subjectKeyIdentifier=hash", err, err_size) != 0 ||
	    add_ext(cert, &ctx, "authorityKeyIdentifier=keyid", err, err_size) != 0 ||
	    add_ext(cert, &ctx, "basicConstraints=critical,CA:TRUE", err, err_size) != 0)
		goto out;
	if (X509_sign(cert, key, EVP_sha256()) <= 0) {
		fail(err, err_size, "cannot sign the CA certificate");
		goto out;
	}
	rc = write_cert(cert_path, cert, err, err_size);
out:
	X509_free(cert);
	EVP_PKEY_free(key);
	return rc;
}

int pkicrypto_cert_issue(const char *issuer_cert_path, const char *issuer_key_path,
                         const char *subject_key_path, const char *common_name,
                         const char *const *ext, int ext_count, int days,
                         const char *cert_path, char *err, size_t err_size)
{
	X509 *issuer = load_cert(issuer_cert_path, err, err_size);
	EVP_PKEY *issuer_key = NULL, *subject_key = NULL;
	X509 *cert = NULL;
	X509V3_CTX ctx;
	int i, rc = -1;

	if (issuer == NULL)
		return -1;
	issuer_key = load_key(issuer_key_path, err, err_size);
	if (issuer_key == NULL)
		goto out;
	subject_key = load_key(subject_key_path, err, err_size);
	if (subject_key == NULL)
		goto out;
	cert = cert_skeleton(subject_key, common_name, days);
	if (cert == NULL || X509_set_issuer_name(cert, X509_get_subject_name(issuer)) != 1) {
		fail(err, err_size, "cannot build the certificate");
		goto out;
	}
	/* x509 -req -copy_extensions copy: the request's extensions first,
	 * then the SKID and key-id AKID it adds itself. */
	X509V3_set_ctx(&ctx, issuer, cert, NULL, NULL, 0);
	X509V3_set_ctx_nodb(&ctx);
	for (i = 0; i < ext_count; i++)
		if (add_ext(cert, &ctx, ext[i], err, err_size) != 0)
			goto out;
	if (add_ext(cert, &ctx, "subjectKeyIdentifier=hash", err, err_size) != 0 ||
	    add_ext(cert, &ctx, "authorityKeyIdentifier=keyid", err, err_size) != 0)
		goto out;
	if (X509_sign(cert, issuer_key, EVP_sha256()) <= 0) {
		fail(err, err_size, "cannot sign the certificate");
		goto out;
	}
	rc = write_cert(cert_path, cert, err, err_size);
out:
	X509_free(cert);
	EVP_PKEY_free(subject_key);
	EVP_PKEY_free(issuer_key);
	X509_free(issuer);
	return rc;
}

/* What a memory BIO holds, NUL-terminated into out. */
static int bio_text(BIO *b, char *out, size_t out_size)
{
	char *data;
	long n = BIO_get_mem_data(b, &data);

	if (n < 0 || (size_t)n >= out_size)
		return -1;
	memcpy(out, data, (size_t)n);
	out[n] = '\0';
	return 0;
}

/* A certificate digest as uppercase hex pairs joined by ':'. */
static int fingerprint(X509 *cert, const EVP_MD *type, char *out, size_t out_size)
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int md_len = 0, i;

	if (X509_digest(cert, type, md, &md_len) != 1 || md_len == 0 || md_len * 3 > out_size)
		return -1;
	for (i = 0; i < md_len; i++)
		snprintf(out + i * 3, 4, i + 1 < md_len ? "%02X:" : "%02X", md[i]);
	return 0;
}

int pkicrypto_cert_fields(const char *cert_path, struct pkicrypto_cert_fields *out, char *err,
                          size_t err_size)
{
	X509 *cert = load_cert(cert_path, err, err_size);
	BIO *b;
	int ok = 1;

	memset(out, 0, sizeof(*out));
	if (cert == NULL)
		return -1;

	/* Each field in its own BIO: the text is whatever the CLI's
	 * -subject/-serial/-startdate/-enddate printed after the "=".
	 *
	 * The subject's XN_FLAG_ONELINE is a choice, made because it prints
	 * "CN = Cix Platform CA" exactly as the CLI did for the live root
	 * (test_pkicrypto checks it). It also escapes RFC 2253 specials,
	 * and how the CLI rendered a CN containing , + " < > ; or a leading
	 * '#' was not measured -- leaf CNs are DNS names and cannot contain
	 * them, so only a free-form CA name could, and none here does. */
	b = BIO_new(BIO_s_mem());
	ok = ok && b != NULL &&
	     X509_NAME_print_ex(b, X509_get_subject_name(cert), 0, XN_FLAG_ONELINE) >= 0 &&
	     bio_text(b, out->subject, sizeof(out->subject)) == 0;
	BIO_free(b);
	b = BIO_new(BIO_s_mem());
	ok = ok && b != NULL && i2a_ASN1_INTEGER(b, X509_get0_serialNumber(cert)) > 0 &&
	     bio_text(b, out->serial, sizeof(out->serial)) == 0;
	BIO_free(b);
	b = BIO_new(BIO_s_mem());
	ok = ok && b != NULL && ASN1_TIME_print(b, X509_get0_notBefore(cert)) == 1 &&
	     bio_text(b, out->not_before, sizeof(out->not_before)) == 0;
	BIO_free(b);
	b = BIO_new(BIO_s_mem());
	ok = ok && b != NULL && ASN1_TIME_print(b, X509_get0_notAfter(cert)) == 1 &&
	     bio_text(b, out->not_after, sizeof(out->not_after)) == 0;
	BIO_free(b);

	/* -fingerprint's default digest is SHA-1 (measured: "SHA1
	 * Fingerprint=" from OpenSSL 3.0.20); SHA-256 alongside, as
	 * "AA:BB:..." both. */
	ok = ok && fingerprint(cert, EVP_sha1(), out->sha1_fingerprint,
	                       sizeof(out->sha1_fingerprint)) == 0;
	ok = ok && fingerprint(cert, EVP_sha256(), out->sha256_fingerprint,
	                       sizeof(out->sha256_fingerprint)) == 0;
	X509_free(cert);
	if (!ok) {
		fail(err, err_size, "cannot read the certificate's fields");
		memset(out, 0, sizeof(*out));
		return -1;
	}
	return 0;
}

int pkicrypto_key_matches_cert(const char *key_path, const char *cert_path, char *err,
                               size_t err_size)
{
	EVP_PKEY *key = load_key(key_path, err, err_size);
	X509 *cert;
	int rc;

	if (key == NULL)
		return -1;
	cert = load_cert(cert_path, err, err_size);
	if (cert == NULL) {
		EVP_PKEY_free(key);
		return -1;
	}
	rc = EVP_PKEY_eq(key, X509_get0_pubkey(cert)) == 1 ? 1 : 0;
	ERR_clear_error();
	X509_free(cert);
	EVP_PKEY_free(key);
	return rc;
}

int pkicrypto_cert_pem_to_der(const char *pem_path, const char *der_path, char *err,
                              size_t err_size)
{
	X509 *cert = load_cert(pem_path, err, err_size);
	FILE *f;
	int ok;

	if (cert == NULL)
		return -1;
	f = open_with_mode(der_path, 0644, "wb");
	if (f == NULL) {
		fail(err, err_size, "cannot create the DER file");
		X509_free(cert);
		return -1;
	}
	ok = i2d_X509_fp(f, cert);
	if (fclose(f) != 0)
		ok = 0;
	X509_free(cert);
	if (!ok) {
		fail(err, err_size, "cannot write the DER file");
		unlink(der_path);
		return -1;
	}
	return 0;
}

/* The passphrase as `-pass file:` read it: the file's first line. A
 * passphrase with a newline in it was cut there, so it is cut here too
 * or a bundle exported that way would not decrypt. */
static size_t passphrase_len(const char *passphrase)
{
	const char *nl = strchr(passphrase, '\n');

	return nl != NULL ? (size_t)(nl - passphrase) : strlen(passphrase);
}

int pkicrypto_export_derive(const char *passphrase, const unsigned char salt[8],
                            unsigned char key[32], unsigned char iv[16])
{
	unsigned char both[48];

	if (PKCS5_PBKDF2_HMAC(passphrase, (int)passphrase_len(passphrase), salt, EXPORT_SALT_LEN,
	                      EXPORT_ITERATIONS, EVP_sha256(), (int)sizeof(both), both) != 1) {
		ERR_clear_error();
		return -1;
	}
	memcpy(key, both, 32);
	memcpy(iv, both + 32, 16);
	OPENSSL_cleanse(both, sizeof(both));
	return 0;
}

int pkicrypto_export_encrypt(const char *passphrase, const unsigned char *in, size_t in_len,
                             char **out_b64, char *err, size_t err_size)
{
	unsigned char key[32], iv[16];
	unsigned char *raw = NULL;
	char *b64 = NULL;
	EVP_CIPHER_CTX *ctx = NULL;
	int n1 = 0, n2 = 0;
	size_t raw_len;

	*out_b64 = NULL;
	if (in_len > (size_t)0x7fffff00) {
		if (err != NULL && err_size > 0)
			snprintf(err, err_size, "bundle too large");
		return -1;
	}
	/* "Salted__", the salt, then the ciphertext: the -salt layout. */
	raw = malloc(16 + in_len + 16);
	if (raw == NULL || RAND_bytes(raw + 8, EXPORT_SALT_LEN) != 1 ||
	    pkicrypto_export_derive(passphrase, raw + 8, key, iv) != 0) {
		fail(err, err_size, "cannot derive the export key");
		goto bad;
	}
	memcpy(raw, EXPORT_MAGIC, 8);
	ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL || EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1 ||
	    EVP_EncryptUpdate(ctx, raw + 16, &n1, in, (int)in_len) != 1 ||
	    EVP_EncryptFinal_ex(ctx, raw + 16 + n1, &n2) != 1) {
		fail(err, err_size, "export encryption failed");
		goto bad;
	}
	raw_len = 16 + (size_t)n1 + (size_t)n2;
	/* -a -A: one base64 line, no newline anywhere. */
	b64 = malloc(4 * ((raw_len + 2) / 3) + 1);
	if (b64 == NULL) {
		fail(err, err_size, "out of memory");
		goto bad;
	}
	EVP_EncodeBlock((unsigned char *)b64, raw, (int)raw_len);
	*out_b64 = b64;
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(key, sizeof(key));
	free(raw);
	return 0;
bad:
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(key, sizeof(key));
	free(raw);
	free(b64);
	return -1;
}

int pkicrypto_export_decrypt(const char *passphrase, const char *b64, size_t b64_len,
                             unsigned char **out, size_t *out_len, char *err, size_t err_size)
{
	unsigned char key[32], iv[16];
	unsigned char *clean = NULL, *raw = NULL, *plain = NULL;
	EVP_CIPHER_CTX *ctx = NULL;
	size_t i, n = 0, raw_len;
	int decoded, n1 = 0, n2 = 0;

	*out = NULL;
	*out_len = 0;
	clean = malloc(b64_len + 1);
	if (clean == NULL) {
		fail(err, err_size, "out of memory");
		return -1;
	}
	for (i = 0; i < b64_len; i++)
		if (b64[i] != ' ' && b64[i] != '\t' && b64[i] != '\r' && b64[i] != '\n')
			clean[n++] = (unsigned char)b64[i];
	if (n == 0 || n % 4 != 0 || n > (size_t)0x7ffffff0) {
		fail(err, err_size, "the bundle is not base64");
		goto bad;
	}
	raw = malloc(n / 4 * 3 + 1);
	if (raw == NULL) {
		fail(err, err_size, "out of memory");
		goto bad;
	}
	decoded = EVP_DecodeBlock(raw, clean, (int)n);
	if (decoded < 0) {
		fail(err, err_size, "the bundle is not base64");
		goto bad;
	}
	/* EVP_DecodeBlock counts the bytes the '=' padding stands for. */
	raw_len = (size_t)decoded - (clean[n - 1] == '=') - (clean[n - 2] == '=');
	if (raw_len < 16 + 16 || memcmp(raw, EXPORT_MAGIC, 8) != 0) {
		fail(err, err_size, "the bundle has no salt header");
		goto bad;
	}
	if (pkicrypto_export_derive(passphrase, raw + 8, key, iv) != 0) {
		fail(err, err_size, "cannot derive the export key");
		goto bad;
	}
	plain = malloc(raw_len - 16 + 16 + 1);
	ctx = EVP_CIPHER_CTX_new();
	if (plain == NULL || ctx == NULL ||
	    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1 ||
	    EVP_DecryptUpdate(ctx, plain, &n1, raw + 16, (int)(raw_len - 16)) != 1 ||
	    EVP_DecryptFinal_ex(ctx, plain + n1, &n2) != 1) {
		/* A wrong passphrase almost always ends here, as bad padding. */
		fail(err, err_size, "wrong passphrase, or the bundle is damaged");
		goto bad;
	}
	plain[n1 + n2] = '\0';
	*out = plain;
	*out_len = (size_t)(n1 + n2);
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(key, sizeof(key));
	free(clean);
	free(raw);
	return 0;
bad:
	EVP_CIPHER_CTX_free(ctx);
	OPENSSL_cleanse(key, sizeof(key));
	free(clean);
	free(raw);
	free(plain);
	return -1;
}

/* A whole file as one buffer, for Ed25519, which signs the message
 * itself (PureEdDSA) and so needs all of it at once -- as -rawin did.
 * Mapped rather than read: a release ISO is signed this way. */
struct mapped {
	const unsigned char *data;
	size_t len;
	void *map;
};

static int map_file(const char *path, struct mapped *m)
{
	static const unsigned char empty[1];
	struct stat st;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	m->map = NULL;
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}
	m->len = (size_t)st.st_size;
	if (m->len == 0) {
		m->data = empty;
		close(fd);
		return 0;
	}
	m->map = mmap(NULL, m->len, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (m->map == MAP_FAILED) {
		m->map = NULL;
		return -1;
	}
	m->data = m->map;
	return 0;
}

static void unmap_file(struct mapped *m)
{
	if (m->map != NULL)
		munmap(m->map, m->len);
}

int pkicrypto_ed25519_public_raw(const char *key_path,
                                 unsigned char out[PKICRYPTO_ED25519_PUB_LEN], char *err,
                                 size_t err_size)
{
	EVP_PKEY *key = load_key(key_path, err, err_size);
	size_t len = PKICRYPTO_ED25519_PUB_LEN;
	int ok;

	if (key == NULL)
		return -1;
	ok = EVP_PKEY_get_base_id(key) == EVP_PKEY_ED25519 &&
	     EVP_PKEY_get_raw_public_key(key, out, &len) == 1 && len == PKICRYPTO_ED25519_PUB_LEN;
	EVP_PKEY_free(key);
	if (!ok) {
		fail(err, err_size, "not an Ed25519 private key");
		return -1;
	}
	return 0;
}

int pkicrypto_ed25519_sign_file(const char *key_path, const char *path,
                                unsigned char out[PKICRYPTO_ED25519_SIG_LEN], char *err,
                                size_t err_size)
{
	EVP_PKEY *key = load_key(key_path, err, err_size);
	EVP_MD_CTX *md = NULL;
	struct mapped m;
	size_t sig_len = PKICRYPTO_ED25519_SIG_LEN;
	int ok;

	if (key == NULL)
		return -1;
	if (EVP_PKEY_get_base_id(key) != EVP_PKEY_ED25519) {
		fail(err, err_size, "not an Ed25519 private key");
		EVP_PKEY_free(key);
		return -1;
	}
	if (map_file(path, &m) != 0) {
		fail(err, err_size, "cannot read the file to sign");
		EVP_PKEY_free(key);
		return -1;
	}
	md = EVP_MD_CTX_new();
	ok = md != NULL && EVP_DigestSignInit(md, NULL, NULL, NULL, key) == 1 &&
	     EVP_DigestSign(md, out, &sig_len, m.data, m.len) == 1 &&
	     sig_len == PKICRYPTO_ED25519_SIG_LEN;
	if (!ok)
		fail(err, err_size, "Ed25519 signing failed");
	EVP_MD_CTX_free(md);
	unmap_file(&m);
	EVP_PKEY_free(key);
	return ok ? 0 : -1;
}

int pkicrypto_ed25519_verify_file(const unsigned char pub[PKICRYPTO_ED25519_PUB_LEN],
                                  const char *path,
                                  const unsigned char sig[PKICRYPTO_ED25519_SIG_LEN])
{
	EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub,
	                                            PKICRYPTO_ED25519_PUB_LEN);
	EVP_MD_CTX *md = NULL;
	struct mapped m;
	int ok;

	if (key == NULL) {
		ERR_clear_error();
		return -1;
	}
	if (map_file(path, &m) != 0) {
		EVP_PKEY_free(key);
		return -1;
	}
	md = EVP_MD_CTX_new();
	ok = md != NULL && EVP_DigestVerifyInit(md, NULL, NULL, NULL, key) == 1 &&
	     EVP_DigestVerify(md, sig, PKICRYPTO_ED25519_SIG_LEN, m.data, m.len) == 1;
	ERR_clear_error();
	EVP_MD_CTX_free(md);
	unmap_file(&m);
	EVP_PKEY_free(key);
	return ok ? 0 : -1;
}
