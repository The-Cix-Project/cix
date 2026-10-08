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
 * Does this path name a document this tree owns? Used both to pick the
 * files the link check reads and to decide which link targets it is
 * willing to resolve.
 *
 * An extension list rather than "anything without a scheme",
 * deliberately: a link to a directory, to an issue, or to a source file
 * by line number is perfectly good prose and none of it is a document
 * whose absence this test can judge. Checking only these extensions is
 * what keeps the check free of the false positives that would get it
 * deleted.
 */
static int looks_like_document(const char *path)
{
	static const char *const exts[] = { ".md", ".yaml", ".svg", ".pub", ".asc", ".json" };
	size_t len = strlen(path);
	size_t i;

	for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		size_t elen = strlen(exts[i]);

		if (len > elen && strcmp(path + len - elen, exts[i]) == 0)
			return 1;
	}
	return 0;
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
 * The next endpoint-table row at or after *p, if any.
 *
 * Rows look like `| GET | `/path` | what it does |`, and the method
 * cell may name SEVERAL -- `| GET, PUT | `/images/{name}/policy` |`
 * covers a read/write pair in one row, which is a form the table uses
 * deliberately and is not drift. So this yields the cell verbatim and
 * `method_at()` walks it; a parser that recognised only a single method
 * silently skipped those rows, and skipping is the one failure a gate
 * must not have (measured: 2 rows, 4 operations, unchecked).
 *
 * Found by scanning cells rather than parsing markdown: the shape is
 * regular, and unlike source code (see test_api_surfaces' own reasoning
 * for why enumeration is unreliable THERE) a table row either matches
 * this shape or is not a row at all.
 *
 * A separate function from its two callers so the parser can be
 * exercised on a fixture holding rows it must reject and rows it must
 * skip. Without that, a gate over a correct document proves only that
 * it found nothing wrong -- which is also what a parser matching zero
 * rows reports.
 */
static int is_method_word(const char *w, size_t len)
{
	static const char *const methods[] = { "GET", "PUT", "POST", "DELETE",
	                                       "PATCH", "HEAD", NULL };
	int i;

	for (i = 0; methods[i] != NULL; i++)
		if (strlen(methods[i]) == len && strncmp(w, methods[i], len) == 0)
			return 1;
	return 0;
}

/*
 * The i-th method in a cell like "GET, PUT". Returns 0 once there are
 * no more, so a caller walks it with an ordinary for loop.
 */
static int method_at(const char *cell, int want, char *out, size_t out_size)
{
	const char *c = cell;
	int i = 0;

	while (*c != '\0') {
		const char *start;
		size_t len;

		while (*c == ' ' || *c == ',')
			c++;
		if (*c == '\0')
			break;
		start = c;
		while (*c != '\0' && *c != ' ' && *c != ',')
			c++;
		len = (size_t)(c - start);
		if (i++ != want)
			continue;
		if (len >= out_size)
			return 0;
		memcpy(out, start, len);
		out[len] = '\0';
		return 1;
	}
	return 0;
}

/* Is every word in the cell an HTTP method? A cell of prose is not a
 * row this gate has anything to say about. */
static int cell_is_methods(const char *cell)
{
	const char *c = cell;
	int words = 0;

	while (*c != '\0') {
		const char *start;

		while (*c == ' ' || *c == ',')
			c++;
		if (*c == '\0')
			break;
		start = c;
		while (*c != '\0' && *c != ' ' && *c != ',')
			c++;
		if (!is_method_word(start, (size_t)(c - start)))
			return 0;
		words++;
	}
	return words > 0;
}

