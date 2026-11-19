/*
 * ADR-0381 release 3: fwacquire_verdict(), the one decision on what cixd
 * does about a firmware file a driver declares and the firmware image
 * lacks. No selftest daemon runs in init mode outside the QEMU guests,
 * so this is the gate on the decision itself.
 */
#include "fwacquire_verdict.h"

#include <stdio.h>

static int g_failures;
static int g_cases;

static void check(int provided, int ambiguous, int policy, int installed, int declared,
                  int trusted, enum fwacquire_action want, const char *what)
{
	struct fwacquire_facts f = { provided, ambiguous, policy, installed, declared, trusted };
	enum fwacquire_action got = fwacquire_verdict(&f);

	g_cases++;
	if (got != want) {
		fprintf(stderr, "FAIL: %s: gave %d, want %d\n", what, (int)got, (int)want);
		g_failures++;
	}
}

int main(void)
{
	check(1, 0, 1, 0, 0, 1, FWACQUIRE_DECLARE,
	      "a trusted output provides it, the policy allows the driver: declare it");
	check(0, 0, 1, 0, 0, 1, FWACQUIRE_NOTHING, "nothing provides it");
	check(1, 1, 1, 0, 0, 1, FWACQUIRE_AMBIGUOUS, "two outputs claim it: never picked");
	check(1, 0, 0, 0, 0, 1, FWACQUIRE_REFUSED,
	      "a blacklisted driver, or autoload off, fetches nothing");
	check(1, 1, 0, 0, 0, 1, FWACQUIRE_AMBIGUOUS, "ambiguity is said even for a refused driver");
	check(1, 0, 1, 1, 1, 1, FWACQUIRE_NOT_CARRIED,
	      "installed and still absent: the selector claims a file the output does not ship");
	check(1, 0, 1, 1, 0, 0, FWACQUIRE_NOT_CARRIED,
	      "an operator's install of an untrusted output counts as installed");
	check(1, 0, 1, 0, 1, 1, FWACQUIRE_PENDING, "declared: the drain installs it, never redeclared");
	check(1, 0, 1, 0, 1, 0, FWACQUIRE_PENDING,
	      "an operator declared an untrusted output: it is theirs, pending");
	check(1, 0, 1, 0, 0, 0, FWACQUIRE_UNTRUSTED,
	      "a source without trust_keys: reported, installed only by an operator");
	if (g_failures != 0)
		return 1;
	printf("test_fwacquire_verdict: %d cases pass\n", g_cases);
	return 0;
}
