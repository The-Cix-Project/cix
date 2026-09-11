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
	RELEASEKEY_ERR_SIGN,        /* openssl refused to sign */
	RELEASEKEY_ERR_BAD_SIG,     /* the signature file is not parseable minisign */
	RELEASEKEY_ERR_UNKNOWN_KEY, /* no trusted key carries the signature's key id */
	RELEASEKEY_ERR_VERIFY       /* parsed and attributed, but does not match these bytes */
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
 * The same key id, hex, for display. This is what minisign prints when
 * a signature does not match the public key an operator holds, so it is
 * the one field that makes a mismatch diagnosable -- worth reporting
 * separately from the public key blob it is already embedded in.
 * Needs RELEASEKEY_ID_HEX_SIZE bytes.
 */
#define RELEASEKEY_ID_HEX_SIZE 17
enum releasekey_error releasekey_key_id_hex(char *out, size_t out_size);

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

/*
 * Verifies `sig_path` over `path` against the public keys in
 * `trusted_dir`, and hands back the signature's trusted comment.
 *
 * The counterpart to releasekey_sign_file(), added for ADR-0279: an
 * artifact's approval travels as a signature beside it, so a host that
 * did not build something still has to decide whether to trust it.
 * Until this existed the daemon could sign and never check -- every
 * verifier was a person with stock minisign on a laptop.
 *
 * Both signatures are checked, and the second is the one with teeth:
 * the global signature covers `signature || trusted_comment`, so a
 * comment naming which package and revision these bytes are cannot be
 * edited without invalidating everything. Checking only the first would
 * accept a genuine artifact relabelled as a different one, which is
 * precisely the attack ADR-0279's revision binding exists to stop.
 *
 * `trusted_dir` holds minisign-format public keys, one per file, the
 * shape docs/keys/ already publishes. EVERY key there is trusted,
 * retired ones included -- a retired key is kept so the artifacts it
 * signed stay verifiable, and dropping it would un-approve history. A
 * signature whose key id matches none of them is RELEASEKEY_ERR_
 * UNKNOWN_KEY, a hard failure: an unknown signer is never a reason to
 * fall through to trusting the bytes. An empty or absent directory
 * verifies nothing, and says so, rather than accepting anything.
 *
 * out_trusted_comment receives the comment only when the return is
 * RELEASEKEY_OK -- there is no such thing as a trusted comment from a
 * signature that did not verify, and handing one back on failure is how
 * a caller ends up reading an attacker's text.
 */
enum releasekey_error releasekey_verify_file(const char *path, const char *sig_path,
                                              const char *trusted_dir,
                                              char *out_trusted_comment, size_t out_size);

/*
 * Copies every minisign public key in `src_dir` into `dst_dir`,
 * returning how many are now trusted, or -1 if dst_dir cannot be made.
 *
 * This is how a host learns which keys to trust (ADR-0279): the sync
 * tarball is the whole repository, so docs/keys/ already arrives on
 * every sync and was discarded. The owner's decision was that the key
 * comes from git via sync, which makes this the one place it enters.
 *
 * Only files that PARSE as a minisign public key are taken. docs/keys/
 * also holds an X.509 certificate and a PGP block, and a trust store
 * that accumulated those would be a directory of things nobody checks
 * the meaning of. Deciding by parse rather than by filename means a key
 * is trusted because it is one.
 *
 * Merge, never replace. A retired key is kept precisely so the
 * artifacts it signed stay verifiable, and docs/keys/ is append-only
 * for that reason -- so a key absent from a sync is not evidence it
 * should stop being trusted, and dropping it would un-approve history.
 * Revoking one is therefore deliberately NOT expressible here; it would
 * need a mechanism that says "revoked" rather than one that says
 * "absent", and inferring the first from the second is how a mirror
 * outage becomes a fleet-wide trust failure.
 */
int releasekey_trust_adopt(const char *src_dir, const char *dst_dir);

const char *releasekey_strerror(enum releasekey_error e);

#endif /* RELEASEKEY_H */