static int next_endpoint_row(const char **p, char *methods, size_t methods_size, char *path,
                             size_t path_size)
{
	while ((*p = strchr(*p, '\n')) != NULL) {
		const char *q, *bar, *tick, *end;
		size_t n;

		(*p)++;
		if (**p != '|')
			continue;
		end = strchr(*p, '\n');
		/* First cell: the method or methods. */
		q = *p + 1;
		bar = strchr(q, '|');
		if (bar == NULL || (end != NULL && bar > end))
			continue;
		n = (size_t)(bar - q);
		while (n > 0 && (q[0] == ' ')) {
			q++;
			n--;
		}
		while (n > 0 && q[n - 1] == ' ')
			n--;
		if (n == 0 || n >= methods_size)
			continue;
		memcpy(methods, q, n);
		methods[n] = '\0';
		if (!cell_is_methods(methods))
			continue;
		/* Second cell: the path, in backticks, on this same line. */
		tick = strchr(bar, '`');
		if (tick == NULL || (end != NULL && tick > end))
			continue;
		tick++;
		q = strchr(tick, '`');
		if (q == NULL || (end != NULL && q > end))
			continue;
		n = (size_t)(q - tick);
		if (n == 0 || n >= path_size || tick[0] != '/')
			continue;
		memcpy(path, tick, n);
		path[n] = '\0';
		return 1;
	}
	return 0;
}

#define CIX_API_SHAPE_COUNT ((int)(sizeof(cix_api_shapes) / sizeof(cix_api_shapes[0])))

/*
 * Which operation this method and path name, or -1.
 *
 * Exact, parameter names included -- `path_raw` is the contract's own
 * spelling. A row naming `{disk_name}` where the contract says `{name}`
 * is a real defect for anyone copying a path out of the table, and it
 * was part of the drift #576 reports.
 *
 * Returns the index rather than a yes/no so the caller can record WHICH
 * operations the table covered, which is what the reverse direction
 * needs.
 */
static int contract_index(const char *method, const char *bare_path)
{
	char want[280];
	int i;

	snprintf(want, sizeof(want), "/v1%s", bare_path);
	for (i = 0; i < CIX_API_SHAPE_COUNT; i++) {
		if (strcmp(cix_api_shapes[i].method, method) == 0 &&
		    strcmp(cix_api_shapes[i].path_raw, want) == 0)
			return i;
	}
	return -1;
}

/*
 * The parser and the refusal, on a fixture (#576).
 *
 * A gate run only over a correct document cannot tell "nothing is
 * wrong" from "I parsed nothing" -- both report no failures. This tells
 * them apart, and it needs no daemon, no box and no probe cycle.
 *
 * SIX rows yield SEVEN (method, path) pairs; every other line in the
 * fixture must yield none.
 *
 * Six of those seven pairs name real operations: `/health`,
 * `/pkg/{name}`, the two-parameter volumes delete, and BOTH halves of a
 * `| GET, PUT |` row -- which is the case that mattered. The real table
 * carries exactly two such rows (`GET, POST /whoami/app-passwords` and
 * `GET, PUT /images/{name}/policy`, measured 2026-10-07); a parser
 * recognising only a single method skipped both, so four operations went
 * unchecked while the gate reported clean. A second `/health` row
 * carries a backtick in its PURPOSE cell, which must not be mistaken
 * for the path -- the real table has rows like that. The seventh pair
 * names a path the contract does not declare and must be refused.
 *
 * What must yield nothing: the markdown header and separator lines, two
 * rows with no path cell, and one with prose in the method cell.
 *
 * The two with no path cell sit deliberately BEFORE the backticked ones.
 * Such a row is skipped because the backtick search runs past the end of
 * its line, which is guarded by comparing the backtick position against
 * the newline -- place them last instead and the search finds no
 * backtick at all, so the guard goes unexercised and the test passes for
 * the wrong reason.
 *
 * The six good pairs are spelled exactly as the contract spells them,
 * so if any of those operations is renamed this fixture fails too. That
 * is deliberate: it would mean the real table needs the same edit.
 */
