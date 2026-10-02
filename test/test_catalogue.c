/*
 * ADR-0324 step C: the catalogue index, with no daemon and no keys.
 *
 * The case that matters most is the fixed point: once the index and its
 * signature are written into the tree, building the index again must
 * give the same bytes. If it did not -- an index that listed itself, or
 * "every file under recipes/" -- the signing host would see a changed
 * tree after every commit and re-sign on every sync, forever.
 */
#include "catalogue.h"

#include <openssl/evp.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static char g_dir[PATH_MAX];

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("  FAIL: %s\n", what);
		failures++;
	}
}

/* The test's own hash, standing in for the daemon's pkg_run_capture_sha256(). */
static int hash_file(const char *path, char *out, size_t out_size)
{
	unsigned char buf[4096], md[EVP_MAX_MD_SIZE];
	unsigned int md_len = 0, i;
	EVP_MD_CTX *ctx;
	FILE *f;
	size_t n;

	if (out_size < 65 || (f = fopen(path, "rb")) == NULL)
		return -1;
	ctx = EVP_MD_CTX_new();
	if (ctx == NULL || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_free(ctx);
		fclose(f);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		EVP_DigestUpdate(ctx, buf, n);
	fclose(f);
	EVP_DigestFinal_ex(ctx, md, &md_len);
	EVP_MD_CTX_free(ctx);
	for (i = 0; i < md_len; i++)
		snprintf(out + i * 2, 3, "%02x", md[i]);
	out[64] = '\0';
	return 0;
}

static void put(const char *rel, const char *content)
{
	char path[PATH_MAX], cmd[PATH_MAX + 32];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_dir, rel);
	snprintf(cmd, sizeof(cmd), "mkdir -p \"$(dirname '%s')\"", path);
	if (system(cmd) != 0)
		return;
	f = fopen(path, "w");
	if (f != NULL) {
		fputs(content, f);
		fclose(f);
	}
}

static void test_build_and_fixed_point(void)
{
	char *a, *b, sha[65], path[PATH_MAX];
	size_t alen = 0, blen = 0;
	struct catalogue *c;

	put("recipes/package/zlib@1.3.2-12.cbs", "package \"zlib\" {}\n");
	put("recipes/package/acl@2.3.2-1.cbs", "package \"acl\" {}\n");
	put("recipes/image/jumpbox@1.0.0.sh", "image\n");
	put("recipes/deployment/dns-1@1.2.0.json", "{}\n");
	put("docs/keys/cix-release.pub", "untrusted comment: k\nRWQ\n");
	put("recipes/README.md", "not something a sync takes\n");
	put("README.md", "nor this\n");

	a = catalogue_build(g_dir, hash_file, &alen);
	check(a != NULL, "an index is built");
	if (a == NULL)
		return;
	check(strncmp(a, "# cix catalogue index v1", 24) == 0 &&
	          strstr(a, "# excludes recipes/INDEX and recipes/INDEX.minisig\n") != NULL,
	      "it opens with its header, which names what it excludes");
	check(strstr(a, "recipes/README.md") == NULL && strstr(a, "  README.md") == NULL,
	      "files a sync does not take are not listed");
	check(strstr(a, "  docs/keys/cix-release.pub\n") != NULL,
	      "keys are listed: adopting one grants artifact trust");
	check(strstr(a, "acl@2.3.2-1") < strstr(a, "zlib@1.3.2-12") &&
	          strstr(a, "recipes/deployment/") < strstr(a, "recipes/image/"),
	      "entries are sorted by path");
	snprintf(path, sizeof(path), "%s/recipes/package/acl@2.3.2-1.cbs", g_dir);
	hash_file(path, sha, sizeof(sha));
	check(strstr(a, sha) != NULL, "each line carries the file's sha256");

	put("recipes/INDEX", a);
	put("recipes/INDEX.minisig", "untrusted comment: x\nRWQ\ntrusted comment: y\nZZZ\n");
	b = catalogue_build(g_dir, hash_file, &blen);
	check(b != NULL && blen == alen && memcmp(a, b, alen) == 0,
	      "writing the index and its signature into the tree does not change the index");

	c = catalogue_parse(a, alen);
	check(c != NULL && catalogue_count(c) == 5, "the index parses back to its five entries");
	check(catalogue_vouches(c, "recipes/package/acl@2.3.2-1.cbs", sha),
	      "it vouches for a listed file with its sha256");
	check(!catalogue_vouches(c, "recipes/package/acl@2.3.2-1.cbs",
	                         "0000000000000000000000000000000000000000000000000000000000000000"),
	      "not for the same path with other bytes");
	check(!catalogue_vouches(c, "recipes/package/evil@1-1.cbs", sha),
	      "nor for a path it does not list");
	catalogue_free(c);
	free(a);
	free(b);

	/* A file added after signing is not vouched for until re-signed. */
	put("recipes/package/new@1-1.cbs", "package \"new\" {}\n");
	a = catalogue_build(g_dir, hash_file, &alen);
	check(a != NULL && blen != alen, "a new file changes the index, so it must be signed again");
	free(a);
}

static void test_comment(void)
{
	static const char sha[] = "ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01";
	char comment[256];
	long long t = 0;

	check(catalogue_comment(sha, 1790000000LL, comment, sizeof(comment)) == 0 &&
	          strcmp(comment, "cix catalogue sha256=" "ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01"
	                          "ab01ab01ab01ab01ab01 time=1790000000") == 0,
	      "the trusted comment names the index and the time");
	check(catalogue_comment_parse(comment, sha, &t) == 0 && t == 1790000000LL,
	      "and reads back");
	check(catalogue_comment_parse(comment,
	                              "cd01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01",
	                              &t) != 0,
	      "a comment for another index is refused");
	check(catalogue_comment_parse("cix catalogue sha256=ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01"
	                              "ab01ab01ab01ab01ab01ab01 time=17x",
	                              sha, &t) != 0,
	      "trailing junk is refused");
	check(catalogue_comment_parse("cix pkg zlib@1 sha256=ab", sha, &t) != 0,
	      "an artifact's comment is not a catalogue's");
}

int main(void)
{
	char tmpl[] = "/tmp/cix_catalogue_XXXXXX";
	char cmd[PATH_MAX + 16];

	printf("test_catalogue\n");
	if (mkdtemp(tmpl) == NULL) {
		printf("CATALOGUE RESULT: FAIL (mkdtemp: %s)\n", strerror(errno));
		return 1;
	}
	snprintf(g_dir, sizeof(g_dir), "%s", tmpl);
	test_build_and_fixed_point();
	test_comment();
	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", g_dir);
	printf("CATALOGUE RESULT: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
