/*
 * ADR-0375 phase 1: cixd_main() runs a step that prepares a kernel only
 * on the first start in that kernel, never on a handover.
 *
 * include/startup_steps.h names those steps. This holds the code to the
 * table, by counting rather than parsing, as test_blocking_waits does:
 *
 *   - every table step is called in cixd_main(), so the table names no
 *     step that is gone;
 *   - every call of one sits within a few lines after a first_in_kernel()
 *     test, so none runs again on a handover;
 *   - the bare init_mode tests left in cixd_main() are exactly the four
 *     classified ones (the handover's own check, the uevent socket, the
 *     shutdown's sync and the shutdown itself), so a new init-mode step
 *     cannot slip in unclassified.
 *
 * boot_subsystem_init(init_mode, ...) passes init_mode only to decide
 * whether a failure is fatal, and is not a gate.
 */
#include "startup_steps.h"

#include <stdio.h>
#include <string.h>

#define MAIN_C "daemon/src/main.c"
#define MAX_LINES 4000
#define WINDOW 25
#define BARE_INIT_MODE_ALLOWED 4

static char g_lines[MAX_LINES][512];
static int g_n;

static int is_comment(const char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	return line[0] == '*' || (line[0] == '/' && (line[1] == '*' || line[1] == '/'));
}

/* cixd_main()'s body, comment lines blanked. 0, or -1. */
static int load_cixd_main(void)
{
	char line[512];
	FILE *f = fopen(MAIN_C, "r");
	int in = 0;

	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot open %s\n", MAIN_C);
		return -1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		if (!in) {
			in = strncmp(line, "static int cixd_main(int argc", 29) == 0;
			continue;
		}
		if (line[0] == '}')
			break;
		if (g_n >= MAX_LINES) {
			fprintf(stderr, "FAIL: cixd_main() is longer than %d lines\n", MAX_LINES);
			fclose(f);
			return -1;
		}
		snprintf(g_lines[g_n++], sizeof(g_lines[0]), "%s", is_comment(line) ? "" : line);
	}
	fclose(f);
	if (g_n == 0) {
		fprintf(stderr, "FAIL: cixd_main() not found in %s\n", MAIN_C);
		return -1;
	}
	return 0;
}

static int check_step(const char *name)
{
	char call[80];
	int i, calls = 0, ok = 1;

	snprintf(call, sizeof(call), "%s(", name);
	for (i = 0; i < g_n; i++) {
		int j, gated = 0;

		if (strstr(g_lines[i], call) == NULL)
			continue;
		calls++;
		for (j = i; j >= 0 && j >= i - WINDOW; j--)
			if (strstr(g_lines[j], "first_in_kernel()") != NULL) {
				gated = 1;
				break;
			}
		if (!gated) {
			fprintf(stderr,
			        "FAIL: %s() is called in cixd_main() with no first_in_kernel() test within "
			        "%d lines before it; a handover would run it again (ADR-0375)\n",
			        name, WINDOW);
			ok = 0;
		}
	}
	if (calls == 0) {
		fprintf(stderr,
		        "FAIL: include/startup_steps.h names %s, which cixd_main() no longer calls\n",
		        name);
		ok = 0;
	}
	return ok;
}

int main(void)
{
	int ok = 1, i, bare = 0;

	if (load_cixd_main() != 0)
		return 1;
#define X(step) ok = check_step(#step) && ok;
	STARTUP_ONCE_PER_KERNEL(X)
#undef X
	for (i = 0; i < g_n; i++)
		if (strstr(g_lines[i], "if (init_mode") != NULL)
			bare++;
	if (bare != BARE_INIT_MODE_ALLOWED) {
		fprintf(stderr,
		        "FAIL: cixd_main() tests init_mode bare %d times, expected %d. A step that "
		        "runs only on a real boot is either once per kernel (first_in_kernel(), and "
		        "named in include/startup_steps.h) or run again on a handover; say which "
		        "(ADR-0375)\n",
		        bare, BARE_INIT_MODE_ALLOWED);
		ok = 0;
	}
	printf("STARTUP STEPS: %s (%d lines of cixd_main, %d bare init_mode tests)\n",
	       ok ? "PASS" : "FAIL", g_n, bare);
	return ok ? 0 : 1;
}