static void check_endpoint_row_parser(void)
{
	static const char fixture[] =
	    "\n"
	    "| Method | Path | Purpose |\n"
	    "|---|---|---|\n"
	    "| GET | `/health` | liveness |\n"
	    "| GET | not a path cell | SKIPPED, no backticks -- and before the backticked\n"
	    "| PUT | still not a path cell | rows on purpose, so the parser has to skip\n"
	    "| GET | `/pkg/{name}` | one path parameter |\n"
	    "| DELETE | `/containers/{name}/volumes/{volume_name}` | two of them |\n"
	    "| GET, PUT | `/images/{name}/policy` | a read/write pair in ONE row |\n"
	    "| Deliberately | `/health` | SKIPPED: the first cell is not a method |\n"
	    "| GET | `/health` | a backtick in the PURPOSE cell: the `status` field |\n"
	    "| GET | `/no/such/endpoint` | deliberately absent from the contract |\n";
	const char *p = fixture;
	char methods[64], method[12], path[256];
	int pairs = 0, declared = 0, refused = 0;

	while (next_endpoint_row(&p, methods, sizeof(methods), path, sizeof(path))) {
		int i;

		for (i = 0; method_at(methods, i, method, sizeof(method)); i++) {
			pairs++;
			if (contract_index(method, path) >= 0)
				declared++;
			else
				refused++;
		}
	}
	/*
	 * Six (method, path) pairs: /health, /pkg/{name}, the volumes
	 * delete, BOTH halves of the GET,PUT policy row, and the absent
	 * one. Two rows with no path cell and one whose first cell is prose
	 * must contribute none.
	 */
	if (pairs != 7)
		fail("the endpoint-row parser found %d (method, path) pair(s) in a fixture "
		     "holding 7 -- three of its lines must be skipped and one row must yield TWO "
		     "pairs, which is the case a single-method parser silently dropped (#576)",
		     pairs);
	if (declared != 6)
		fail("the endpoint-row parser resolved %d of 6 fixture pairs naming real "
		     "operations; a gate that resolves nothing cannot tell a correct document "
		     "from one it failed to read",
		     declared);
	if (refused != 1)
		fail("the endpoint-row parser accepted `GET /no/such/endpoint`, which the "
		     "contract does not declare -- the refusal path is what makes this gate a "
		     "gate (#576)");
	printf("  api endpoint parser: %d pair(s), %d resolved, %d refused\n", pairs, declared,
	       refused);
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
 * change as any `openapi.yaml` edit, never after -- this is the rule
 * that was missing when it drifted 10 phases stale."
 *
 * The rule existed and the table drifted anyway: measured 2026-10-07,
 * 19 endpoints a reader could not call. Eight were `/disks*` paths
 * renamed to `/storage*` without the README following, its own index
 * rows among them; eleven were volume rows spelling `{name}` or
 * `{volume}` where the contract says `{volume_name}`. That is the same
 * argument this file's header already makes -- a rule cannot notice --
 * applied to the one index whose rows point into the contract rather
 * than at a file.
 *
 * BOTH DIRECTIONS, exactly, with no allow-list. Every row must resolve
 * to a real operation, and every operation must have a row. The two
 * catch different failures: the forward one catches an endpoint removed
 * or renamed, the reverse one catches a NEW endpoint nobody documented,
 * which is the failure this table will have next.
 *
 * Three numbers, measured 2026-10-07 and easy to confuse. 19 rows
 * named something that did not exist (fixed first). 4 more rows
 * went unchecked because the parser recognised only one method per
 * cell, so `| GET, PUT |` rows were skipped entirely. 15 operations
 * then had no row at all -- `GET /system/assembly`, `POST /config`,
 * `POST /pki/export`, `PUT /networks/{name}` and so on: ordinary
 * operator capabilities, not a category anyone had decided to leave
 * out. They were added rather than exempted, so the table is 339 of 339
 * and this check needs no exceptions.
 */
static void check_api_endpoint_index(void)
{
	static char covered[2048];
	char *doc = slurp("docs/api/README.md");
	const char *p;
	char methods[64], method[12], path[256];
	int pairs = 0, i;

	if (CIX_API_SHAPE_COUNT > (int)sizeof(covered)) {
		fail("the contract declares %d operations, more than this check can track -- "
		     "raise `covered` rather than let the reverse direction go unsound",
		     CIX_API_SHAPE_COUNT);
		return;
	}
	if (doc == NULL) {
		fail("docs/api/README.md is unreadable -- it is the narrative index into the "
		     "REST contract");
		return;
	}
	p = doc;
	while (next_endpoint_row(&p, methods, sizeof(methods), path, sizeof(path))) {
		int m;

		for (m = 0; method_at(methods, m, method, sizeof(method)); m++) {
			int idx = contract_index(method, path);

			pairs++;
			if (idx < 0)
				fail("docs/api/README.md's endpoint table offers `%s %s`, which the "
				     "contract does not declare -- a reader copying that path gets a "
				     "404 (#576)",
				     method, path);
			else
				covered[idx] = 1;
		}
	}
	/*
	 * The reverse direction: an operation the table does not name.
	 *
	 * This is the half that catches a NEW endpoint nobody documented,
	 * which is the failure this table will have next -- the forward
	 * direction only catches one that was removed or renamed.
	 *
	 * Exact, with no allow-list, because the table turned out to be a
	 * complete index by intent rather than a selection: it already held
	 * 324 of the 339 (method, path) pairs when this check was written,
	 * and the fifteen it lacked were ordinary operator capabilities --
	 * `GET /system/assembly`, `POST /config`, `POST /pki/export`,
	 * `PUT /networks/{name}` and so on -- not a category anyone had
	 * decided to leave out. So they were added rather than exempted,
	 * and the table is now 339 of 339. An allow-list here would be a
	 * place for the next fifteen to accumulate.
	 */
	for (i = 0; i < CIX_API_SHAPE_COUNT; i++) {
		if (covered[i])
			continue;
		fail("the contract declares `%s %s` (%s) and docs/api/README.md's endpoint table "
		     "has no row for it -- the table is a complete index, so a new operation "
		     "needs a row in the same change (#576)",
		     cix_api_shapes[i].method, cix_api_shapes[i].path_raw, cix_api_shapes[i].op_id);
	}
	/*
	 * A floor on the forward count, which names the PARSER.
	 *
	 * A parser that silently matched nothing reports no forward failure
	 * at all, and the reverse direction above then fails 339 times --
	 * loud, but every message says "the table has no row for it", which
	 * reads as 339 documentation bugs. This one line says which it is.
	 */
	if (pairs < 250)
		fail("only %d endpoint-table pair(s) were found in docs/api/README.md, against "
		     "339 measured on 2026-10-07 -- the parser stopped reading the table, and "
		     "any \"no row for it\" failures above are a consequence of that rather "
		     "than 339 separate documentation gaps",
		     pairs);
	printf("  api endpoint index: %d (method, path) pair(s), both directions against %d "
	       "declared operations\n",
	       pairs, CIX_API_SHAPE_COUNT);
	free(doc);
}

/*
 * 7. Every local link in every document resolves.
 *
 * check_adr_index() above already does this for the ADR index's own
 * rows. Nothing did it for links INSIDE documents, and on 2026-10-08 an
 * audit found TEN broken ones that had accumulated silently -- eight
 * ADRs citing each other by a filename the file never had
 * (`0305-a-recipes-filename-is-its-format.md` for
 * `0305-a-recipes-format-is-its-filename.md`: the right words in the
 * wrong order), one citing ADR-0155 by a title it never carried, and a
 * CHANGELOG entry naming ADR-0215 as `a-boot-manager-of-our-own`.
 *
 * Every one of them was written by someone who knew which ADR they
 * meant and guessed at its filename, which is exactly the failure a
 * machine should catch instead of a reader. A document's links are part
 * of what it claims.
 *
 * FENCED BLOCKS ARE SKIPPED, and that is not a convenience: 0000-adr-
 * process.md's worked example of the ADR format contains
 * `[ADR-0099](0099-something.md)`, which is an illustration and must
 * stay. A checker that flagged it would be wrong, and the first thing
 * anyone would do is delete the check.
 *
 * Only targets with a document extension are checked. A link to a
 * directory, an anchor, or anything external is not this test's
 * business -- external links cannot be resolved offline, which is the
 * same line check_adr_index() already draws.
 */
static void check_links_in_file(const char *path)
{
	char *buf = slurp(path);
	char dir[512];
	char target[512];
	char resolved[1100];
	const char *slash;
	char *line, *next;
	int fence = 0;

	if (buf == NULL) {
		fail("%s: unreadable -- it is in this test's own list of documents", path);
		return;
	}

	slash = strrchr(path, '/');
	if (slash == NULL)
		snprintf(dir, sizeof(dir), ".");
	else
		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);

	for (line = buf; line != NULL && *line != '\0'; line = next) {
		const char *p;

		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';

		/* A fence toggles, and its own line is never scanned. */
		if (strncmp(line, "```", 3) == 0) {
			fence = !fence;
			continue;
		}
		if (fence)
			continue;

		for (p = line; (p = strstr(p, "](")) != NULL; p += 2) {
			const char *start = p + 2;
			const char *end = strchr(start, ')');
			const char *hash;
			size_t len;

			if (end == NULL)
				break;
			len = (size_t)(end - start);
			if (len == 0 || len >= sizeof(target))
				continue;
			memcpy(target, start, len);
			target[len] = '\0';

			/* An anchor on the end is not part of the path. */
			hash = strchr(target, '#');
			if (hash != NULL)
				target[hash - target] = '\0';
			if (target[0] == '\0' || target[0] == '#')
				continue;
			/* External (scheme:) or absolute: not checkable here. */
			if (strchr(target, ':') != NULL || target[0] == '/')
				continue;
			if (!looks_like_document(target))
				continue;

			snprintf(resolved, sizeof(resolved), "%s/%s", dir, target);
			if (!file_exists(resolved))
				fail("%s links to %s, which does not exist", path, target);
		}
	}

	free(buf);
}

