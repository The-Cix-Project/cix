/*
 * cix#351: daemon/src/pkicrypto.c, the in-process replacement for every
 * `openssl` command cixd used to fork.
 *
 * The replacement has to produce what the CLI produced, so most of what
 * is checked here is a known answer the CLI itself gave on a Cix host
 * (192.168.15.95, 2026-10-01, OpenSSL 3.0.20, probe-pki-cli@1):
 *
 *   - the live root CA's fields, in the exact text GET /v1/pki/ca
 *     returns -- the API surface the field reader feeds;
 *   - `openssl enc` ciphertext that must decrypt, and the key/IV the CLI
 *     derived for a fixed salt -- every export bundle depends on these;
 *   - an Ed25519 key with the CLI's raw public key and signature --
 *     Ed25519 is deterministic, so the signature must match byte for
 *     byte, and release key ids hang off the public key.
 *
 * The rest checks new certificates against the profile the CLI's
 * output was measured to have: the extensions present, in the CLI's
 * order, with its critical flags; v3; sha256WithRSA; a validity of
 * exactly the days asked for; a UTF8String CN; and a chain that
 * verifies.
 */
#include "pkicrypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

static int g_failures;

#define CHECK(cond, ...)                                                                            \
	do {                                                                                        \
		if (!(cond)) {                                                                      \
			printf("  FAIL: ");                                                         \
			printf(__VA_ARGS__);                                                        \
			printf("\n");                                                               \
			g_failures++;                                                               \
		}                                                                                   \
	} while (0)

