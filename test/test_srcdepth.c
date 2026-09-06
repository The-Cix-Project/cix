/*
 * ADR-0255's depth grammar: n-<lines>.<releases>.
 *
 * The fixture is kernel.org's real `stable` shape at the time this was
 * written, because the whole reason the grammar distinguishes `n-1` from
 * `n-0.1` is that on real data they differ by an entire release line.
 * A synthetic list with one line per major would not exercise the
 * distinction at all, and the bug this guards against -- collapsing the
 * two spellings into one meaning -- would pass.
 */
#include "srcdepth.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void fail(const char *what, const char *got, const char *want)
{
	fprintf(stderr, "  FAIL: %s -- got \"%s\", want \"%s\"\n", what, got, want);
	g_failures++;
}

/* kernel.org stable, as actually published: two live 7.x lines plus an
 * older one, several releases deep, deliberately NOT in sorted order. */
static const char *const KORG[] = {
	"7.1.12", "7.2.3", "7.0.31", "7.2.1", "7.1.13",
	"7.2.2", "7.0.30", "7.1.11", "7.0.29"
};
#define KORG_N (sizeof(KORG) / sizeof(KORG[0]))

static void expect_resolve(const char *depth, const char *want)
{
	char out[64];
	char err[256];
	int lines = -1, releases = -1;
	enum srcdepth_error e = srcdepth_parse(depth, &lines, &releases);

	if (e != SRCDEPTH_OK) {
		fail(depth, srcdepth_strerror(e), want);
		return;
	}
	e = srcdepth_resolve(KORG, KORG_N, lines, releases, out, sizeof(out), err, sizeof(err));
	if (e != SRCDEPTH_OK) {
		fail(depth, err, want);
		return;
	}
	if (strcmp(out, want) != 0)
		fail(depth, out, want);
	else
		printf("  %-8s -> %-8s ok\n", depth, out);
}

static void expect_parse_rejected(const char *s)
{
	int a, b;

	if (srcdepth_parse(s, &a, &b) == SRCDEPTH_OK) {
		fprintf(stderr, "  FAIL: \"%s\" should not parse as a depth\n", s);
		g_failures++;
	}
}

static void expect_resolve_fails(const char *depth, enum srcdepth_error want,
                                  const char *must_mention)
{
	char out[64];
	char err[256];
	int lines = 0, releases = 0;
	enum srcdepth_error e;

	if (srcdepth_parse(depth, &lines, &releases) != SRCDEPTH_OK) {
		fprintf(stderr, "  FAIL: \"%s\" should parse\n", depth);
		g_failures++;
		return;
	}
	e = srcdepth_resolve(KORG, KORG_N, lines, releases, out, sizeof(out), err, sizeof(err));
	if (e != want) {
		fprintf(stderr, "  FAIL: %s gave %s, wanted %s\n", depth, srcdepth_strerror(e),
		        srcdepth_strerror(want));
		g_failures++;
		return;
	}
	/* ADR-0255: a resolution failure must say WHY, not merely that. An
	 * empty or generic reason is the failure this asserts against. */
	if (err[0] == '\0' || strstr(err, must_mention) == NULL) {
		fprintf(stderr, "  FAIL: %s reason %s did not mention \"%s\"\n", depth, err,
		        must_mention);
		g_failures++;
		return;
	}
	if (out[0] != '\0') {
		fprintf(stderr, "  FAIL: %s failed but still wrote \"%s\"\n", depth, out);
		g_failures++;
		return;
	}
	printf("  %-8s -> refused: %s\n", depth, err);
}