static void check_doc_links(void)
{
	static const char *const root_docs[] = { "README.md",       "CHANGELOG.md", "CLAUDE.md",
		                                  "CONTRIBUTING.md", "SECURITY.md",  "TRADEMARK.md" };
	DIR *d;
	struct dirent *de;
	size_t i;
	int files = 0;

	for (i = 0; i < sizeof(root_docs) / sizeof(root_docs[0]); i++) {
		check_links_in_file(root_docs[i]);
		files++;
	}

	d = opendir("docs");
	if (d == NULL) {
		fail("docs/ is unreadable");
		return;
	}
	while ((de = readdir(d)) != NULL) {
		char path[512];
		struct stat st;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "docs/%s", de->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISREG(st.st_mode)) {
			if (looks_like_document(de->d_name)) {
				check_links_in_file(path);
				files++;
			}
			continue;
		}
		if (!S_ISDIR(st.st_mode))
			continue;
		{
			DIR *sub = opendir(path);
			struct dirent *se;

			if (sub == NULL)
				continue;
			while ((se = readdir(sub)) != NULL) {
				char spath[600];

				if (se->d_name[0] == '.' || !looks_like_document(se->d_name))
					continue;
				snprintf(spath, sizeof(spath), "%s/%s", path, se->d_name);
				if (file_exists(spath)) {
					check_links_in_file(spath);
					files++;
				}
			}
			closedir(sub);
		}
	}
	closedir(d);

	printf("  doc links: %d documents scanned\n", files);
}