/* GET /v1/pki/ca on 192.168.15.95, 2026-10-01. */
static const char LIVE_ROOT_PEM[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIDFTCCAf2gAwIBAgIURlmBVW579wPXVqQ7vpCuuFJBRJswDQYJKoZIhvcNAQEL\n"
	"BQAwGjEYMBYGA1UEAwwPQ2l4IFBsYXRmb3JtIENBMB4XDTI2MDkwNzE2NDcyNVoX\n"
	"DTM2MDkwNDE2NDcyNVowGjEYMBYGA1UEAwwPQ2l4IFBsYXRmb3JtIENBMIIBIjAN\n"
	"BgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4snBTXuRblq6FfvaqPxsnc/kWsUA\n"
	"JyvM0qxKpV0APgUIwuv5gWw8Y4SvF8nP+jqdntzuJZvkDfy2vJQTVhnbj/pyCBWm\n"
	"mlyIyMOAgtZKuemhrMq5b6qMXGu4eO3v2G7sHMbsRh/CnIoLjyb34NSi5Cm7sGMe\n"
	"BGNL2oWH3cknluITZmO85aD0y9SHmnobOWEN6/noDopgaGmI7UUqRTPxf2R4XXch\n"
	"HvMPXzN4o2SosdAm4KWlpt/7wcmeCr2ei9NsloJKBuKdg9w6dmzIW//SrcvyrmJA\n"
	"T7LyqL1IdTtvxINaTzYIftRrSwXhea/3v9MohRyCfbNQtsp/Rxlmr57tGQIDAQAB\n"
	"o1MwUTAdBgNVHQ4EFgQUDOFHPBtr/rJQn2TO7CrUSpbncoQwHwYDVR0jBBgwFoAU\n"
	"DOFHPBtr/rJQn2TO7CrUSpbncoQwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0B\n"
	"AQsFAAOCAQEABV/cZn+h5jL84A5hJN9+HnOUk1XxG8o0+sHM1NRQtCnx4SZdzX4i\n"
	"WCacw5nXzuD46e3MuMCQVVe51VeC6nY0+cxseB9nDJSWW9vJdsX5uO8w6NvQWhHQ\n"
	"XdFZw9c04TUl4i5uEGlWr/J4D3oe6qbWTzsOLIgssuwuimHOqFmzBzh+qAqkUqPz\n"
	"rQwx4hs1g6pwKLJHN4apx8TpGmlXSxSOAyP4vjB5WvZ6l7Aa8p8Y7+vRP2owxVxs\n"
	"ocxjgWqfC1fUU6SropT51qMyZQ1aKRKCgSLhsvesSQyOZ6sNRREdXSDWykOWaOQA\n"
	"HLV91zSRbaTWMSHFUoMwyPv4JGG3EKDfnw==\n"
	"-----END CERTIFICATE-----\n";

/* `openssl genpkey -algorithm ed25519`, from the probe. A throwaway. */
static const char ED25519_KEY_PEM[] =
	"-----BEGIN PRIVATE KEY-----\n"
	"MC4CAQAwBQYDK2VwBCIEICjZQ72woVMQORZoTQCozAnr28jNK3rMpvZQpoOXI8o2\n"
	"-----END PRIVATE KEY-----\n";

static const unsigned char ED25519_PUB[32] = {
	0x8c, 0xdb, 0xd0, 0x17, 0x73, 0x81, 0xf5, 0x52, 0x6c, 0x94, 0xd1, 0x73, 0xa8, 0x4a, 0x89, 0xe6,
	0x92, 0x22, 0x85, 0x1c, 0x42, 0x72, 0x20, 0xc8, 0xac, 0x32, 0x28, 0x81, 0xa2, 0x5c, 0x95, 0xb8,
};

static const char ED25519_MSG[] = "cix#351 ed25519 known-answer message";

/* `openssl pkeyutl -sign -rawin` over ED25519_MSG (no newline). */
static const unsigned char ED25519_SIG[64] = {
	0x25, 0x5f, 0x80, 0xb5, 0xae, 0x1d, 0x2b, 0x01, 0x81, 0x0d, 0x1d, 0x46, 0xf2, 0x28, 0xc2, 0x32,
	0xf7, 0x12, 0xf4, 0xf5, 0x14, 0x93, 0xca, 0x04, 0xc4, 0x3d, 0xe6, 0x67, 0x48, 0xe6, 0x37, 0x31,
	0x16, 0xea, 0x51, 0xf7, 0xa4, 0xa8, 0x65, 0xf9, 0xb1, 0xb0, 0x7f, 0xd0, 0x67, 0xf0, 0x33, 0x62,
	0x95, 0x30, 0x48, 0x9e, 0xce, 0xf9, 0xb3, 0xc8, 0xb8, 0x91, 0xf9, 0xf1, 0x00, 0xca, 0xbf, 0x02,
};

/* `openssl enc -aes-256-cbc -pbkdf2 -iter 600000 -md sha256 -salt -a -A
 * -pass file:...` of EXPORT_PLAIN under EXPORT_PASS. */
static const char EXPORT_PASS[] = "cix-kat-passphrase";
static const char EXPORT_PLAIN[] = "cix#351 export known-answer vector\n";
static const char EXPORT_B64[] =
	"U2FsdGVkX1/GA5H+41MG8abAeV76l1OKV6tEHvTLf2G5Tfr5vJJOv6QgZ4AKfClH7Ks6AoBCs+dx1fxNcvWvXw==";

/* `openssl enc ... -S 0011223344556677 -P`. */
static const unsigned char EXPORT_SALT[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
static const unsigned char EXPORT_KEY[32] = {
	0x82, 0xA8, 0x0D, 0xDB, 0x2A, 0x6C, 0x68, 0x8D, 0x4E, 0xF3, 0x62, 0xD5, 0xDD, 0x4D, 0xB3, 0x03,
	0x72, 0xF0, 0x1A, 0x6B, 0x17, 0xAF, 0x30, 0xD3, 0xB1, 0x87, 0x80, 0xF8, 0xC0, 0x48, 0xF3, 0x4D,
};
static const unsigned char EXPORT_IV[16] = {
	0xB7, 0x92, 0xAE, 0x3B, 0xA0, 0x66, 0x6B, 0x81, 0x33, 0xCD, 0xF9, 0x92, 0x11, 0xD7, 0x65, 0xCE,
};

static char g_dir[64];

static const char *path_in(const char *name)
{
	static char buf[8][128];
	static int next;
	char *p = buf[next++ % 8];

	snprintf(p, sizeof(buf[0]), "%s/%s", g_dir, name);
	return p;
}

static int write_file(const char *path, const char *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	int ok;

	if (f == NULL)
		return -1;
	ok = fwrite(data, 1, len, f) == len;
	return fclose(f) == 0 && ok ? 0 : -1;
}

static X509 *read_cert(const char *path)
{
	FILE *f = fopen(path, "r");
	X509 *x;

	if (f == NULL)
		return NULL;
	x = PEM_read_X509(f, NULL, NULL, NULL);
	fclose(f);
	return x;
}

/* The certificate's extensions, as NIDs in order, must be want[]. */
static void check_ext_order(const char *what, X509 *x, const int *want, int want_n)
{
	int n = X509_get_ext_count(x), i;

	CHECK(n == want_n, "%s has %d extensions, expected %d", what, n, want_n);
	for (i = 0; i < n && i < want_n; i++) {
		int nid = OBJ_obj2nid(X509_EXTENSION_get_object(X509_get_ext(x, i)));

		CHECK(nid == want[i], "%s extension %d is %s, expected %s", what, i, OBJ_nid2sn(nid),
		      OBJ_nid2sn(want[i]));
	}
}

static int ext_critical(X509 *x, int nid)
{
	int i = X509_get_ext_by_NID(x, nid, -1);

	return i >= 0 && X509_EXTENSION_get_critical(X509_get_ext(x, i));
}

/* v3, sha256WithRSA, valid for exactly days from now, CN a UTF8String. */
static void check_profile(const char *what, X509 *x, const char *cn, int days)
{
	X509_NAME *name = X509_get_subject_name(x);
	int idx = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
	X509_NAME_ENTRY *e = idx >= 0 ? X509_NAME_get_entry(name, idx) : NULL;
	ASN1_STRING *s = e != NULL ? X509_NAME_ENTRY_get_data(e) : NULL;
	int pday = 0, psec = 0;
	const ASN1_INTEGER *serial = X509_get0_serialNumber(x);

	CHECK(X509_get_version(x) == 2, "%s is not v3", what);
	CHECK(X509_get_signature_nid(x) == NID_sha256WithRSAEncryption, "%s is not sha256WithRSA",
	      what);
	CHECK(s != NULL && ASN1_STRING_type(s) == V_ASN1_UTF8STRING &&
	          (size_t)ASN1_STRING_length(s) == strlen(cn) &&
	          memcmp(ASN1_STRING_get0_data(s), cn, strlen(cn)) == 0,
	      "%s CN is not the UTF8String \"%s\"", what, cn);
	CHECK(ASN1_TIME_diff(&pday, &psec, X509_get0_notBefore(x), X509_get0_notAfter(x)) == 1 &&
	          pday == days && psec == 0,
	      "%s validity is %d days %d s, expected %d days", what, pday, psec, days);
	CHECK(serial != NULL && ASN1_STRING_length(serial) > 0 && ASN1_STRING_length(serial) <= 20 &&
	          ASN1_STRING_type(serial) == V_ASN1_INTEGER,
	      "%s serial is not a positive integer of at most 20 bytes", what);
}

static void test_live_root_fields(void)
{
	struct pkicrypto_cert_fields f;
	char err[256] = "";

	printf("live root CA fields\n");
	CHECK(write_file(path_in("live-root.pem"), LIVE_ROOT_PEM, strlen(LIVE_ROOT_PEM)) == 0,
	      "cannot write the live root");
	CHECK(pkicrypto_cert_fields(path_in("live-root.pem"), &f, err, sizeof(err)) == 0,
	      "fields: %s", err);
	CHECK(strcmp(f.subject, "CN = Cix Platform CA") == 0, "subject \"%s\"", f.subject);
	CHECK(strcmp(f.serial, "465981556E7BF703D756A43BBE90AEB85241449B") == 0, "serial \"%s\"",
	      f.serial);
	CHECK(strcmp(f.not_before, "Sep  7 16:47:25 2026 GMT") == 0, "not_before \"%s\"",
	      f.not_before);
	CHECK(strcmp(f.not_after, "Sep  4 16:47:25 2036 GMT") == 0, "not_after \"%s\"", f.not_after);
	CHECK(strcmp(f.sha1_fingerprint,
	             "2B:2B:6B:8A:1F:CB:EA:30:58:24:AD:89:30:59:1D:D8:77:14:08:E2") == 0,
	      "sha1 fingerprint \"%s\"", f.sha1_fingerprint);
	CHECK(strlen(f.sha256_fingerprint) == 95 && f.sha256_fingerprint[2] == ':',
	      "sha256 fingerprint \"%s\"", f.sha256_fingerprint);
}

static void test_chain(void)
{
	static const char *const int_ext[] = {
		"basicConstraints=critical,CA:TRUE,pathlen:0",
		"keyUsage=critical,keyCertSign,cRLSign",
	};
	static const char *const leaf_ext[] = { "subjectAltName=DNS:probe-leaf,IP:192.168.150.250" };
	static const int root_order[] = { NID_subject_key_identifier, NID_authority_key_identifier,
		                          NID_basic_constraints };
	static const int int_order[] = { NID_basic_constraints, NID_key_usage,
		                         NID_subject_key_identifier, NID_authority_key_identifier };
	static const int leaf_order[] = { NID_subject_alt_name, NID_subject_key_identifier,
		                          NID_authority_key_identifier };
	char err[256] = "";
	struct stat st;
	struct pkicrypto_cert_fields f;
	X509 *root = NULL, *inter = NULL, *leaf = NULL, *der_leaf = NULL;
	X509_STORE *store = NULL;
	X509_STORE_CTX *ctx = NULL;
	STACK_OF(X509) *untrusted = NULL;
	FILE *fp;
	char head[32] = "";

	printf("root -> intermediate -> leaf\n");
	CHECK(pkicrypto_rsa_key_create(path_in("ca.key"), err, sizeof(err)) == 0, "ca key: %s", err);
	CHECK(pkicrypto_rsa_key_create(path_in("int.key"), err, sizeof(err)) == 0, "int key: %s", err);
	CHECK(pkicrypto_rsa_key_create(path_in("leaf.key"), err, sizeof(err)) == 0, "leaf key: %s",
	      err);
	CHECK(stat(path_in("ca.key"), &st) == 0 && (st.st_mode & 0777) == 0600,
	      "a private key is not mode 0600");
	fp = fopen(path_in("ca.key"), "r");
	if (fp != NULL) {
		if (fgets(head, sizeof(head), fp) == NULL)
			head[0] = '\0';
		fclose(fp);
	}
	CHECK(strcmp(head, "-----BEGIN PRIVATE KEY-----\n") == 0,
	      "a private key is not unencrypted PKCS#8 PEM, as genpkey wrote");

	CHECK(pkicrypto_ca_create(path_in("ca.key"), "Probe Root", 30, path_in("ca.crt"), err,
	                          sizeof(err)) == 0,
	      "root: %s", err);
	CHECK(pkicrypto_cert_issue(path_in("ca.crt"), path_in("ca.key"), path_in("int.key"),
	                           "Probe Int", int_ext, 2, 30, path_in("int.crt"), err,
	                           sizeof(err)) == 0,
	      "intermediate: %s", err);
	CHECK(pkicrypto_cert_issue(path_in("int.crt"), path_in("int.key"), path_in("leaf.key"),
	                           "probe-leaf", leaf_ext, 1, 30, path_in("leaf.crt"), err,
	                           sizeof(err)) == 0,
	      "leaf: %s", err);

	root = read_cert(path_in("ca.crt"));
	inter = read_cert(path_in("int.crt"));
	leaf = read_cert(path_in("leaf.crt"));
	CHECK(root != NULL && inter != NULL && leaf != NULL, "a certificate did not read back");
	if (root == NULL || inter == NULL || leaf == NULL)
		goto out;

	check_profile("root", root, "Probe Root", 30);
	check_profile("intermediate", inter, "Probe Int", 30);
	check_profile("leaf", leaf, "probe-leaf", 30);
	check_ext_order("root", root, root_order, 3);
	check_ext_order("intermediate", inter, int_order, 4);
	check_ext_order("leaf", leaf, leaf_order, 3);
	CHECK(ext_critical(root, NID_basic_constraints), "root basicConstraints not critical");
	CHECK(ext_critical(inter, NID_basic_constraints) && ext_critical(inter, NID_key_usage),
	      "intermediate basicConstraints/keyUsage not critical");
	CHECK(X509_get_pathlen(inter) == 0, "intermediate pathlen is not 0");
	CHECK(X509_check_ca(root) && X509_check_ca(inter) && !X509_check_ca(leaf),
	      "CA flags wrong");
	CHECK(X509_check_host(leaf, "probe-leaf", 0, 0, NULL) == 1, "leaf SAN lacks DNS:probe-leaf");
	CHECK(X509_check_ip_asc(leaf, "192.168.150.250", 0) == 1, "leaf SAN lacks IP 192.168.150.250");

	store = X509_STORE_new();
	ctx = X509_STORE_CTX_new();
	untrusted = sk_X509_new_null();
	CHECK(store != NULL && ctx != NULL && untrusted != NULL && X509_STORE_add_cert(store, root) == 1 &&
	          sk_X509_push(untrusted, inter) > 0 &&
	          X509_STORE_CTX_init(ctx, store, leaf, untrusted) == 1 && X509_verify_cert(ctx) == 1,
	      "the chain does not verify: %s",
	      ctx != NULL ? X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx)) : "no ctx");

	CHECK(pkicrypto_cert_fields(path_in("ca.crt"), &f, err, sizeof(err)) == 0 &&
	          strcmp(f.subject, "CN = Probe Root") == 0,
	      "root fields: subject \"%s\" %s", f.subject, err);
	CHECK(strlen(f.serial) > 0 && strlen(f.serial) % 2 == 0 &&
	          strspn(f.serial, "0123456789ABCDEF") == strlen(f.serial),
	      "serial \"%s\" is not even-length uppercase hex", f.serial);

	CHECK(pkicrypto_key_matches_cert(path_in("int.key"), path_in("int.crt"), NULL, 0) == 1,
	      "a key does not match its own certificate");
	CHECK(pkicrypto_key_matches_cert(path_in("leaf.key"), path_in("int.crt"), NULL, 0) == 0,
	      "a key matches another's certificate");
	CHECK(pkicrypto_key_matches_cert(path_in("nonexistent.key"), path_in("int.crt"), NULL, 0) == -1,
	      "a missing key is not an error");

	CHECK(pkicrypto_cert_pem_to_der(path_in("leaf.crt"), path_in("leaf.der"), err, sizeof(err)) == 0,
	      "pem->der: %s", err);
	fp = fopen(path_in("leaf.der"), "rb");
	if (fp != NULL) {
		der_leaf = d2i_X509_fp(fp, NULL);
		fclose(fp);
	}
	CHECK(der_leaf != NULL && X509_cmp(der_leaf, leaf) == 0, "the DER is not the same certificate");
out:
	sk_X509_free(untrusted);
	X509_STORE_CTX_free(ctx);
	X509_STORE_free(store);
	X509_free(der_leaf);
	X509_free(leaf);
	X509_free(inter);
	X509_free(root);
}

