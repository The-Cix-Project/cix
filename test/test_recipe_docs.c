/*
 * cix#491: no package recipe deletes its own documentation trees.
 *
 * ADR-0306 withdrew ADR-0251's clause 4, so the finalize phase keeps
 * usr/share/{man,info,doc,locale,i18n}. That restored nothing for 57
 * recipes that deleted those trees themselves (measured 2026-09-18),
 * licences included. The last three, glibc 2.44-20, gcc 16.2.0-18 and
 * node 24.21.0-3, stopped on 2026-10-01; this keeps the count at zero.
 *
 * Only the LATEST revision of each package is judged. A published
 * revision is immutable (ADR-0107), so glibc 2.44-19 deletes its man
 * pages forever and that is history, not a regression.
 *
 * What is refused is removing a whole tree: usr/share itself, or its
 * man, info, doc, locale or i18n directory, by `remove`, `remove tree`
 * or as an argument line of `run "rm"`. Removing one named file under
 * them is not refused, because a package giving up a single path
 * another package owns is how ADR-0319 is met (libblkid, libuuid and
 * login drop util-linux's terminal-colors.d.5 and util-linux.mo).
 *
 * The scan is a shell pipeline over the corpus, like test_naming's,
 * and for the same reasons: the corpus is another repository
 * (ADR-0308), present in a release build at CIX_RECIPES_DIR and absent
 * from a bare checkout, where this says it did not scan rather than
 * passing quietly. Before trusting the pipeline on the corpus it is run
 * on a fixture of lines whose answers are known, so a pattern that
 * matches nothing on this platform's grep fails here rather than
 * passing every recipe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One recipe line that removes a documentation tree. Single-quoted in
 * the shell; it contains no single quote. */
#define DOC_TREE_REMOVAL                                                                    \
	"^[[:space:]]*(remove([[:space:]]+(tree|glob))*[[:space:]]+)?\"\\$\\{dest\\}/usr/share" \
	"(/(man|info|doc|locale|i18n))?/?(\\*)?\""

/* Runs cmd and returns how many lines it printed, echoing each after
 * prefix, or none when prefix is NULL; -1 if it could not be run. */
static int count_lines(const char *cmd, const char *prefix)
{
	FILE *p = popen(cmd, "r");
	char line[1024];
	int n = 0;

	if (p == NULL)
		return -1;
	while (fgets(line, sizeof(line), p) != NULL) {
		if (prefix == NULL) {
			n++;
			continue;
		}
		printf("%s%s", prefix, line);
		if (strchr(line, '\n') == NULL)
			printf("\n");
		n++;
	}
	if (pclose(p) == -1)
		return -1;
	return n;
}

static int fixture_check(void)
{
	static const char *const removes[] = {
		"        remove tree \"${dest}/usr/share/man\"",
		"        remove tree \"${dest}/usr/share/locale\"",
		"        remove \"${dest}/usr/share/doc\"",
		"        remove tree \"${dest}/usr/share\"",
		"            \"${dest}/usr/share/info\"",
		"            \"${dest}/usr/share/i18n/\"",
		"        remove tree glob \"${dest}/usr/share/doc/*\"",
	};
	static const char *const keeps[] = {
		"            \"!\" \"-e\" \"${dest}/usr/share/man\"",
		"        remove glob \"${dest}/usr/share/man/man5/terminal-colors.d.5\"",
		"        remove glob \"${dest}/usr/share/locale/*/LC_MESSAGES/util-linux.mo\"",
		"        remove tree \"${dest}/usr/share/bash-completion\"",
		"            \"${dest}/usr/share/gcc-16.2.0\"",
		"        mkdir \"${dest}/usr/share/man\" parents",
	};
	char path[64], cmd[512];
	FILE *f;
	size_t i;
	int n, failures = 0;

	snprintf(path, sizeof(path), "/tmp/cix_recipe_docs_%d.cbs", (int)getpid());
	f = fopen(path, "w");
	if (f == NULL) {
		printf("  FAIL: cannot write the fixture %s\n", path);
		return 1;
	}
	for (i = 0; i < sizeof(removes) / sizeof(removes[0]); i++)
		fprintf(f, "%s\n", removes[i]);
	for (i = 0; i < sizeof(keeps) / sizeof(keeps[0]); i++)
		fprintf(f, "%s\n", keeps[i]);
	fclose(f);

	snprintf(cmd, sizeof(cmd), "grep -nE '%s' %s", DOC_TREE_REMOVAL, path);
	n = count_lines(cmd, "  fixture match: ");
	unlink(path);
	if (n != (int)(sizeof(removes) / sizeof(removes[0]))) {
		printf("  FAIL: the pattern matched %d fixture line(s); lines 1-%d remove a "
		       "documentation tree and the rest do not\n",
		       n, (int)(sizeof(removes) / sizeof(removes[0])));
		failures++;
	}
	return failures;
}

int main(void)
{
	const char *env = getenv("CIX_RECIPES_DIR");
	const char *root = (env != NULL && env[0] != '\0') ? env : "../cix-recipes/recipes";
	char dir[512], cmd[2048];
	int failures, scanned, found;

	printf("test_recipe_docs\n");
	failures = fixture_check();
	if (failures != 0) {
		printf("RECIPE DOCS RESULT: FAIL (the pattern is wrong; the corpus was not judged)\n");
		return 1;
	}

	snprintf(dir, sizeof(dir), "%s/package", root);
	if (access(dir, R_OK) != 0) {
		printf("recipe docs: corpus not present at %s -- NOT scanned (set "
		       "CIX_RECIPES_DIR, or clone cix-recipes beside this repository, to "
		       "gate it)\n",
		       dir);
		printf("RECIPE DOCS RESULT: PASS (fixture only)\n");
		return 0;
	}

	/* The latest revision of each package, by sort -V over the version
	 * after the '@'. */
	snprintf(cmd, sizeof(cmd),
	         "cd '%s' && for n in $(ls *.cbs | sed 's/@.*//' | sort -u); do "
	         "ls \"$n\"@*.cbs | sort -V | tail -1; done",
	         dir);
	scanned = count_lines(cmd, NULL);
	if (scanned <= 0) {
		printf("RECIPE DOCS RESULT: FAIL (no package recipe found under %s)\n", dir);
		return 1;
	}

	snprintf(cmd, sizeof(cmd),
	         "cd '%s' && for n in $(ls *.cbs | sed 's/@.*//' | sort -u); do "
	         "grep -HnE '%s' \"$(ls \"$n\"@*.cbs | sort -V | tail -1)\"; done",
	         dir, DOC_TREE_REMOVAL);
	found = count_lines(cmd, "  FAIL: ");
	if (found < 0) {
		printf("RECIPE DOCS RESULT: FAIL (cannot scan %s)\n", dir);
		return 1;
	}
	if (found > 0) {
		printf("RECIPE DOCS RESULT: FAIL (%d line(s) in the latest revisions remove a "
		       "documentation tree; ADR-0306 keeps them, cix#491)\n",
		       found);
		return 1;
	}
	printf("RECIPE DOCS RESULT: PASS (%d latest revisions scanned)\n", scanned);
	return 0;
}
