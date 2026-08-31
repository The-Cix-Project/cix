#include "releasekey.h"

#include "opensslrun.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#define OPENSSL_BIN "/usr/bin/openssl"
#define ED25519_PUB_LEN 32
#define ED25519_SIG_LEN 64
#define RELEASEKEY_ID_LEN 8

static char g_key_path[PATH_MAX];

void releasekey_init(const char *keys_dir)
{
	snprintf(g_key_path, sizeof(g_key_path), "%s/cix-release.key", keys_dir);
}

int releasekey_is_set(void)
{
	struct stat st;

	return g_key_path[0] != '\0' && stat(g_key_path, &st) == 0 && S_ISREG(st.st_mode);
}

const char *releasekey_strerror(enum releasekey_error e)
{
	switch (e) {
	case RELEASEKEY_OK: return "ok";
	case RELEASEKEY_ERR_BAD_KEY: return "not a parseable Ed25519 private key";
	case RELEASEKEY_ERR_NOT_SET: return "no release signing key is installed";
	case RELEASEKEY_ERR_IO: return "could not read or write a key file";
	case RELEASEKEY_ERR_SIGN: return "signing failed";
	}
	return "unknown";
}

/*
 * The raw 32-byte public key.
 *
 * Taken as the tail of the DER SubjectPublicKeyInfo rather than parsed:
 * an Ed25519 SPKI is a fixed 44 bytes whose last 32 are the key itself,
 * and openssl refuses to emit one at all for any other algorithm -- so
 * a wrong key type fails here, before anything is stored, rather than
 * at signing time on a real release.
 */
static enum releasekey_error public_raw(const char *key_path, unsigned char out[ED25519_PUB_LEN])
{
	char der_path[PATH_MAX];
	char *argv[10];
	FILE *f;
	unsigned char der[256];
	size_t n;
	enum releasekey_error rc = RELEASEKEY_ERR_BAD_KEY;

	snprintf(der_path, sizeof(der_path), "%s.pub.der", key_path);
	argv[0] = (char *)OPENSSL_BIN;
	argv[1] = (char *)"pkey";
	argv[2] = (char *)"-in";
	argv[3] = (char *)key_path;
	argv[4] = (char *)"-pubout";
	argv[5] = (char *)"-outform";
	argv[6] = (char *)"DER";
	argv[7] = (char *)"-out";
	argv[8] = der_path;
	argv[9] = NULL;
	if (pki_run_openssl(argv, NULL, 0) != 0) {
		unlink(der_path);
		return RELEASEKEY_ERR_BAD_KEY;
	}
	f = fopen(der_path, "rb");
	if (f == NULL) {
		unlink(der_path);
		return RELEASEKEY_ERR_IO;
	}
	n = fread(der, 1, sizeof(der), f);
	fclose(f);
	unlink(der_path);
	/* 44 bytes exactly: 12 of algorithm identifier, 32 of key. Anything
	 * else is not Ed25519 and must not be treated as one. */
	if (n == 44) {
		memcpy(out, der + 12, ED25519_PUB_LEN);
		rc = RELEASEKEY_OK;
	}
	return rc;
}

static void key_id(const unsigned char pub[ED25519_PUB_LEN], unsigned char out[RELEASEKEY_ID_LEN])
{
	unsigned char digest[SHA256_DIGEST_LENGTH];

	SHA256(pub, ED25519_PUB_LEN, digest);
	memcpy(out, digest, RELEASEKEY_ID_LEN);
}

/* Standard base64, no wrapping -- minisign expects one line. */
static int b64(const unsigned char *in, size_t in_len, char *out, size_t out_size)
{
	int n;

	if (out_size < ((in_len + 2) / 3) * 4 + 1)
		return -1;
	n = EVP_EncodeBlock((unsigned char *)out, in, (int)in_len);
	return n > 0 ? 0 : -1;
}

enum releasekey_error releasekey_set(const char *pem, size_t pem_len)
{
	char tmp[PATH_MAX];
	unsigned char pub[ED25519_PUB_LEN];
	enum releasekey_error rc;
	int fd;

	if (g_key_path[0] == '\0')
		return RELEASEKEY_ERR_IO;
	snprintf(tmp, sizeof(tmp), "%s.new", g_key_path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return RELEASEKEY_ERR_IO;
	if (write(fd, pem, pem_len) != (ssize_t)pem_len) {
		close(fd);
		unlink(tmp);
		return RELEASEKEY_ERR_IO;
	}
	close(fd);

	/* Validate before installing: a key that cannot produce a public
	 * half is refused here rather than at signing time. */
	rc = public_raw(tmp, pub);
	if (rc != RELEASEKEY_OK) {
		unlink(tmp);
		return rc;
	}
	if (rename(tmp, g_key_path) != 0) {
		unlink(tmp);
		return RELEASEKEY_ERR_IO;
	}
	return RELEASEKEY_OK;
}

enum releasekey_error releasekey_clear(void)
{
	if (g_key_path[0] == '\0')
		return RELEASEKEY_ERR_IO;
	unlink(g_key_path);
	return RELEASEKEY_OK;
}

enum releasekey_error releasekey_public(char *out, size_t out_size)
{
	unsigned char pub[ED25519_PUB_LEN];
	unsigned char blob[2 + RELEASEKEY_ID_LEN + ED25519_PUB_LEN];
	char encoded[128];
	enum releasekey_error rc;