static void test_export(void)
{
	unsigned char key[32], iv[16];
	unsigned char *plain = NULL;
	size_t plain_len = 0;
	char *b64 = NULL;
	char err[256] = "";

	printf("export cipher\n");
	CHECK(pkicrypto_export_derive(EXPORT_PASS, EXPORT_SALT, key, iv) == 0 &&
	          memcmp(key, EXPORT_KEY, 32) == 0 && memcmp(iv, EXPORT_IV, 16) == 0,
	      "PBKDF2 key/IV differ from the CLI's");

	CHECK(pkicrypto_export_decrypt(EXPORT_PASS, EXPORT_B64, strlen(EXPORT_B64), &plain,
	                               &plain_len, err, sizeof(err)) == 0 &&
	          plain_len == strlen(EXPORT_PLAIN) && memcmp(plain, EXPORT_PLAIN, plain_len) == 0,
	      "the CLI's ciphertext did not decrypt to its plaintext: %s", err);
	free(plain);
	plain = NULL;

	CHECK(pkicrypto_export_decrypt("wrong passphrase", EXPORT_B64, strlen(EXPORT_B64), &plain,
	                               &plain_len, NULL, 0) != 0,
	      "a wrong passphrase decrypted");
	free(plain);
	plain = NULL;

	CHECK(pkicrypto_export_encrypt(EXPORT_PASS, (const unsigned char *)EXPORT_PLAIN,
	                               strlen(EXPORT_PLAIN), &b64, err, sizeof(err)) == 0,
	      "encrypt: %s", err);
	if (b64 != NULL) {
		CHECK(strncmp(b64, "U2FsdGVkX1", 10) == 0 && strchr(b64, '\n') == NULL,
		      "our output is not one base64 line with the Salted__ header");
		CHECK(pkicrypto_export_decrypt(EXPORT_PASS, b64, strlen(b64), &plain, &plain_len, err,
		                               sizeof(err)) == 0 &&
		          plain_len == strlen(EXPORT_PLAIN) &&
		          memcmp(plain, EXPORT_PLAIN, plain_len) == 0,
		      "our own ciphertext did not round-trip: %s", err);
		free(plain);
	}
	free(b64);
}

