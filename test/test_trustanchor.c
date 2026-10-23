/*
 * test_trustanchor -- ADR-0350's trust-anchor store and the combined
 * bundle cixd's fetches verify against.
 *
 * The certificates are made here with pkicrypto.c, the same code that
 * makes the platform's own CA, so nothing is a fixture that could drift
 * from what OpenSSL accepts: a root CA, an intermediate it issues with
 * basicConstraints CA:TRUE, and a leaf it issues without.
 *
 * What it holds the store to: exactly one CA certificate per anchor
 * (a leaf, a key alongside the certificate, and two certificates are
 * each refused with their reason); the fingerprint is the bare
 * lowercase SHA-256 of the DER; the store survives a restart; the
 * combined bundle is the public roots followed by every anchor, and
 * there is none -- so libcurl keeps its default -- when no anchor is
 * registered or the public roots cannot be read.
 */
#include "pkicrypto.h"
#include "trustanchor.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, what)                                                                       \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			fprintf(stderr, "FAIL: %s\n", (what));                                              \
			g_failures++;                                                                       \
		} else {                                                                                \
			printf("  ok: %s\n", (what));                                                       \
		}                                                                                       \
	} while (0)

/* trustanchor.c logs through the log store; a test provides its own. */
void logstore_write(const char *source, const char *level, const char *fmt, ...);
void logstore_write(const char *source, const char *level, const char *fmt, ...)
{
	(void)source;
	(void)level;
	(void)fmt;
}

static char g_dir[256];

static void path_in(char *out, size_t size, const char *name)
{
	snprintf(out, size, "%s/%s", g_dir, name);
}

/* The whole file, NUL-terminated, into a static buffer. */
static const char *slurp(const char *path, size_t *len)
{
	static char buf[65536];
	FILE *f = fopen(path, "r");
	size_t n = 0;

	if (f != NULL) {
		n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
	}
	buf[n] = '\0';
	*len = n;
	return buf;
}

/* A copy of a certificate file's PEM. */
static char *read_pem(const char *name, size_t *len)
{
	char path[512];
	const char *p;
	char *copy;

	path_in(path, sizeof(path), name);
	p = slurp(path, len);
	copy = malloc(*len + 1);
	memcpy(copy, p, *len + 1);
	return copy;
}

