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

/* The generated route table (#576): what the contract actually declares,
 * so the endpoint index below is compared against the spec rather than
 * against a second hand-written list. */
#include "generated/api_shapes.h"

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
/*
 * NNNN-slug.md, and never README.md itself -- the index does not index
 * itself. Named once because two checks ask it (#201's index coverage
 * and the header schema below), and two copies of "what counts as an
 * ADR file" is exactly the kind of second definition this file exists
 * to prevent.
 */
static int is_adr_filename(const char *name)
{
	size_t len = strlen(name);

	if (len < 8 || strcmp(name + len - 3, ".md") != 0)
		return 0;
	return name[0] >= '0' && name[0] <= '9';
}

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
		if (!is_adr_filename(e->d_name))
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

/*
 * Every ADR presents itself the same way (#357-adjacent doc audit).
 *
 * This corpus drifted into two formats and nobody noticed: seventeen
 * files used "# ADR-NNNN: Title" against two hundred and fifty using
 * "# NNNN — Title", and seven of those additionally carried Status,
 * Date and Issue as a bullet list instead of a "## Status" heading.
 * Neither form was wrong. They were a second way of saying the same
 * thing, which is what One Source of Truth forbids of a document set
 * as much as of a registry -- and a reader should never have to work
 * out which shape they are looking at before they can find the status.
 *
 * Checked here rather than left to care for the reason the file-level
 * comment above already gives: the drift happened while the rule
 * existed, and a rule cannot notice.
 *
 * Deliberately narrow. The H1 must have the canonical shape, a
 * "## Status" heading must exist, and its first word must be a status
 * from ADR-0000's declared set. What follows that word is prose and is
 * not checked -- "Accepted; phased." and "Accepted. Supersedes
 * ADR-0123" both say something a bare token cannot, and a gate that
 * rejected them would be asking documents to be less accurate.
 */
static void check_adr_headers(void)
{
	DIR *d = opendir("docs/adr");
	struct dirent *e;

	if (d == NULL) {
		fail("docs/adr is unreadable");
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[512];
		char *text;
		const char *status;
		size_t n;

		if (!is_adr_filename(e->d_name))
			continue;
		snprintf(path, sizeof(path), "docs/adr/%s", e->d_name);
		text = slurp(path);
		if (text == NULL) {
			fail("%s is unreadable", path);
			continue;
		}

		/* H1: "# NNNN — Title", the number matching the filename. */
		if (strncmp(text, "# ", 2) != 0 || strncmp(text + 2, e->d_name, 4) != 0 ||
		    strncmp(text + 6, " \xe2\x80\x94 ", 5) != 0)
			fail("%s: first line must be \"# %.4s \xe2\x80\x94 Title\" (see docs/adr/0000-adr-process.md)",
			      path, e->d_name);

		/* A "## Status" heading, and a recognised status as its first word. */
		status = strstr(text, "\n## Status\n");
		if (status == NULL) {
			fail("%s: no \"## Status\" heading -- a bullet list is the format this corpus "
			      "drifted into and no longer accepts", path);
			free(text);
			continue;
		}
		status += strlen("\n## Status\n");
		while (*status == '\n' || *status == ' ')
			status++;
		n = strcspn(status, " \n.,;:");
		if (!(n == 8 && strncmp(status, "Accepted", 8) == 0) &&
		    !(n == 8 && strncmp(status, "Proposed", 8) == 0) &&
		    !(n == 10 && strncmp(status, "Superseded", 10) == 0) &&
		    !(n == 10 && strncmp(status, "Deprecated", 10) == 0))
			fail("%s: status starts with \"%.*s\" -- must be Proposed, Accepted, "
			      "Superseded or Deprecated (qualifying prose after it is fine)",
			      path, (int)n, status);
		free(text);
		}
	closedir(d);
}


