#include "releasekey.h"

#include "opensslrun.h"

#include <dirent.h>
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
	case RELEASEKEY_ERR_BAD_SIG: return "not a parseable minisign signature";
	case RELEASEKEY_ERR_UNKNOWN_KEY: return "signed by a key this host does not trust";
	case RELEASEKEY_ERR_VERIFY: return "signature does not match these bytes";
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

enum releasekey_error releasekey_key_id_hex(char *out, size_t out_size)
{
	unsigned char pub[ED25519_PUB_LEN];
	unsigned char id[RELEASEKEY_ID_LEN];
	size_t i;
	enum releasekey_error rc;

	if (out_size < RELEASEKEY_ID_HEX_SIZE)
		return RELEASEKEY_ERR_IO;
	if (!releasekey_is_set())
		return RELEASEKEY_ERR_NOT_SET;
	rc = public_raw(g_key_path, pub);
	if (rc != RELEASEKEY_OK)
		return rc;
	key_id(pub, id);
	for (i = 0; i < RELEASEKEY_ID_LEN; i++)
		snprintf(out + i * 2, out_size - i * 2, "%02x", id[i]);
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

/*
 * ---- verification (ADR-0279) ----
 *
 * The daemon has signed since ADR-0220 and has never verified anything;
 * every verifier was a person running stock minisign. An artifact that
 * carries its own approval changes that, because a host which did not
 * build something now has to decide whether to trust it.
 */

/*
 * The fixed 12-byte prefix of an Ed25519 SubjectPublicKeyInfo.
 *
 * public_raw() above takes a key as the last 32 bytes of a 44-byte DER
 * SPKI; this goes the other way, wrapping a raw key back into one so
 * openssl will read it. The same 44/12/32 split, written once in each
 * direction rather than as a magic offset in two places.
 */
static const unsigned char g_ed25519_spki_prefix[12] = { 0x30, 0x2a, 0x30, 0x05, 0x06, 0x03,
	                                                  0x2b, 0x65, 0x70, 0x03, 0x21, 0x00 };

/*
 * Standard base64 in, raw bytes out.
 *
 * EVP_DecodeBlock reports the PADDED length -- it decodes '=' to zero
 * bytes and counts them -- so the padding has to be subtracted here or
 * a 74-byte signature blob reads back as 75 and every length check
 * downstream is off by one. It also accepts only whole 4-character
 * groups, which is why a length that is not a multiple of four is
 * rejected before it is handed over rather than after.
 *
 * `out` must hold (strlen(in) / 4) * 3 bytes -- that PADDED length, not
 * the number of bytes you expect back. Sizing it to the expected result
 * is the mistake this paragraph exists for: a 64-byte signature needs
 * 66 bytes here, and a destination of exactly 64 makes the guard below
 * refuse the decode. It fails closed, which is the right direction and
 * is also indistinguishable from a malformed file -- so a programming
 * error presents as "nothing verifies", which is how it reached a real
 * build (#403).
 */
static int unb64(const char *in, unsigned char *out, size_t out_size)
{
	size_t len = strlen(in);
	size_t pad = 0;
	int n;

	if (len == 0 || len % 4 != 0)
		return -1;
	if (in[len - 1] == '=')
		pad++;
	if (len >= 2 && in[len - 2] == '=')
		pad++;
	if ((len / 4) * 3 > out_size)
		return -1;
	n = EVP_DecodeBlock(out, (const unsigned char *)in, (int)len);
	if (n < 0 || (size_t)n != (len / 4) * 3)
		return -1;
	return (int)((size_t)n - pad);
}

/* One Ed25519 signature check: `sig` over the whole of `in_path`. */
static int verify_raw(const unsigned char pub[ED25519_PUB_LEN], const char *in_path,
                       const unsigned char sig[ED25519_SIG_LEN], const char *scratch_stem)
{
	char der_path[PATH_MAX], sig_path[PATH_MAX];
	unsigned char der[44];
	char *argv[16];
	FILE *f;
	int rc = -1;

	snprintf(der_path, sizeof(der_path), "%s.%d.vpub.der", scratch_stem, (int)getpid());
	snprintf(sig_path, sizeof(sig_path), "%s.%d.vsig", scratch_stem, (int)getpid());

	memcpy(der, g_ed25519_spki_prefix, sizeof(g_ed25519_spki_prefix));
	memcpy(der + sizeof(g_ed25519_spki_prefix), pub, ED25519_PUB_LEN);

	f = fopen(der_path, "wb");
	if (f == NULL)
		return -1;
	if (fwrite(der, 1, sizeof(der), f) != sizeof(der)) {
		fclose(f);
		unlink(der_path);
		return -1;
	}
	fclose(f);

	f = fopen(sig_path, "wb");
	if (f == NULL) {
		unlink(der_path);
		return -1;
	}
	if (fwrite(sig, 1, ED25519_SIG_LEN, f) != ED25519_SIG_LEN) {
		fclose(f);
		unlink(der_path);
		unlink(sig_path);
		return -1;
	}
	fclose(f);

	/* -rawin for the same reason signing uses it: Ed25519 is PureEdDSA
	 * and signs the message itself, with no digest step of our own. */
	argv[0] = (char *)OPENSSL_BIN;
	argv[1] = (char *)"pkeyutl";
	argv[2] = (char *)"-verify";
	argv[3] = (char *)"-rawin";
	argv[4] = (char *)"-pubin";
	argv[5] = (char *)"-inkey";
	argv[6] = der_path;
	argv[7] = (char *)"-keyform";
	argv[8] = (char *)"DER";
	argv[9] = (char *)"-sigfile";
	argv[10] = sig_path;
	argv[11] = (char *)"-in";
	argv[12] = (char *)in_path;
	argv[13] = NULL;
	rc = pki_run_openssl(argv, NULL, 0);

	unlink(der_path);
	unlink(sig_path);
	return rc == 0 ? 0 : -1;
}

/*
 * A minisign public key file: a comment line, then base64 of
 * "Ed" || key_id[8] || public_key[32]. Exactly what
 * releasekey_public() writes, and what docs/keys/ publishes.
 */
static int read_public_key_file(const char *path, unsigned char out_id[RELEASEKEY_ID_LEN],
                                 unsigned char out_pub[ED25519_PUB_LEN])
{
	char line[512];
	unsigned char blob[64];
	FILE *f = fopen(path, "r");
	int n = -1, got = 0;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		size_t len = strlen(line);

		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (len == 0 || strncmp(line, "untrusted comment:", 18) == 0)
			continue;
		n = unb64(line, blob, sizeof(blob));
		got = 1;
		break;
	}
	fclose(f);
	if (!got)
		return -1;
	/* 2 + 8 + 32. Anything else is not a minisign Ed25519 public key,
	 * which is how a .crt or a PGP block in the same directory is
	 * skipped rather than misread -- docs/keys/ holds all three. */
	if (n != 2 + RELEASEKEY_ID_LEN + ED25519_PUB_LEN || blob[0] != 'E' || blob[1] != 'd')
		return -1;
	memcpy(out_id, blob + 2, RELEASEKEY_ID_LEN);
	memcpy(out_pub, blob + 2 + RELEASEKEY_ID_LEN, ED25519_PUB_LEN);
	return 0;
}

