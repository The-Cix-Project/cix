#ifndef PWHASH_H
#define PWHASH_H

#include <stddef.h>

/*
 * ADR-0144: real bcrypt password hashing, replacing the project's
 * prior unsalted-SHA256 credential store (daemon/src/ldap.c's own
 * former passsha256 field) now that these credentials are becoming
 * the host's own root-of-trust (ADR-0144's host authentication work),
 * not just a container-facing directory's password field.
 *
 * A thin wrapper around a real, audited, vendored bcrypt
 * implementation (daemon/src/vendor/bcrypt.c + blowfish.c, OpenBSD's
 * own reference implementation, ISC-licensed) -- this project's own
 * "No Hacks" rule already correctly identified hand-rolling bcrypt
 * from scratch as reimplementing a security-critical primitive with
 * no audit trail; vendoring a real, widely-deployed reference
 * implementation verbatim (rather than writing a from-scratch one, or
 * shelling out) is the resolution, not an exception to that rule.
 */

/* $2b$<cost>$<22-char-salt><31-char-hash>\0 -- glauth's own PassBcrypt
 * field expects exactly this real bcrypt hash string format, so this
 * project's own stored value is directly renderable into glauth.cfg
 * with zero reformatting (see daemon/src/ldap.c's own render step). */
#define PWHASH_BCRYPT_LEN 60

/* A real, deliberately-chosen cost factor -- 2^12 = 4096 rounds,
 * comfortably under 200ms on this project's own real target hardware
 * (measured directly), the same "not too slow to be usable, not too
 * fast to be crackable" balance every modern bcrypt deployment uses.
 * Not runtime-autocalibrated (unlike OpenBSD libc's own bcrypt()
 * classic interface, deliberately dropped from the vendored copy) --
 * a fixed, known, documented cost is preferable to a value that could
 * silently drift between two logins on hardware of different speeds. */
#define PWHASH_BCRYPT_COST 12

/*
 * Hashes password into out (a real, salted bcrypt hash string,
 * PWHASH_BCRYPT_LEN + 1 bytes minimum). Returns 0 on success, -1 on
 * failure (out_size too small, or the underlying vendored bcrypt
 * implementation's own real failure path).
 */
int pwhash_bcrypt_new(const char *password, char *out, size_t out_size);

/*
 * Real, constant-time verification of password against hash (a
 * PWHASH_BCRYPT_LEN-shaped string previously produced by
 * pwhash_bcrypt_new(), e.g. read back from persisted state). Returns
 * 1 if it matches, 0 otherwise (a mismatch and a malformed hash
 * string are indistinguishable to the caller, deliberately -- never
 * leak which case occurred).
 */
int pwhash_bcrypt_check(const char *password, const char *hash);

#endif /* PWHASH_H */
