/*
 * test_naming -- the build system has ONE name, and it is CBS.
 *
 * For a long time this tree called the same thing three things: CBS
 * (the upstream build system, cix-build-system), CPDL (its recipe
 * language), and "PBS" -- a tag that appeared in the REST contract as
 * `"format": "pbs"`, spread into ~450 identifiers, comments, docs and
 * changelog entries, and was never expanded anywhere. Nothing in the
 * tree said what the letters stood for, because nothing could: it was
 * a third name for a thing that already had two.
 *
 * The owner's instruction, 2026-09-26: *"It's called CBS. Right? Cix
 * build system? ... This is a parallel I'm uninterested in, let's not
 * diverge right? One source of truth, no parallels unless we have
 * legitimate need, then we discuss."*
 *
 * That is the No Parallel Implementations maxim applied to a NAME, and
 * a name is exactly the kind of parallel that reappears quietly: one
 * new field, one comment, one doc paragraph written by someone who
 * read an old ADR. Prose cannot hold this. A gate can, which is the
 * same reasoning ADR-0224 gives for counting gcc recipes rather than
 * preferring TCC in words.
 *
 * TWO SPELLINGS ARE ALLOWED, and both are proper nouns rather than
 * uses of the name:
 *
 *   - `PR_CAPBSET_DROP`, the Linux prctl. It contains "PBS" by
 *     coincidence (CA-PBS-ET) and renaming it would break the build.
 *     This is the reason the rename could not be a bare sed, and the
 *     reason this check strips rather than matching whole words.
 *   - `probe-pbs`, `probe-pbs-caps`, `probe-pbs-targz`,
 *     `probe-approve-pbs`. These are PUBLISHED package names. A
 *     published version is immutable, so the artifacts really are
 *     called that, and a document naming one is stating a fact about
 *     something that exists. Renaming those references would make the
 *     documents wrong.
 *
 * Anything else containing "pbs" in any case is a regression.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The same shape test_lint uses: shell globs expanded by the shell, so
 * this needs no directory walker of its own. Deliberately WIDER than
 * test_lint's, which covers product C only -- a name leaks through
 * documentation and tests at least as readily as through code, and the
 * original spread proves it: 56 occurrences were in CHANGELOG.md and
 * 19 in the OpenAPI contract.
 */
static const char *const g_globs[] = {
	"daemon/src/*.c",  "daemon/include/*.h", "src/*.c",        "include/*.h",
	"netplane/src/*.c", "image/src/*.c",     "tools/*.c",      "cli/src/*.c",
	"client/src/*.c",  "init/src/*.c",       "test/*.c",       "test/*.h",
	"docs/*.md",       "docs/api/*.yaml",    "docs/api/*.md",  "docs/adr/*.md",
	"docs/guides/*.md", "docs/roadmap/*.md", "docs/mission/*.md",
	"web/*.js",        "*.md",               "Makefile",
};
#define NGLOBS ((int)(sizeof(g_globs) / sizeof(g_globs[0])))

/* The allowed spellings, longest first so that stripping
 * `probe-approve-pbs` happens before `probe-pbs` could match part of
 * it, and `PR_CAPBSET_DROP` before the bare `CAPBSET`. */
static const char *const g_allowed[] = {
	"PR_CAPBSET_DROP", "CAPBSET",
	"probe-approve-pbs", "probe-pbs-targz", "probe-pbs-caps", "probe-pbs",
};
#define NALLOWED ((int)(sizeof(g_allowed) / sizeof(g_allowed[0])))

/* Case-insensitive search, since the regression could be "Pbs" as
 * readily as "pbs" or "PBS". */
static char *find_ci(char *hay, const char *needle)
{
	size_t nlen = strlen(needle);
	char *p;

	for (p = hay; *p != '\0'; p++) {
		size_t i;

		for (i = 0; i < nlen; i++) {
			char a = p[i];
			char b = needle[i];

			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = (char)(b - 'A' + 'a');
			if (a != b)
				break;
		}
		if (i == nlen)
			return p;
	}
	return NULL;
}

/*
 * Blank out every allowed spelling, then look at what is left. This
 * is why the check strips rather than tokenising: `PR_CAPBSET_DROP`
 * hides "PBS" INSIDE a word, so no word-boundary rule can tell it
 * apart from a real use.
 */
static int line_has_stray(char *line)
{
	int i;

	for (i = 0; i < NALLOWED; i++) {
		char *at;

		while ((at = find_ci(line, g_allowed[i])) != NULL)
			memset(at, ' ', strlen(g_allowed[i]));
	}
	return find_ci(line, "pbs") != NULL;
}

int main(void)
{
	char cmd[2048];
	char line[8192];
	int strays = 0;
	int i;

	for (i = 0; i < NGLOBS; i++) {
		FILE *p;

		/*
		 * This file is skipped, and it has to be: a gate that
		 * forbids a string must contain that string to search for
		 * it, so scanning itself would make it permanently red. The
		 * exclusion is by exact path rather than by any cleverness
		 * (splitting the literal, encoding it) precisely so that it
		 * stays obvious to the next reader that ONE file is exempt
		 * and which one.
		 */
		snprintf(cmd, sizeof(cmd),
		         "for f in %s; do [ -e \"$f\" ] || continue; "
		         "[ \"$f\" = test/test_naming.c ] && continue; "
		         "grep -Hni pbs \"$f\" 2>/dev/null; done",
		         g_globs[i]);
		p = popen(cmd, "r");
		if (p == NULL) {
			printf("NAMING RESULT: FAIL (cannot scan %s)\n", g_globs[i]);
			return 1;
		}
		while (fgets(line, sizeof(line), p) != NULL) {
			char copy[8192];

			snprintf(copy, sizeof(copy), "%s", line);
			if (line_has_stray(copy)) {
				/* Print the ORIGINAL, not the blanked copy. */
				fputs(line, stdout);
				strays++;
			}
		}
		pclose(p);
	}

	if (strays != 0) {
		printf("NAMING RESULT: FAIL (%d line(s) still say PBS -- the build system "
		       "is called CBS, and the only allowed spellings are PR_CAPBSET_DROP "
		       "and the published probe-pbs* package names)\n",
		       strays);
		return 1;
	}
	printf("NAMING RESULT: PASS\n");
	return 0;
}