/*
 * 8. The architecture document names every component that exists
 *    (ADR-0334, #582).
 *
 * This is the gate the hand-laid-out SVG made impossible, and its
 * absence is why that diagram went 34 ADRs stale while saying so in its
 * own title ("as of Part 92", against a ROADMAP past 270). Whether a
 * picture is current was a judgement nobody made; against a text source
 * it is a string match.
 *
 * WHAT IT CHECKS, and the limit is as important as the check: that a
 * component with a daemon source file of its own is NAMED in the
 * document. Not that the arrows round it are right, not that the shape
 * is accurate -- neither is mechanically checkable, and a gate
 * pretending to check them would pass while the thing it names was
 * wrong. A subsystem with no source file of its own is invisible here
 * too. What it catches is the failure that actually happened: a whole
 * subsystem shipping and the picture never hearing about it.
 *
 * The pairing is explicit rather than derived from the filename,
 * because the two vocabularies differ on purpose -- `pkgrepo.c` is
 * "Package repositories" to a reader, and a check that demanded the
 * word "pkgrepo" in prose would be enforcing the code's names on the
 * documentation. Adding a subsystem means adding a row here, which is
 * the same deliberate-edit-in-a-diff device test_apigen's operation
 * count and ADR-0224's gcc recipe count already use.
 */