/* The trusted key carrying `want_id`, or -1 when no file in the
 * directory holds it. Every readable key is considered; a file that is
 * not a minisign public key is skipped, not an error. */
static int find_trusted_key(const char *dir, const unsigned char want_id[RELEASEKEY_ID_LEN],
                             unsigned char out_pub[ED25519_PUB_LEN])
{
	DIR *d = opendir(dir);
	struct dirent *ent;
	int found = -1;

	if (d == NULL)
		return -1;
	while ((ent = readdir(d)) != NULL) {
		char path[PATH_MAX];
		unsigned char id[RELEASEKEY_ID_LEN], pub[ED25519_PUB_LEN];

		if (ent->d_name[0] == '.')
			continue;
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= sizeof(path))
			continue;
		if (read_public_key_file(path, id, pub) != 0)
			continue;
		if (memcmp(id, want_id, RELEASEKEY_ID_LEN) != 0)
			continue;
		memcpy(out_pub, pub, ED25519_PUB_LEN);
		found = 0;
		break;
	}
	closedir(d);
	return found;
}

enum releasekey_error releasekey_verify_file(const char *path, const char *sig_path,
                                              const char *trusted_dir,
                                              char *out_trusted_comment, size_t out_size)
{
	char lines[4][1024];
	char comment[768];
	char gsig_in[PATH_MAX];
	unsigned char blob[128], gsig[ED25519_SIG_LEN];
	unsigned char sig[ED25519_SIG_LEN];
	unsigned char key_id_want[RELEASEKEY_ID_LEN], pub[ED25519_PUB_LEN];
	FILE *f;
	int n, i, count = 0;
	size_t clen;

	if (path == NULL || sig_path == NULL || trusted_dir == NULL)
		return RELEASEKEY_ERR_IO;

	f = fopen(sig_path, "r");
	if (f == NULL)
		return RELEASEKEY_ERR_IO;
	while (count < 4 && fgets(lines[count], sizeof(lines[0]), f) != NULL) {
		size_t len = strlen(lines[count]);

		while (len > 0 && (lines[count][len - 1] == '\n' || lines[count][len - 1] == '\r'))
			lines[count][--len] = '\0';
		count++;
	}
	fclose(f);
	if (count < 4)
		return RELEASEKEY_ERR_BAD_SIG;

	if (strncmp(lines[0], "untrusted comment:", 18) != 0)
		return RELEASEKEY_ERR_BAD_SIG;
	if (strncmp(lines[2], "trusted comment: ", 17) != 0)
		return RELEASEKEY_ERR_BAD_SIG;

	/* "Ed" only. "ED" is minisign's prehashed BLAKE2b-512 mode, which
	 * this daemon cannot compute and must therefore refuse rather than
	 * verify the wrong way round -- releasekey_sign_file() writes "Ed"
	 * for the same reason. */
	n = unb64(lines[1], blob, sizeof(blob));
	if (n != 2 + RELEASEKEY_ID_LEN + ED25519_SIG_LEN || blob[0] != 'E' || blob[1] != 'd')
		return RELEASEKEY_ERR_BAD_SIG;
	memcpy(key_id_want, blob + 2, RELEASEKEY_ID_LEN);
	memcpy(sig, blob + 2 + RELEASEKEY_ID_LEN, ED25519_SIG_LEN);

	/*
	 * Decoded through a larger buffer, not straight into gsig[64].
	 *
	 * EVP_DecodeBlock writes the PADDED length -- 66 bytes for the 88
	 * base64 characters a 64-byte signature becomes -- so a destination
	 * sized to the signature is two bytes short and unb64()'s own guard
	 * refuses the whole decode. That failed closed, which is the right
	 * direction, but it failed closed for EVERYTHING: measured on
	 * 192.168.15.95, every verification returned "not a parseable
	 * minisign signature", including one this daemon had just written.
	 */
	{
		unsigned char gbuf[96];

		n = unb64(lines[3], gbuf, sizeof(gbuf));
		if (n != ED25519_SIG_LEN)
			return RELEASEKEY_ERR_BAD_SIG;
		memcpy(gsig, gbuf, ED25519_SIG_LEN);
	}

	clen = strlen(lines[2] + 17);
	if (clen >= sizeof(comment))
		return RELEASEKEY_ERR_BAD_SIG;
	memcpy(comment, lines[2] + 17, clen + 1);

	if (find_trusted_key(trusted_dir, key_id_want, pub) != 0)
		return RELEASEKEY_ERR_UNKNOWN_KEY;

	if (verify_raw(pub, path, sig, sig_path) != 0)
		return RELEASEKEY_ERR_VERIFY;

	/*
	 * The global signature, over signature || trusted_comment. Omitting
	 * this is the usual mistake and it is not cosmetic: without it the
	 * comment naming which package and revision these bytes belong to
	 * is editable, so a genuine artifact can be relabelled as another
	 * one and still verify.
	 */
	snprintf(gsig_in, sizeof(gsig_in), "%s.%d.vgin", sig_path, (int)getpid());
	f = fopen(gsig_in, "wb");
	if (f == NULL)
		return RELEASEKEY_ERR_IO;
	if (fwrite(sig, 1, ED25519_SIG_LEN, f) != ED25519_SIG_LEN ||
	    (clen > 0 && fwrite(comment, 1, clen, f) != clen)) {
		fclose(f);
		unlink(gsig_in);
		return RELEASEKEY_ERR_IO;
	}
	fclose(f);
	i = verify_raw(pub, gsig_in, gsig, gsig_in);
	unlink(gsig_in);
	if (i != 0)
		return RELEASEKEY_ERR_VERIFY;

	/* Only now: a trusted comment from a signature that did not verify
	 * is an attacker's text, and handing one back is how a caller comes
	 * to believe it. */
	if (out_trusted_comment != NULL) {
		if (clen >= out_size)
			return RELEASEKEY_ERR_IO;
		memcpy(out_trusted_comment, comment, clen + 1);
	}
	return RELEASEKEY_OK;
}
