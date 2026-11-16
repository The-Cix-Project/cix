/*
 * #688: hostroll_start_verdict(), the host roll's one decision about
 * whether its first step may start. A roll never starts a hostbuild
 * beside another package job, and never builds or stages once its wait
 * has ended.
 */
#include "hostroll_verdict.h"

#include <stdio.h>

static int g_failures;

static void check(int jobs, long now, long until, enum hostroll_start want, const char *what)
{
	enum hostroll_start got = hostroll_start_verdict(jobs, now, until);

	if (got != want) {
		fprintf(stderr, "FAIL: %s: jobs=%d now=%ld until=%ld gave %d, want %d\n", what, jobs,
		        now, until, (int)got, (int)want);
		g_failures++;
	}
}

int main(void)
{
	check(0, 100, 200, HOSTROLL_START_NOW, "nothing in flight starts at once");
	check(0, 300, 200, HOSTROLL_START_NOW, "nothing in flight starts even past the wait");
	check(1, 100, 200, HOSTROLL_START_WAIT, "one job in flight waits");
	check(7, 199, 200, HOSTROLL_START_WAIT, "jobs in flight wait until the last second");
	check(1, 200, 200, HOSTROLL_START_GIVE_UP, "the wait ends at its deadline");
	check(3, 500, 200, HOSTROLL_START_GIVE_UP, "past the deadline gives up");
	check(-1, 100, 200, HOSTROLL_START_NOW, "a negative count is nothing in flight");
	if (g_failures != 0)
		return 1;
	printf("test_hostroll_verdict: 7 cases pass\n");
	return 0;
}