static void check_architecture_components(void)
{
	static const struct {
		const char *source; /* daemon/src/<source> -- the component exists if this does */
		const char *names;  /* ... then the document must contain this */
	} components[] = {
		{ "scheduler.c", "Scheduler" },
		{ "pkgsource.c", "Recipe sources" },
		{ "pkgrepo.c", "Package repositories" },
		{ "catalogue.c", "catalogue" },
		{ "upstreamkeys.c", "Upstream" },
		{ "pkgbad.c", "roll back" },
		{ "registry.c", "Registry" },
		{ "network.c", "Network" },
		{ "dns.c", "DNS" },
		{ "pki.c", "PKI" },
		{ "ldap.c", "LDAP" },
		{ "ntp.c", "NTP" },
		{ "volume.c", "volume" },
		{ "logstore.c", "Logs" },
		{ "hostauth.c", "authorize_route" },
		{ "procfuse.c", "procfuse" },
		{ "esp.c", "ESP" },
	};
	char *doc = slurp("docs/architecture/architecture.md");
	size_t i;
	int checked = 0;

	if (doc == NULL) {
		fail("docs/architecture/architecture.md is unreadable -- the architecture diagram must "
		     "exist, and since ADR-0334 it is this file rather than an SVG");
		return;
	}

	/*
	 * The diagram itself, not just the prose. A document that described
	 * every component in sentences and drew none of them would satisfy
	 * a bare substring search while being no diagram at all.
	 */
	if (strstr(doc, "```mermaid") == NULL)
		fail("docs/architecture/architecture.md carries no ```mermaid block -- ADR-0334 made the "
		     "diagram a fenced mermaid source precisely so it renders in the forge");

	for (i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
		char path[256];

		snprintf(path, sizeof(path), "daemon/src/%s", components[i].source);
		if (!file_exists(path))
			continue; /* component gone: nothing to require */
		if (strstr(doc, components[i].names) == NULL)
			fail("daemon/src/%s exists but docs/architecture/architecture.md never says \"%s\" "
			     "-- a subsystem shipped and the architecture picture did not hear about it "
			     "(ADR-0334)",
			     components[i].source, components[i].names);
		else
			checked++;
	}

	/*
	 * The gate must not pass vacuously, and it cannot report that it
	 * did not. Every name check above is conditional on its source
	 * file existing, so a tree carrying the document but not
	 * daemon/src would skip all seventeen and report success having
	 * compared nothing -- and the count printed below is invisible
	 * where it matters: `make selftest` prints one PASS/FAIL line
	 * per binary and none of its stdout (measured in
	 * probe-cix-compile@0.2.57-498's log, 2026-10-08: 314 lines, zero
	 * of them a test's own output). So the count is asserted here
	 * rather than left for a reader who will never see it.
	 */
	if (checked == 0)
		fail("docs/architecture/architecture.md was read, and not one component source in "
		     "daemon/src exists -- this gate compared nothing, so its success means nothing "
		     "(ADR-0334)");

	printf("  architecture: %d components named\n", checked);
	free(doc);
}

int main(void)
{
	check_adr_index();
	check_adr_headers();
	check_directory_indexes();
	check_guides_index();
	check_endpoint_row_parser();
	check_api_endpoint_index();
	check_doc_links();
	check_architecture_components();

	if (g_failures > 0) {
		printf("DOCINDEX RESULT: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("DOCINDEX RESULT: PASS\n");
	return 0;
}
