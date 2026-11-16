/*
 * ADR-0379: rootslot_verdict(), the one decision on whether the running
 * control-plane root is behind cix-root. Unknown counts as behind, a
 * converging image is never behind, and a version that failed to deploy
 * is not tried again until cix-root moves past it.
 */
#include "rootslot_verdict.h"

#include <stdio.h>

static int g_failures;
static int g_cases;

static void check(const char *running, const char *current, int settled, const char *bad,
                  enum rootslot_verdict want, const char *what)
{
	enum rootslot_verdict got = rootslot_verdict(running, current, settled, bad);

	g_cases++;
	if (got != want) {
		fprintf(stderr, "FAIL: %s: gave %d, want %d\n", what, (int)got, (int)want);
		g_failures++;
	}
}

int main(void)
{
	check("aaa", "aaa", 1, "", ROOTSLOT_CURRENT, "the running root is the current version");
	check("aaa", "aaa", 0, "", ROOTSLOT_CURRENT,
	      "current while converging: nothing to deploy yet either way");
	check("aaa", "bbb", 1, "", ROOTSLOT_BEHIND, "cix-root moved and settled");
	check("", "bbb", 1, "", ROOTSLOT_BEHIND, "unknown counts as behind");
	check(NULL, "bbb", 1, NULL, ROOTSLOT_BEHIND, "a NULL record is unknown");
	check("aaa", "bbb", 0, "", ROOTSLOT_UNSETTLED, "a converging image is not behind yet");
	check("", "bbb", 0, "", ROOTSLOT_UNSETTLED, "unknown and converging waits");
	check("aaa", "bbb", 1, "bbb", ROOTSLOT_FAILED, "the current version failed to deploy");
	check("aaa", "ccc", 1, "bbb", ROOTSLOT_BEHIND, "cix-root moved past the bad version");
	check("aaa", "", 1, "", ROOTSLOT_NO_IMAGE, "no current version");
	check("aaa", NULL, 1, "", ROOTSLOT_NO_IMAGE, "a NULL current version");
	if (g_failures != 0)
		return 1;
	printf("test_rootslot_verdict: %d cases pass\n", g_cases);
	return 0;
}
