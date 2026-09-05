/*
 * test_lint -- a second opinion on our own C, from a second compiler.
 *
 * This project compiles with TCC and only TCC (ADR-0224), and TCC's
 * -Wall is thin. That is not a complaint about TCC; it is a fact with a
 * measured cost. A gcc -Wall -Wextra pass over the same sources, run
 * once as an audit, found ten real defects TCC had reported nothing
 * about across a -Wall -Werror build that was green the whole time:
 *
 *   - two int/size_t comparisons in containerdef.c, in a file whose own
 *     other loops already used size_t
 *   - four variables declared and never used, one of them left behind
 *     by a removal in the same session
 *   - a comment block indented as though it were guarded by the `if`
 *     above it, which is exactly how a reader mis-reads control flow
 *   - a struct initialised by position with a field silently left off
 *
 * None of those would have been found by a runtime test, because none
 * of them changes behaviour today. They are the class that becomes a
 * bug on the next edit, and the only thing that finds them is a
 * compiler that looks harder.
 *
 * So gcc is a LINTER here and never a producer: -fsyntax-only, no
 * object file, nothing that could reach an artifact. The Toolchain
 * Tenet is untouched -- TCC still compiles every byte this project
 * ships. See ADR-0248.
 *
 * gcc is invoked by ABSOLUTE PATH deliberately. A bare `gcc` resolved
 * through $PATH computes its own installation prefix as a relative
 * path and fails to find cc1 with a misleading "No such file or
 * directory" -- a documented property of this environment, not a
 * guess.
 *
 * -Wno-comment is the one suppression, and it is style rather than
 * substance: this codebase's comments quote code containing "/*",
 * which that warning objects to and nothing else does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GCC_BIN "/usr/bin/gcc"

/*
 * One include superset for every area rather than per-file sets. An
 * unused -I is harmless, and a per-file table is a second place to
 * remember a new directory in.
 */
#define INCLUDES \
	"-Iinclude -Idaemon/include -Inetplane/include -Itest -Icli/src " \
	"-Ibuild -Ibuild/generated"

#define FLAGS \
	"-fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-comment " \
	"-D_GNU_SOURCE -D_FORTIFY_SOURCE=0"

/*
 * The directories whose C this project owns and therefore must keep
 * warning-free. test/ is deliberately absent: a test fixture is
 * allowed to be blunt, and holding 117 files to this bar would make
 * the gate about the tests rather than about the product.
 */
static const char *const g_globs[] = {
	"daemon/src/*.c", "src/*.c", "netplane/src/*.c",
	"image/src/*.c", "tools/*.c", "cli/src/*.c", "client/src/*.c",
};
#define NGLOBS ((int)(sizeof(g_globs) / sizeof(g_globs[0])))

int main(void)
{
	char cmd[2048];
	FILE *p;
	char line[4096];
	int warnings = 0;
	int i;

	if (access(GCC_BIN, X_OK) != 0) {
		/*
		 * Not a skip. gcc is a declared build tool of the cix recipe
		 * (pkg_build_depends), so its absence means the build
		 * environment is not what the recipe asked for, and a gate
		 * that quietly passes in that case is not a gate.
		 */
		printf("LINT RESULT: FAIL (%s is not executable -- gcc is a declared "
		       "build tool of this recipe)\n", GCC_BIN);
		return 1;
	}

	for (i = 0; i < NGLOBS; i++) {
		snprintf(cmd, sizeof(cmd),
		         "for f in %s; do [ -e \"$f\" ] || continue; "
		         "%s %s %s \"$f\" 2>&1; done",
		         g_globs[i], GCC_BIN, FLAGS, INCLUDES);
		p = popen(cmd, "r");
		if (p == NULL) {
			printf("LINT RESULT: FAIL (cannot run %s)\n", GCC_BIN);
			return 1;
		}
		while (fgets(line, sizeof(line), p) != NULL) {
			if (strstr(line, ": warning:") != NULL ||
			    strstr(line, ": error:") != NULL) {
				fputs(line, stdout);
				warnings++;
			}
		}
		pclose(p);
	}

	if (warnings != 0) {
		printf("LINT RESULT: FAIL (%d warning(s) gcc reports and TCC does not)\n",
		       warnings);
		return 1;
	}
	printf("LINT RESULT: PASS\n");
	return 0;
}