int main(void)
{
	char key[512], root[512], inter_key[512], inter[512], leaf_key[512], leaf[512];
	char store[512], bundle[512], pub[512], missing[512], err[512], fp[65];
	static const char *const ca_ext[] = { "basicConstraints=critical,CA:TRUE" };
	struct pkicrypto_cert_fields fields;
	struct pkicrypto_anchor info;
	char *root_pem, *inter_pem, *leaf_pem, *both, *pair;
	size_t root_len, inter_len, leaf_len, len, i;
	const char *content;
	char expect_fp[65];
	FILE *f;
	int n;

	snprintf(g_dir, sizeof(g_dir), "/tmp/trustanchor_test_%d", (int)getpid());
	if (mkdir(g_dir, 0700) != 0) {
		perror("mkdir");
		return 1;
	}
	path_in(key, sizeof(key), "root.key");
	path_in(root, sizeof(root), "root.crt");
	path_in(inter_key, sizeof(inter_key), "inter.key");
	path_in(inter, sizeof(inter), "inter.crt");
	path_in(leaf_key, sizeof(leaf_key), "leaf.key");
	path_in(leaf, sizeof(leaf), "leaf.crt");
	path_in(store, sizeof(store), "trust_anchors.json");
	path_in(bundle, sizeof(bundle), "ca-bundle.pem");
	path_in(pub, sizeof(pub), "public-roots.pem");
	path_in(missing, sizeof(missing), "no-such-public-roots.pem");

	if (pkicrypto_rsa_key_create(key, err, sizeof(err)) != 0 ||
	    pkicrypto_ca_create(key, "Test Site Root", 30, root, err, sizeof(err)) != 0 ||
	    pkicrypto_rsa_key_create(inter_key, err, sizeof(err)) != 0 ||
	    pkicrypto_cert_issue(root, key, inter_key, "Test Site Intermediate", ca_ext, 1, 30, inter,
	                         err, sizeof(err)) != 0 ||
	    pkicrypto_rsa_key_create(leaf_key, err, sizeof(err)) != 0 ||
	    pkicrypto_cert_issue(root, key, leaf_key, "forge.test", NULL, 0, 30, leaf, err,
	                         sizeof(err)) != 0) {
		fprintf(stderr, "FAIL: making the test certificates: %s\n", err);
		return 1;
	}
	root_pem = read_pem("root.crt", &root_len);
	inter_pem = read_pem("inter.crt", &inter_len);
	leaf_pem = read_pem("leaf.crt", &leaf_len);

	printf("1. inspecting an anchor\n");
	CHECK(pkicrypto_anchor_inspect(root_pem, root_len, &info, err, sizeof(err)) == 0 &&
	          info.root == 1 && info.expired == 0,
	      "a self-signed CA reads as a root, not expired");
	CHECK(strcmp(info.name, "Test Site Root") == 0, "the name is the subject's commonName (#629)");
	/* The same digest pkicrypto_cert_fields() reports, written bare
	 * and lowercase: one definition of the fingerprint, two spellings. */
	CHECK(pkicrypto_cert_fields(root, &fields, err, sizeof(err)) == 0, "the root's fields read");
	for (i = 0, n = 0; fields.sha256_fingerprint[i] != '\0' && n < 64; i++)
		if (fields.sha256_fingerprint[i] != ':')
			expect_fp[n++] = (char)tolower((unsigned char)fields.sha256_fingerprint[i]);
	expect_fp[n] = '\0';
	CHECK(strlen(info.fingerprint) == 64 && strcmp(info.fingerprint, expect_fp) == 0,
	      "the fingerprint is the SHA-256 of the DER, 64 lowercase hex, no separators");
	CHECK(pkicrypto_anchor_inspect(inter_pem, inter_len, &info, err, sizeof(err)) == 0 &&
	          info.root == 0 && strcmp(info.name, "Test Site Intermediate") == 0,
	      "an intermediate CA is accepted, reads as not a root, and is named by its CN");
	CHECK(pkicrypto_anchor_inspect(leaf_pem, leaf_len, &info, err, sizeof(err)) != 0 &&
	          strstr(err, "not a CA") != NULL,
	      "a leaf certificate is refused as not a CA");
	printf("     (%s)\n", err);

	both = malloc(root_len + 4096);
	{
		size_t klen;
		const char *k = slurp(key, &klen);

		memcpy(both, k, klen);
		memcpy(both + klen, root_pem, root_len + 1);
		CHECK(pkicrypto_anchor_inspect(both, klen + root_len, &info, err, sizeof(err)) != 0 &&
		          strstr(err, "private key") != NULL,
		      "a private key beside the certificate is refused");
	}
	pair = malloc(root_len + inter_len + 1);
	memcpy(pair, root_pem, root_len);
	memcpy(pair + root_len, inter_pem, inter_len + 1);
	CHECK(pkicrypto_anchor_inspect(pair, root_len + inter_len, &info, err, sizeof(err)) != 0 &&
	          strstr(err, "more than one") != NULL,
	      "two certificates in one PEM are refused");
	CHECK(pkicrypto_anchor_inspect("hello", 5, &info, err, sizeof(err)) != 0,
	      "text that is not a PEM certificate is refused");

	printf("2. the store and the combined bundle\n");
	f = fopen(pub, "w");
	fputs("# PUBLIC ROOTS\n", f);
	fclose(f);
	CHECK(trustanchor_init(store, bundle, pub) == 0 && trustanchor_count() == 0,
	      "an absent store starts empty");
	CHECK(trustanchor_bundle() == NULL && access(bundle, F_OK) != 0,
	      "with no anchors there is no combined bundle, so libcurl keeps its default");
	CHECK(trustanchor_add(leaf_pem, leaf_len, fp, err, sizeof(err)) == TRUSTANCHOR_INVALID,
	      "adding a leaf is TRUSTANCHOR_INVALID");
	CHECK(trustanchor_add(root_pem, root_len, fp, err, sizeof(err)) == TRUSTANCHOR_OK &&
	          strcmp(fp, expect_fp) == 0,
	      "adding the root succeeds and names it by its fingerprint");
	CHECK(trustanchor_add(root_pem, root_len, fp, err, sizeof(err)) == TRUSTANCHOR_DUPLICATE,
	      "adding it again is TRUSTANCHOR_DUPLICATE");
	content = slurp(bundle, &len);
	CHECK(trustanchor_bundle() != NULL && strncmp(content, "# PUBLIC ROOTS\n", 15) == 0 &&
	          strstr(content, root_pem) != NULL,
	      "the combined bundle is the public roots followed by the anchor");
	CHECK(trustanchor_add(inter_pem, inter_len, fp, err, sizeof(err)) == TRUSTANCHOR_OK &&
	          trustanchor_count() == 2,
	      "an intermediate is added beside it");

	printf("3. persistence and removal\n");
	CHECK(trustanchor_init(store, bundle, pub) == 0 && trustanchor_count() == 2 &&
	          trustanchor_find(expect_fp) != NULL,
	      "the store survives a restart");
	CHECK(trustanchor_remove(expect_fp) == TRUSTANCHOR_OK && trustanchor_count() == 1,
	      "removing the root by fingerprint");
	content = slurp(bundle, &len);
	CHECK(strstr(content, root_pem) == NULL && strstr(content, inter_pem) != NULL,
	      "the bundle no longer carries the removed anchor");
	CHECK(trustanchor_remove(expect_fp) == TRUSTANCHOR_NOT_FOUND,
	      "removing it again is TRUSTANCHOR_NOT_FOUND");
	CHECK(trustanchor_remove(trustanchor_get(0)->info.fingerprint) == TRUSTANCHOR_OK &&
	          trustanchor_bundle() == NULL && access(bundle, F_OK) != 0,
	      "the last removal takes the combined bundle away");

	printf("4. limits and fallbacks\n");
	CHECK(trustanchor_init(store, bundle, missing) == 0, "the store loads without public roots");
	CHECK(trustanchor_add(root_pem, root_len, fp, err, sizeof(err)) == TRUSTANCHOR_OK &&
	          trustanchor_bundle() == NULL,
	      "without the public roots no bundle is written, so public sources stay reachable");
	CHECK(trustanchor_init(store, bundle, pub) == 0 && trustanchor_bundle() != NULL,
	      "with the public roots back, the next start writes it");
	for (i = 0; i < TRUSTANCHOR_MAX; i++) {
		char cn[64], extra[512], *pem;
		size_t plen;

		snprintf(cn, sizeof(cn), "Test CA %zu", i);
		snprintf(extra, sizeof(extra), "extra%zu.crt", i);
		path_in(root, sizeof(root), extra);
		if (pkicrypto_ca_create(key, cn, 30, root, err, sizeof(err)) != 0)
			break;
		pem = read_pem(extra, &plen);
		if (trustanchor_add(pem, plen, fp, err, sizeof(err)) == TRUSTANCHOR_FULL) {
			free(pem);
			break;
		}
		free(pem);
	}
	CHECK(trustanchor_count() == TRUSTANCHOR_MAX, "the store holds TRUSTANCHOR_MAX anchors");
	CHECK(trustanchor_add(inter_pem, inter_len, fp, err, sizeof(err)) == TRUSTANCHOR_FULL,
	      "one past the limit is TRUSTANCHOR_FULL");

	free(root_pem);
	free(inter_pem);
	free(leaf_pem);
	free(both);
	free(pair);
	{
		char cmd[512];

		snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
		if (system(cmd) != 0)
			fprintf(stderr, "  (could not remove %s)\n", g_dir);
	}
	if (g_failures != 0) {
		printf("TRUSTANCHOR RESULT: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("TRUSTANCHOR RESULT: PASS\n");
	return 0;
}
