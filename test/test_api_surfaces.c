/*
 * test_api_surfaces -- ADR-0218 layer 1, the negative check.
 *
 * A presentation channel must not build API paths of its own. This does
 * NOT try to enumerate which paths a channel uses: that was attempted
 * three times during the work that produced ADR-0218 and gave three
 * different answers, one of them supporting the confident and false
 * conclusion that the dashboard could not stop a container (it builds
 * that URL by string concatenation, which the extraction did not see).
 *
 * So the question is inverted. Enumerating what a channel builds is
 * unreliable; asserting that it builds NOTHING is trivial, because it
 * needs no understanding of the code at all -- just the absence of a
 * literal. That asymmetry is the whole design: an unreliable check
 * would be worth less than no check, and a reliable one is available
 * for the asking.
 *
 * What a violation looks like: someone types "/v1/containers/" + name
 * instead of using the generated constant. It compiles, it works today,
 * and it silently stops working the day the contract moves -- which is
 * exactly the drift #182's five-domain regrouping would cause at scale.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

/*
 * Scans one file for `/v1/` inside a string literal. Deliberately crude
 * -- it looks for the text, not for syntax -- because crudeness is what
 * makes it trustworthy here: there is no parse to get wrong. Comments
 * are the one place the text may legitimately appear (describing the
 * API, or recording a path that used to be built by hand), so a line
 * whose first non-space characters are a comment marker is skipped.
 */
static void scan(const char *path, const char *channel, const char *how)
{
	FILE *f = fopen(path, "r");
	char line[8192];
	int lineno = 0, hits = 0;

	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		failures++;
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p = line;
		const char *found;

		lineno++;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, "/*", 2) == 0 || strncmp(p, "*", 1) == 0 ||
		    strncmp(p, "//", 2) == 0)
			continue;
		/*
		 * A path being BUILT is a string literal that STARTS with
		 * /v1/. A path merely mentioned -- "POST /v1/system/iso needs
		 * both" in a help message -- appears mid-sentence and is not
		 * a path this code constructs. Checked structurally (does the
		 * literal begin here?) rather than by guessing at intent.
		 *
		 * The precision was not academic: the first version of this
		 * check flagged exactly such a help string, and the right
		 * response was to sharpen the rule rather than add an
		 * exception -- a check with a growing exception list stops
		 * being one.
		 */
		found = strstr(line, "\"/v1/");
		if (found == NULL)
			found = strstr(line, "`/v1/");
		if (found == NULL)
			continue;
		if (hits == 0)
			fprintf(stderr,
			        "FAIL: %s builds API paths by hand. Use the generated %s instead --\n"
			        "      a hand-typed path compiles and fails at runtime, and silently\n"
			        "      breaks the day the contract moves (ADR-0218).\n",
			        channel, how);
		fprintf(stderr, "      %s:%d: %.100s", path, lineno, line);
		hits++;
	}
	fclose(f);
	if (hits > 0)
		failures++;
}

int main(void)
{
	scan("cli/src/main.c", "the CLI", "CIX_API_<operationId> constants from build/generated/cix_api.h");

	if (failures == 0)
		printf("API SURFACES RESULT: PASS\n");
	else
		printf("API SURFACES RESULT: FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