	if (!releasekey_is_set())
		return RELEASEKEY_ERR_NOT_SET;
	rc = public_raw(g_key_path, pub);
	if (rc != RELEASEKEY_OK)
		return rc;
	blob[0] = 'E';
	blob[1] = 'd';
	key_id(pub, blob + 2);
	memcpy(blob + 2 + RELEASEKEY_ID_LEN, pub, ED25519_PUB_LEN);
	if (b64(blob, sizeof(blob), encoded, sizeof(encoded)) != 0)
		return RELEASEKEY_ERR_IO;
	if ((size_t)snprintf(out, out_size, "untrusted comment: cix release signing key\n%s\n",
	                      encoded) >= out_size)
		return RELEASEKEY_ERR_IO;
	return RELEASEKEY_OK;
}

/* openssl pkeyutl -sign -rawin: Ed25519 is PureEdDSA, so this is a
 * signature over the file's own bytes with no digest step. */
static enum releasekey_error sign_raw(const char *in_path, unsigned char out[ED25519_SIG_LEN])
{
	char sig_path[PATH_MAX];
	char *argv[12];
	FILE *f;
	size_t n;

	snprintf(sig_path, sizeof(sig_path), "%s.rawsig", in_path);
	argv[0] = (char *)OPENSSL_BIN;
	argv[1] = (char *)"pkeyutl";
	argv[2] = (char *)"-sign";
	argv[3] = (char *)"-rawin";
	argv[4] = (char *)"-inkey";
	argv[5] = g_key_path;
	argv[6] = (char *)"-in";
	argv[7] = (char *)in_path;
	argv[8] = (char *)"-out";
	argv[9] = sig_path;
	argv[10] = NULL;
	if (pki_run_openssl(argv, NULL, 0) != 0) {
		unlink(sig_path);
		return RELEASEKEY_ERR_SIGN;
	}
	f = fopen(sig_path, "rb");
	if (f == NULL) {
		unlink(sig_path);
		return RELEASEKEY_ERR_IO;
	}
	n = fread(out, 1, ED25519_SIG_LEN, f);
	fclose(f);
	unlink(sig_path);
	return n == ED25519_SIG_LEN ? RELEASEKEY_OK : RELEASEKEY_ERR_SIGN;
}

enum releasekey_error releasekey_sign_file(const char *path, const char *sig_path,
                                            const char *trusted_comment)
{
	unsigned char pub[ED25519_PUB_LEN];
	unsigned char sig[ED25519_SIG_LEN], gsig[ED25519_SIG_LEN];
	unsigned char sig_blob[2 + RELEASEKEY_ID_LEN + ED25519_SIG_LEN];
	char sig_b64[256], gsig_b64[256];
	char gsig_in[PATH_MAX];
	enum releasekey_error rc;
	FILE *f;

	if (!releasekey_is_set())
		return RELEASEKEY_ERR_NOT_SET;
	rc = public_raw(g_key_path, pub);
	if (rc != RELEASEKEY_OK)
		return rc;
	rc = sign_raw(path, sig);
	if (rc != RELEASEKEY_OK)
		return rc;

	/*
	 * The global signature covers signature || trusted_comment, so the
	 * comment cannot be edited after the fact. Written to a file rather
	 * than piped because openssl's -rawin reads a file, and the daemon
	 * shells out rather than linking the EVP signing API.
	 */
	snprintf(gsig_in, sizeof(gsig_in), "%s.gsigin", sig_path);
	f = fopen(gsig_in, "wb");
	if (f == NULL)
		return RELEASEKEY_ERR_IO;
	if (fwrite(sig, 1, ED25519_SIG_LEN, f) != ED25519_SIG_LEN ||
	    fwrite(trusted_comment, 1, strlen(trusted_comment), f) != strlen(trusted_comment)) {
		fclose(f);
		unlink(gsig_in);
		return RELEASEKEY_ERR_IO;
	}
	fclose(f);
	rc = sign_raw(gsig_in, gsig);
	unlink(gsig_in);
	if (rc != RELEASEKEY_OK)
		return rc;

	/* "Ed" here, not "ED": legacy mode, signature over the file. */
	sig_blob[0] = 'E';
	sig_blob[1] = 'd';
	key_id(pub, sig_blob + 2);
	memcpy(sig_blob + 2 + RELEASEKEY_ID_LEN, sig, ED25519_SIG_LEN);
	if (b64(sig_blob, sizeof(sig_blob), sig_b64, sizeof(sig_b64)) != 0 ||
	    b64(gsig, ED25519_SIG_LEN, gsig_b64, sizeof(gsig_b64)) != 0)
		return RELEASEKEY_ERR_IO;

	f = fopen(sig_path, "w");
	if (f == NULL)
		return RELEASEKEY_ERR_IO;
	fprintf(f, "untrusted comment: signature from the cix release key\n%s\n", sig_b64);
	fprintf(f, "trusted comment: %s\n%s\n", trusted_comment, gsig_b64);
	fclose(f);
	return RELEASEKEY_OK;
}
