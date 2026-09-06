#ifndef PGPVERIFY_H
#define PGPVERIFY_H

#include <stddef.h>

/*
 * OpenPGP clearsigned-document verification, for upstream checksum
 * files that carry their own signature.
 *
 * WHY THIS EXISTS (ADR-0193's open question, and issue #65's real one).
 *
 * A kernel channel can resolve "stable" to a version, but turning that
 * into a recipe means writing a pkg_sha256. If the daemon computes that
 * checksum by downloading the tarball itself, the field silently stops
 * meaning "an operator verified these bytes out of band" and starts
 * meaning "whatever arrived first" -- the same field, the same syntax,
 * a materially weaker claim, and nothing anywhere says so. ADR-0193
 * refused that downgrade and stopped at reporting.
 *
 * kernel.org publishes sha256sums.asc per release directory: its own
 * checksum for every tarball, PGP-clearsigned. Quoting THAT checksum is
 * neither trust-on-first-use nor a false claim of out-of-band review --
 * it is exactly what an operator does by hand, and it is checkable by
 * anyone afterwards. Verifying the signature is what makes it so;
 * without it, a checksum fetched from the same origin as the tarball
 * defends against nothing that origin could not also forge.
 *
 * WHAT THIS DOES AND DOES NOT IMPLEMENT.
 *
 * It parses OpenPGP packet framing and performs RFC 4880 section 7
 * cleartext canonicalisation. It does NOT implement cryptography: the
 * hash and the RSA verification are OpenSSL's, which cixd already
 * links. That split is deliberate -- packet framing is bounded, and
 * a malformed packet fails verification rather than leaking anything,
 * whereas a hand-rolled signature primitive is the one thing this
 * project should never own.
 *
 * KEY TRUST IS THE CALLER'S PROBLEM, ON PURPOSE.
 *
 * Nothing here fetches, stores or blesses a key. The caller supplies
 * the armored public key AND the fingerprint it expects, and a
 * mismatch is a failure like any other. Pinning a fingerprint is an
 * out-of-band human act; a verifier that acquired its own trust root
 * over the network would rebuild trust-on-first-use one layer up,
 * which is the exact thing this file exists to avoid.
 *
 * Supported: v4 signatures, RSA (algorithm 1), SHA-256/384/512.
 * Anything else is refused by name rather than approximated.
 */

enum pgp_verify_result {
	PGP_VERIFY_OK = 0,
	PGP_VERIFY_BAD_SIGNATURE,  /* well-formed, and it does not verify */
	PGP_VERIFY_WRONG_KEY,      /* verified shape, but not the pinned fingerprint */
	PGP_VERIFY_MALFORMED,      /* not a clearsigned document we can parse */
	PGP_VERIFY_UNSUPPORTED,    /* a real OpenPGP construct this does not implement */
	PGP_VERIFY_INTERNAL        /* allocation or OpenSSL failure */
};

const char *pgp_verify_result_name(enum pgp_verify_result r);

/*
 * Verifies a clearsigned document.
 *
 * doc/doc_len          the whole "-----BEGIN PGP SIGNED MESSAGE-----"
 *                      text, exactly as fetched.
 * pubkey_armored       an ASCII-armored public key block.
 * expect_fingerprint   hex, case-insensitive, no spaces (40 chars for a
 *                      v4 key). Required -- passing NULL is refused,
 *                      because "verify against whatever key you were
 *                      handed" is not verification.
 * out_text/out_len     on OK, the canonical signed text, malloc'd and
 *                      NUL-terminated. The CALLER frees it. Never set
 *                      on failure, so a caller cannot accidentally use
 *                      unverified bytes.
 * err/err_size         a human-readable reason, always written.
 *
 * The signed text is returned only on success, deliberately: there is
 * no API here for reading the payload of a document that did not
 * verify.
 */
enum pgp_verify_result pgp_clearsign_verify(const char *doc, size_t doc_len,
                                             const char *pubkey_armored, size_t pubkey_len,
                                             const char *expect_fingerprint, char **out_text,
                                             size_t *out_len, char *err, size_t err_size);

/*
 * Pulls "<sha256>  <name>" out of an already-VERIFIED checksum listing.
 * Takes the verified text rather than the document precisely so it
 * cannot be called on unverified input by mistake.
 */
int pgp_checksum_lookup(const char *verified_text, const char *filename, char *out_sha256,
                        size_t out_size);

#endif /* PGPVERIFY_H */