int main(void)
{
	printf("test_srcdepth\n");

	/* The grammar, read literally. */
	expect_resolve("n", "7.2.3");      /* newest release in the channel */
	expect_resolve("n-0.1", "7.2.2");  /* same line, one release back */
	expect_resolve("n-0.2", "7.2.1");
	expect_resolve("n-1", "7.1.13");   /* previous LINE, newest of it */
	expect_resolve("n-1.1", "7.1.12");
	expect_resolve("n-2", "7.0.31");
	expect_resolve("n-0", "7.2.3");    /* n-0 is n */

	/*
	 * The distinction the grammar exists for. If these two ever produce
	 * the same answer, a box asked to sit one release back has been
	 * walked across a major version boundary instead -- which is the
	 * exact failure ADR-0193 hit with `longterm`.
	 */
	{
		char a[64], b[64], e[256];

		srcdepth_resolve(KORG, KORG_N, 0, 1, a, sizeof(a), e, sizeof(e)); /* n-0.1 */
		srcdepth_resolve(KORG, KORG_N, 1, 0, b, sizeof(b), e, sizeof(e)); /* n-1   */
		if (strcmp(a, b) == 0) {
			fprintf(stderr, "  FAIL: n-0.1 and n-1 both gave \"%s\"\n", a);
			g_failures++;
		} else {
			printf("  n-0.1 (%s) != n-1 (%s) ok\n", a, b);
		}
	}

	/* Syntax. */
	expect_parse_rejected("");
	expect_parse_rejected("x");
	expect_parse_rejected("n-");
	expect_parse_rejected("n-.");
	expect_parse_rejected("n-1.");
	expect_parse_rejected("n--1");
	expect_parse_rejected("n-1.2.3");
	expect_parse_rejected("n-1x");
	expect_parse_rejected("1");

	/* Unsatisfiable depths name what was asked and what exists. */
	expect_resolve_fails("n-9", SRCDEPTH_ERR_NO_SUCH_LINE, "3 line");
	expect_resolve_fails("n-0.9", SRCDEPTH_ERR_NO_SUCH_RELEASE, "7.2");

	/* Version parsing. */
	{
		struct srcdepth_version v = srcdepth_version_parse("6.18.40-24");

		if (!v.valid || v.major != 6 || v.minor != 18 || v.patch != 40) {
			fprintf(stderr, "  FAIL: recipe suffix not ignored\n");
			g_failures++;
		}
		v = srcdepth_version_parse("7.2");
		if (!v.valid || v.patch != 0) {
			fprintf(stderr, "  FAIL: two-component version\n");
			g_failures++;
		}
		v = srcdepth_version_parse("v7.2.3");
		if (!v.valid || v.major != 7) {
			fprintf(stderr, "  FAIL: leading v\n");
			g_failures++;
		}
		/* A prerelease is NOT the release it resembles. */
		v = srcdepth_version_parse("7.3-rc1");
		if (v.valid) {
			fprintf(stderr, "  FAIL: 7.3-rc1 accepted as a release\n");
			g_failures++;
		}
	}

	/* A duplicate must not make "one back" mean "the same one". */
	{
		static const char *const dup[] = { "7.2.3", "7.2.3", "7.2.2" };
		char out[64], err[256];

		if (srcdepth_resolve(dup, 3, 0, 1, out, sizeof(out), err, sizeof(err)) != SRCDEPTH_OK ||
		    strcmp(out, "7.2.2") != 0) {
			fprintf(stderr, "  FAIL: duplicate not collapsed (got \"%s\")\n", out);
			g_failures++;
		} else {
			printf("  duplicates collapsed ok\n");
		}
	}

	/* One malformed entry must not take the package offline. */
	{
		static const char *const messy[] = { "not-a-version", "7.2.3", "", "7.2.2" };
		char out[64], err[256];

		if (srcdepth_resolve(messy, 4, 0, 0, out, sizeof(out), err, sizeof(err)) != SRCDEPTH_OK ||
		    strcmp(out, "7.2.3") != 0) {
			fprintf(stderr, "  FAIL: malformed entry broke resolution (got \"%s\")\n", out);
			g_failures++;
		} else {
			printf("  malformed entries ignored ok\n");
		}
	}

	/* Nothing at all is a named failure, not a crash. */
	{
		char out[64], err[256];

		if (srcdepth_resolve(NULL, 0, 0, 0, out, sizeof(out), err, sizeof(err)) !=
		    SRCDEPTH_ERR_NO_CANDIDATES) {
			fprintf(stderr, "  FAIL: empty candidate set\n");
			g_failures++;
		}
	}

	if (g_failures != 0) {
		fprintf(stderr, "test_srcdepth: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_srcdepth: PASS\n");
	return 0;
}
