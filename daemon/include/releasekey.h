#ifndef RELEASEKEY_H
#define RELEASEKEY_H

#include <stddef.h>

/*
 * The release-signing key, and minisign signatures over published
 * artifacts (ADR-0220).
 *
 * Deliberately NOT the Secure Boot key in signingkeys.h. That one is
 * RSA, because UEFI requires it, and it answers "may this firmware boot
 * this image?". This one is Ed25519, because minisign requires it, and
 * answers "did Cix publish these bytes?". Sharing a key between those
 * two questions would mean anyone able to sign a download is also able
 * to sign a bootloader.
 *
 * Same transport discipline as ADR-0212: the private key goes in over
 * REST and nothing -- no GET, no error message, no log line -- ever
 * brings it back out. Only the public key comes back, in minisign's own
 * format, so an operator can hand it to a verifier.
 *
 * Signing shells out to openssl, which this daemon already depends on
 * at runtime; the format work is here.
 */

enum releasekey_error {
	RELEASEKEY_OK = 0,
	RELEASEKEY_ERR_BAD_KEY,     /* not a parseable Ed25519 private key */
	RELEASEKEY_ERR_NOT_SET,     /* no key installed */
	RELEASEKEY_ERR_IO,          /* could not read/write a file */
	RELEASEKEY_ERR_SIGN         /* openssl refused to sign */
};

/* <data-dir>/keys/cix-release.key, alongside the Secure Boot pair. */
void releasekey_init(const char *keys_dir);

/* Stores a PEM Ed25519 private key, replacing any existing one. The key
 * is validated by deriving its public half: anything openssl cannot
 * read as Ed25519 is refused before it is written, so a bad paste
 * cannot be discovered later at signing time. */
enum releasekey_error releasekey_set(const char *pem, size_t pem_len);

/* Removes the key. Not an error when none is installed. */
enum releasekey_error releasekey_clear(void);

int releasekey_is_set(void);

/*
 * The minisign-format public key file: a comment line, then base64 of
 * "Ed" || key_id[8] || public_key[32].
 *
 * The key id is the first 8 bytes of SHA-256(public key) rather than
 * minisign's random one, because we hold a bare PEM key and have
 * nowhere to keep a random id. Deriving it means the public key file
 * and every signature agree by construction -- there is no stored id to
 * lose or let drift.
 */
enum releasekey_error releasekey_public(char *out, size_t out_size);

/*
 * Writes a minisign signature for `path` to `sig_path`.
 *
 * Legacy "Ed" mode -- Ed25519 over the file itself, not over a hash of
 * it. minisign also defines a prehashed "ED" mode using BLAKE2b-512;
 * this avoids it deliberately, because OpenSSL's Ed25519 is PureEdDSA
 * and signs the message directly, so the daemon needs no BLAKE2b of its
 * own. Stock minisign verifies both.
 *
 * Two signatures are written, and the second is not optional: one over
 * the file, and a "global" one over signature || trusted_comment.
 * Omitting the global signature is the usual mistake, and upstream
 * minisign rejects the result.
 */
enum releasekey_error releasekey_sign_file(const char *path, const char *sig_path,
                                            const char *trusted_comment);

const char *releasekey_strerror(enum releasekey_error e);

#endif /* RELEASEKEY_H */
