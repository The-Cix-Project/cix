#ifndef CIX_PKICRYPTO_H
#define CIX_PKICRYPTO_H

#include <stddef.h>

/*
 * cix#351: the cryptographic work cixd used to fork /usr/bin/openssl
 * for, done in the libcrypto it already links. One function per
 * operation the CLI performed, each reproducing what the CLI produced
 * (measured on 192.168.15.95, 2026-10-01, OpenSSL 3.0.20, by
 * probe-pki-cli@1):
 *
 *   certificates  v3, sha256WithRSAEncryption, notBefore now, notAfter
 *                 now + days, a random 159-bit serial (-CAcreateserial)
 *   root          SKID, AKID(keyid), basicConstraints critical CA:TRUE
 *   issued        the requested extensions in order, then SKID,
 *                 AKID(keyid) (x509 -req -copy_extensions copy)
 *   field text    the x509 -subject/-serial/-startdate/-enddate/
 *                 -fingerprint formats, which the API returns
 *   export        openssl enc -aes-256-cbc -pbkdf2 -iter 600000
 *                 -md sha256 -salt -a -A, so existing bundles import
 *
 * Every function returns 0 on success and -1 on failure, with a
 * one-line reason in err when err is not NULL.
 */

#define PKICRYPTO_ED25519_PUB_LEN 32
#define PKICRYPTO_ED25519_SIG_LEN 64

/* A new RSA-2048 private key, written as unencrypted PKCS#8 PEM with
 * mode 0600 -- what `genpkey -algorithm RSA -pkeyopt
 * rsa_keygen_bits:2048` wrote. */
int pkicrypto_rsa_key_create(const char *key_path, char *err, size_t err_size);

/* A self-signed root CA for key_path, subject CN=common_name, valid
 * for days -- what `req -x509 -new -subj /CN=...` wrote. */
int pkicrypto_ca_create(const char *key_path, const char *common_name, int days,
                        const char *cert_path, char *err, size_t err_size);

/*
 * A certificate for subject_key_path, subject CN=common_name, issued by
 * issuer_cert_path/issuer_key_path. ext[] holds ext_count extensions in
 * the `-addext` form, "name=value" ("basicConstraints=critical,CA:TRUE,
 * pathlen:0", "subjectAltName=DNS:a,IP:10.0.0.1"), parsed by the same
 * libcrypto routine -addext used.
 */
int pkicrypto_cert_issue(const char *issuer_cert_path, const char *issuer_key_path,
                         const char *subject_key_path, const char *common_name,
                         const char *const *ext, int ext_count, int days,
                         const char *cert_path, char *err, size_t err_size);

/* A certificate's fields as the CLI printed them, without the
 * "subject=" style prefixes: subject "CN = name", serial uppercase hex,
 * dates "Sep  7 16:47:25 2026 GMT", and the SHA-1 (the CLI's default) and
 * SHA-256 fingerprints as "AA:BB:...". */
struct pkicrypto_cert_fields {
	char subject[256];
	char serial[96];
	char not_before[40];
	char not_after[40];
	char sha1_fingerprint[64];
	char sha256_fingerprint[100];
};

int pkicrypto_cert_fields(const char *cert_path, struct pkicrypto_cert_fields *out, char *err,
                          size_t err_size);

/*
 * ADR-0350: what an operator's trust anchor is, read from the PEM they
 * supplied. The PEM must be exactly one certificate and nothing else --
 * a private key, or a second block of any kind, is refused -- and the
 * certificate must carry basicConstraints CA:TRUE. fingerprint is the
 * SHA-256 of the DER as 64 lowercase hex characters, with no
 * separators; name is the subject's commonName, or the whole subject
 * when it has none; not_after is unix seconds; root is 1 for a self-signed
 * certificate; expired is 1 when not_after has passed, which the caller
 * decides about (a new anchor is refused, a stored one stays listed).
 * Returns 0, or -1 with the reason in err.
 */
struct pkicrypto_anchor {
	char fingerprint[65];
	char name[256];
	char subject[256];
	char issuer[256];
	long long not_after;
	int root;
	int expired;
};

int pkicrypto_anchor_inspect(const char *pem, size_t pem_len, struct pkicrypto_anchor *out,
                             char *err, size_t err_size);

/* 1 if the private key at key_path is the one cert_path certifies,
 * 0 if not, -1 if either cannot be read. */
int pkicrypto_key_matches_cert(const char *key_path, const char *cert_path, char *err,
                               size_t err_size);

/* The certificate at pem_path, written as DER to der_path (mode 0644). */
int pkicrypto_cert_pem_to_der(const char *pem_path, const char *der_path, char *err,
                              size_t err_size);

/* The export bundle cipher. Encrypt: in -> one base64 line, malloc'd
 * and NUL-terminated. Decrypt: such a line (whitespace ignored) ->
 * plaintext, malloc'd, with a NUL after out_len bytes. */
int pkicrypto_export_encrypt(const char *passphrase, const unsigned char *in, size_t in_len,
                             char **out_b64, char *err, size_t err_size);
int pkicrypto_export_decrypt(const char *passphrase, const char *b64, size_t b64_len,
                             unsigned char **out, size_t *out_len, char *err, size_t err_size);

/* The export cipher's key and IV for a given 8-byte salt, exposed so
 * test_pkicrypto can check them against the CLI's own `enc -P`. */
int pkicrypto_export_derive(const char *passphrase, const unsigned char salt[8],
                            unsigned char key[32], unsigned char iv[16]);

/* Ed25519: the raw public key of the PEM private key at key_path
 * (fails for any other key type); a signature over the whole of the
 * file at path; and a check of one. Verify returns 0 only for a good
 * signature. */
int pkicrypto_ed25519_public_raw(const char *key_path,
                                 unsigned char out[PKICRYPTO_ED25519_PUB_LEN], char *err,
                                 size_t err_size);
int pkicrypto_ed25519_sign_file(const char *key_path, const char *path,
                                unsigned char out[PKICRYPTO_ED25519_SIG_LEN], char *err,
                                size_t err_size);
int pkicrypto_ed25519_verify_file(const unsigned char pub[PKICRYPTO_ED25519_PUB_LEN],
                                  const char *path,
                                  const unsigned char sig[PKICRYPTO_ED25519_SIG_LEN]);


/* The sha256 of len bytes at data, as 64 lowercase hex digits and a NUL
 * in out (out_size >= 65). For a digest of something already in memory
 * -- a session token (#562) -- where writing it to a file to reuse
 * pkg_run_capture_sha256() would put the secret on disk. */
int pkicrypto_sha256_hex(const void *data, size_t len, char *out, size_t out_size);

#endif