/*
 * docs/api/README.md's endpoint table names only endpoints that exist
 * (#576).
 *
 * That table is a hand-maintained copy of the list openapi.yaml owns,
 * and until this check nothing compared them. ADR-0218's
 * contract-to-CODE link is gated three times over -- test_apiroute on
 * routing, test_apigen on operations and permissions, test_api_surfaces
 * on channels inventing paths -- while the contract-to-PROSE link had
 * only a rule in CLAUDE.md's Documentation Map: "Updated in the same
 * change as any openapi.yaml edit, never after -- this is the rule that
 * was missing when it drifted 10 phases stale."
 *
 * The rule existed and the table drifted anyway: measured 2026-10-07,
 * 23 references to 8 endpoints that 404, because the /disks* paths were
 * renamed to /storage* and the README was not. Its own index rows
 * offered `GET /disks` and `POST /disks/{disk_name}/format`. That is
 * the same argument this file's header already makes -- a rule cannot
 * notice -- applied to the one index whose rows point into the contract
 * rather than at a file.
 *
 * ONE DIRECTION ONLY, deliberately. Every row must resolve to a real
 * operation; an operation with no row is NOT a failure here. The
 * reverse direction is the more valuable one and needs a judgement this
 * gate should not make on its own: 339 operations against a table that
 * legitimately groups some, and a mechanical demand for 339 rows would
 * push the document towards being a worse version of the spec. #576
 * carries it.
 *
 * The comparison is exact, method and path together, parameter names
 * included -- `path_raw` is the contract's own spelling. A row naming
 * `{disk_name}` where the contract says `{name}` is a real defect for
 * anyone copying a path out of the table, and it was part of this
 * drift.
 */
static void check_api_endpoint_index(void)
{
	char *doc = slurp("docs/api/README.md");
	const char *p;
	int rows = 0;

	if (doc == NULL) {
		fail("docs/api/README.md is unreadable -- it is the narrative index into the "
		     "REST contract");
		return;
	}
	/*
	 * Rows look like `| GET | `/path` | what it does |`. Found by
	 * scanning for the method inside the first cell rather than by
	 * parsing markdown: the shape is regular, and unlike source code
	 * (see test_api_surfaces' own reasoning for why enumeration is
	 * unreliable THERE) a table row either matches this shape or is
	 * not a row at all.
	 */
	for (p = doc; (p = strchr(p, '\n')) != NULL; ) {
		static const char *const methods[] = { "GET", "PUT", "POST", "DELETE",
		                                       "PATCH", "HEAD", NULL };
		char method[12], path[256], want[280];
		const char *q, *tick, *end;
		size_t n;
		int m, i, found = 0;

		p++;
		if (*p != '|')
			continue;
		q = p + 1;
		while (*q == ' ')
			q++;
		for (m = 0; methods[m] != NULL; m++) {
			size_t len = strlen(methods[m]);

			if (strncmp(q, methods[m], len) == 0 &&
			    (q[len] == ' ' || q[len] == '|'))
				break;
		}
		if (methods[m] == NULL)
			continue;
		snprintf(method, sizeof(method), "%s", methods[m]);
		/* The path is the next cell, in backticks. */
		q = strchr(q, '|');
		if (q == NULL)
			continue;
		tick = strchr(q, '`');
		end = strchr(q, '\n');
		if (tick == NULL || (end != NULL && tick > end))
			continue;
		tick++;
		q = strchr(tick, '`');
		if (q == NULL || (end != NULL && q > end))
			continue;
		n = (size_t)(q - tick);
		if (n == 0 || n >= sizeof(path) || tick[0] != '/')
			continue;
		memcpy(path, tick, n);
		path[n] = '\0';
		rows++;
		snprintf(want, sizeof(want), "/v1%s", path);
		for (i = 0; i < (int)(sizeof(cix_api_shapes) / sizeof(cix_api_shapes[0])); i++) {
			if (strcmp(cix_api_shapes[i].method, method) == 0 &&
			    strcmp(cix_api_shapes[i].path_raw, want) == 0) {
				found = 1;
				break;
			}
		}
		if (!found)
			fail("docs/api/README.md's endpoint table offers `%s %s`, which the contract "
			     "does not declare -- a reader copying that path gets a 404 (#576)",
			     method, path);
	}
	printf("  api endpoint index: %d row(s) checked against the contract\n", rows);
	free(doc);
}
int main(void)
{
	check_adr_index();
	check_adr_headers();
	check_directory_indexes();
	check_guides_index();
	check_api_endpoint_index();

	if (g_failures > 0) {
		printf("DOCINDEX RESULT: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("DOCINDEX RESULT: PASS\n");
	return 0;
}
