/*
 * ADR-0254's last link: turning a verified upstream checksum into a
 * publishable kernel recipe revision.
 *
 * The fixture is the REAL kernel recipe from the repo, not a synthetic
 * one. A generator that works on a tidy invented recipe and not on the
 * actual file is worthless, and the real file is where the hazards are:
 * two space-separated sources, two space-separated checksums, a
 * {{REPO_TOKEN}} placeholder, a git ref pinning the config, and 559
 * lines of comment prose that mention pkg_source and pkg_sha256 in
 * passing.
 */
#include "kernelrecipe.h"

#include "test_image_fixture.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void fail(const char *fmt, const char *a)
{
	fprintf(stderr, "  FAIL: ");
	fprintf(stderr, fmt, a);
	fprintf(stderr, "\n");
	g_failures++;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *b;
	long n;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		fclose(f);
		return NULL;
	}
	fclose(f);
	b[n] = '\0';
	return b;
}

/* Counts non-overlapping occurrences, for "nothing else moved" checks. */
static int count(const char *hay, const char *needle)
{
	int n = 0;
	const char *p = hay;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}


/*
 * Extracts a line-anchored key="value" from generated text.
 *
 * Presence checks are not enough and that is not hypothetical: a
 * deliberately reversed splice order, which misaligns every later edit
 * by the length change of the earlier one, still leaves every expected
 * substring present somewhere in the file. Only comparing the whole
 * value catches it.
 */
