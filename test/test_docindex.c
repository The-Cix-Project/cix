/*
 * Issue #201: every index gains a row when its directory gains a
 * document.
 *
 * Three index drifts were found by hand in one evening, all the same
 * shape -- the substance shipped and the pointer to it did not. The
 * Documentation Map in CLAUDE.md already assigns every document an
 * owner and a mutability rule, and that Map is what made them findable
 * once someone went looking. What a rule cannot do is notice.
 *
 * This is the same argument ADR-0199 made about build tools:
 * sufficiency is enforced by the build, minimality is review. Index
 * completeness is a sufficiency property, so it is enforced here rather
 * than left to care. The strongest evidence that care is not enough is
 * that one of the three misses was committed hours before it was found,
 * by someone who knew the rule and had just re-read it.
 *
 * Deliberately checks only what is mechanically true -- that a row
 * EXISTS and its link resolves. Whether a row says anything useful is
 * review, and pretending to check it here would be the kind of gate
 * that passes while the thing it names is wrong.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	fputs("FAIL: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	g_failures++;
}

/* Whole file into memory -- the largest index here is a few tens of KB,
 * and a streaming search would only add a way to miss a match spanning
 * a buffer boundary. */
static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *buf;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	n = ftell(f);
	if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static int file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * 1. Every ADR is listed in the ADR index, and every listed link
 *    resolves.
 *
 * The index links by filename, so the filename is what is searched
 * for -- matching on the number alone would let "0215" in a sentence
 * about something else satisfy the check, which is how a gate ends up
 * green while the row it is meant to enforce is missing.
 */
static void check_adr_index(void)
{
	char *index = slurp("docs/adr/README.md");
	DIR *d;
	struct dirent *e;
	int listed = 0;

	if (index == NULL) {
		fail("docs/adr/README.md is unreadable -- the ADR index must exist");
		return;
	}
	d = opendir("docs/adr");
	if (d == NULL) {
		fail("docs/adr is unreadable");
		free(index);
		return;
	}
	while ((e = readdir(d)) != NULL) {
		size_t len = strlen(e->d_name);

		/* NNNN-slug.md, and never README.md itself -- the index does
		 * not index itself. */
		if (len < 8 || strcmp(e->d_name + len - 3, ".md") != 0)
			continue;
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		if (strstr(index, e->d_name) == NULL)
			fail("ADR %s has no row in docs/adr/README.md -- the decision shipped and the "
			     "pointer to it did not (#201)",
			     e->d_name);
		else
			listed++;
	}
	closedir(d);

	/*
	 * And the other direction: a row whose target does not exist. A
	 * dangling row is the same drift seen from the other side -- it
	 * survives a rename, and reads as if the document is still there.
	 */
	{
		const char *p = index;

		while ((p = strstr(p, "](")) != NULL) {
			char target[512];
			const char *start = p + 2;
			const char *end = strchr(start, ')');
			char path[600];

			p = start;
			if (end == NULL || (size_t)(end - start) >= sizeof(target))
				continue;
			memcpy(target, start, (size_t)(end - start));
			target[end - start] = '\0';
			/* Only local ADR files -- external links are not this
			 * test's business and cannot be checked offline. */
			if (strchr(target, ':') != NULL || target[0] == '#' || target[0] == '/')
				continue;
			if (strstr(target, ".md") == NULL)
				continue;
			snprintf(path, sizeof(path), "docs/adr/%s", target);
			if (!file_exists(path))
				fail("docs/adr/README.md links to %s, which does not exist", target);
		}
	}

	printf("  adr index: %d ADRs listed\n", listed);
	free(index);
}

/*
 * 2 and 3. Every docs/ subdirectory has a row in BOTH indexes.
 *
 * Both, not either: docs/README.md is what a reader browsing the tree
 * finds, and CLAUDE.md's Documentation Map is what assigns the
 * directory an owner and a mutability rule. docs/brand/ really did sit
 * in one and not the other, which is why this checks them separately
 * rather than assuming they agree.
 */
static void check_directory_indexes(void)
{
	char *docs_index = slurp("docs/README.md");
	char *map = slurp("CLAUDE.md");
	DIR *d;
	struct dirent *e;
	int checked = 0;

	if (docs_index == NULL || map == NULL) {
		fail("docs/README.md or CLAUDE.md is unreadable -- both indexes must exist");
		free(docs_index);
		free(map);
		return;
	}
	d = opendir("docs");
	if (d == NULL) {
		fail("docs/ is unreadable");
		free(docs_index);
		free(map);
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[512];
		char needle_docs[256];
		char needle_map[256];
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "docs/%s", e->d_name);
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;

		/* Searched as a link target ("dir/") rather than a bare name,
		 * so a directory mentioned in passing in prose cannot satisfy
		 * the check. */
		snprintf(needle_docs, sizeof(needle_docs), "(%s/)", e->d_name);
		snprintf(needle_map, sizeof(needle_map), "docs/%s/", e->d_name);

		if (strstr(docs_index, needle_docs) == NULL)
			fail("docs/%s/ has no row in docs/README.md (#201)", e->d_name);
		if (strstr(map, needle_map) == NULL)
			fail("docs/%s/ has no row in CLAUDE.md's Documentation Map -- \"not in the map\" "
			     "is already documented there as an invalid state (#201)",
			     e->d_name);
		checked++;
	}
	closedir(d);
	printf("  directory indexes: %d docs/ subdirectories checked\n", checked);
	free(docs_index);
	free(map);
}

/*
 * 4. Every guide is reachable from the guides index.
 *
 * Same property one level down, and the same failure mode: a guide
 * nobody links to is a guide nobody finds, which is indistinguishable
 * from one that was never written.
 */
static void check_guides_index(void)
{
	char *index = slurp("docs/guides/README.md");
	DIR *d;
	struct dirent *e;
	int listed = 0;

	if (index == NULL) {
		fail("docs/guides/README.md is unreadable -- the guides index must exist");
		return;
	}
	d = opendir("docs/guides");
	if (d == NULL) {
		fail("docs/guides is unreadable");
		free(index);
		return;
	}
	while ((e = readdir(d)) != NULL) {
		size_t len = strlen(e->d_name);

		if (len < 4 || strcmp(e->d_name + len - 3, ".md") != 0)
			continue;
		if (strcmp(e->d_name, "README.md") == 0)
			continue;
		if (strstr(index, e->d_name) == NULL)
			fail("guide %s has no row in docs/guides/README.md (#201)", e->d_name);
		else
			listed++;
	}
	closedir(d);
	printf("  guides index: %d guides listed\n", listed);
	free(index);
}

int main(void)
{
	check_adr_index();
	check_directory_indexes();
	check_guides_index();

	if (g_failures > 0) {
		printf("DOCINDEX RESULT: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("DOCINDEX RESULT: PASS\n");
	return 0;
}