static void test_ed25519(void)
{
	unsigned char pub[32], sig[64], bad[64];
	char err[256] = "";

	printf("ed25519\n");
	CHECK(write_file(path_in("ed.key"), ED25519_KEY_PEM, strlen(ED25519_KEY_PEM)) == 0 &&
	          write_file(path_in("msg"), ED25519_MSG, strlen(ED25519_MSG)) == 0 &&
	          write_file(path_in("empty"), "", 0) == 0,
	      "cannot write the Ed25519 fixtures");
	CHECK(pkicrypto_ed25519_public_raw(path_in("ed.key"), pub, err, sizeof(err)) == 0 &&
	          memcmp(pub, ED25519_PUB, 32) == 0,
	      "raw public key differs from the CLI's: %s", err);
	CHECK(pkicrypto_ed25519_sign_file(path_in("ed.key"), path_in("msg"), sig, err, sizeof(err)) == 0 &&
	          memcmp(sig, ED25519_SIG, 64) == 0,
	      "signature differs from the CLI's: %s", err);
	CHECK(pkicrypto_ed25519_verify_file(ED25519_PUB, path_in("msg"), ED25519_SIG) == 0,
	      "the CLI's signature does not verify");
	memcpy(bad, ED25519_SIG, 64);
	bad[10] ^= 1;
	CHECK(pkicrypto_ed25519_verify_file(ED25519_PUB, path_in("msg"), bad) != 0,
	      "a damaged signature verified");
	CHECK(pkicrypto_ed25519_sign_file(path_in("ed.key"), path_in("empty"), sig, NULL, 0) == 0 &&
	          pkicrypto_ed25519_verify_file(ED25519_PUB, path_in("empty"), sig) == 0,
	      "an empty file does not sign and verify");
	CHECK(pkicrypto_ed25519_public_raw(path_in("ca.key"), pub, NULL, 0) != 0,
	      "an RSA key was accepted as Ed25519");
}

int main(void)
{
	char cmd[96];

	snprintf(g_dir, sizeof(g_dir), "/tmp/cix_pkicrypto_XXXXXX");
	if (mkdtemp(g_dir) == NULL) {
		printf("PKICRYPTO RESULT: FAIL (cannot create a scratch directory)\n");
		return 1;
	}
	printf("test_pkicrypto\n");
	test_live_root_fields();
	test_chain();
	test_export();
	test_ed25519();
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", g_dir);
	if (g_failures != 0) {
		printf("PKICRYPTO RESULT: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("PKICRYPTO RESULT: PASS\n");
	return 0;
}