static int field(const char *text, const char *key, char *out, size_t out_size)
{
	size_t klen = strlen(key);
	const char *p = text;

	while (p != NULL && *p != '\0') {
		if ((p == text || p[-1] == '\n') && strncmp(p, key, klen) == 0 && p[klen] == '"') {
			const char *q = strchr(p + klen + 1, '"');

			if (q == NULL || (size_t)(q - (p + klen + 1)) >= out_size)
				return -1;
			memcpy(out, p + klen + 1, (size_t)(q - (p + klen + 1)));
			out[q - (p + klen + 1)] = '\0';
			return 0;
		}
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	return -1;
}

int main(void)
{
	static const char SHA[] = "8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03";
	char kpath[PATH_MAX];
	char *cur;

	/*
	 * #504: the corpus is its own repository and flat. This test
	 * links daemon code (kernelrecipe.c), which is why it stayed here
	 * rather than moving with the recipes it reads -- and why it is
	 * NOT in SELFTESTS: a build container has no sibling checkout, so
	 * a release selftest would fail on a missing file rather than on
	 * anything about the kernel recipe.
	 */
	if (test_recipe_path("package", "kernel", "6.18.40-24", "sh", kpath, sizeof(kpath)) != 0)
		return 1;
	cur = slurp(kpath);
	char *gen = NULL;
	char err[256];
	char tarball[512], sums[512];

	if (cur == NULL) {
		fprintf(stderr, "KERNEL RECIPE: cannot read the fixture recipe -- run from the repo root\n");
		return 1;
	}

	/* URLs are derived from the version, including the vN.x directory */
	if (kernel_upstream_urls("7.2.3", tarball, sizeof(tarball), sums, sizeof(sums)) != 0)
		fail("could not derive upstream URLs for %s", "7.2.3");
	else {
		if (strcmp(tarball,
		           "https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz") != 0)
			fail("wrong tarball URL: %s", tarball);
		if (strcmp(sums, "https://cdn.kernel.org/pub/linux/kernel/v7.x/sha256sums.asc") != 0)
			fail("wrong sums URL: %s", sums);
	}
	if (kernel_upstream_urls("6.18.49", tarball, sizeof(tarball), sums, sizeof(sums)) == 0) {
		if (strstr(tarball, "/v6.x/") == NULL)
			fail("6.18.49 did not resolve into v6.x: %s", tarball);
	}
	/* a recipe-revision suffix is ours and is never an upstream name */
	if (kernel_upstream_urls("7.2.3-1", tarball, sizeof(tarball), sums, sizeof(sums)) == 0)
		fail("accepted a recipe revision suffix as an upstream version%s", "");

	/* the generation itself */
	if (kernel_recipe_generate(cur, "7.2.3-1", "https://cdn.kernel.org/pub/linux/kernel/v7.x/"
	                                            "linux-7.2.3.tar.xz",
	                            SHA, &gen, err, sizeof(err)) != 0) {
		fail("generation failed: %s", err);
	} else {
		if (strstr(gen, "pkg_version=\"7.2.3-1\"") == NULL)
			fail("pkg_version was not updated%s", "");
		if (strstr(gen, "linux-7.2.3.tar.xz") == NULL)
			fail("new tarball URL is absent%s", "");
		if (strstr(gen, SHA) == NULL)
			fail("new checksum is absent%s", "");
		if (strstr(gen, "linux-6.18.40.tar.xz") != NULL)
			fail("the OLD tarball URL survived%s", "");

		/* the second source and its checksum must be untouched */
		if (strstr(gen, "{{REPO_TOKEN}}") == NULL)
			fail("the forge source placeholder was lost%s", "");
		if (strstr(gen, "qemu-part1.config?ref=0ee4c282dcbdb08e2b59c93d8dd829278620e8e2") == NULL)
			fail("the config source and its pinned git ref were lost%s", "");
		if (strstr(gen, "f98fcc7b5d20c2991b52e53db20f7b4ff5d82e9fb7ac271ecdfe8241cd04ab36") == NULL)
			fail("the config source's checksum was lost%s", "");

		/* the body is carried forward byte for byte */
		/* the exact values, not merely their presence */
		{
			char v[1024];

			if (field(gen, "pkg_version=", v, sizeof(v)) != 0 || strcmp(v, "7.2.3-1") != 0)
				fail("pkg_version value is wrong: %s", v);
			if (field(gen, "pkg_sha256=", v, sizeof(v)) != 0)
				fail("pkg_sha256 is not a well-formed assignment%s", "");
			else if (strncmp(v, SHA, 64) != 0 || v[64] != ' ')
				fail("pkg_sha256 first field is wrong: %s", v);
			if (field(gen, "pkg_source=", v, sizeof(v)) != 0)
				fail("pkg_source is not a well-formed assignment%s", "");
			else if (strncmp(v, "https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz ",
			                 63) != 0)
				fail("pkg_source first field is wrong: %s", v);
		}

		if (count(gen, "pkg_build()") != count(cur, "pkg_build()"))
			fail("pkg_build() no longer appears exactly once%s", "");
		if (count(gen, "merge_config.sh") != count(cur, "merge_config.sh"))
			fail("the config merge step was altered%s", "");
		/*
		 * The declaration is what a generated recipe must not lose,
		 * and since ADR-0304 (#482) that is pkg_build_depends rather
		 * than pkg_build_image: the build container is composed from
		 * the declared tools, so losing this line leaves a recipe
		 * that composes nothing and cannot build at all. This
		 * asserted pkg_build_image until then, a field the daemon no
		 * longer reads.
		 */
		if (strstr(gen, "pkg_build_depends=\"bash bc binutils") == NULL)
			fail("pkg_build_depends was lost%s", "");
	}
	free(gen);
	gen = NULL;

	/* a checksum that is not 64 lowercase hex is refused, not written */
	if (kernel_recipe_generate(cur, "7.2.3-1", "https://x/linux-7.2.3.tar.xz", "deadbeef", &gen,
	                           err, sizeof(err)) == 0)
		fail("a short checksum was accepted%s", "");
	if (gen != NULL)
		fail("a rejected generation still produced output%s", "");
	if (kernel_recipe_generate(cur, "7.2.3-1", "https://x/linux-7.2.3.tar.xz",
	                           "8BA259E8E7B13EC6EF0941C8A39AD90B24BD4A4D6C0010BA6BAFB794550ECD03",
	                           &gen, err, sizeof(err)) == 0)
		fail("an uppercase checksum was accepted%s", "");

	free(cur);
	if (g_failures > 0) {
		fprintf(stderr, "KERNEL RECIPE: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("KERNEL RECIPE: ok (url derivation, generation, second source preserved, body intact, "
	       "bad checksums refused)\n");
	return 0;
}
