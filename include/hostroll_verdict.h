#ifndef CIX_HOSTROLL_VERDICT_H
#define CIX_HOSTROLL_VERDICT_H

/*
 * #688: may the host roll (ADR-0327) start its first step now?
 *
 * The roll used to refuse when a hostbuild was in flight and to start
 * beside anything else. Refusing loses the window, since a scheduled run
 * is recorded once and not retried inside it. Starting beside an
 * ordinary build puts a hostbuild, which compiles the whole control plane
 * and runs its selftest, beside a build that may be just as heavy. Two
 * concurrent heavy builds wedged cixd on 192.168.15.95 for about 16
 * minutes (CLAUDE.md). So the roll waits until no package job of any
 * kind is in flight, the same question an in-place switch asks
 * (pkg_any_job_in_flight(), #686), and gives up when its wait ends. A
 * roll that gives up builds, stages and reboots nothing.
 *
 * jobs_in_flight: package jobs running now (pkg_any_job_in_flight()).
 * now, wait_until: seconds since the epoch; the wait ends at wait_until.
 */
enum hostroll_start {
	HOSTROLL_START_NOW,     /* nothing in flight: decide what to build */
	HOSTROLL_START_WAIT,    /* jobs in flight, and time left to wait for them */
	HOSTROLL_START_GIVE_UP  /* jobs still in flight when the wait ended */
};

static inline enum hostroll_start hostroll_start_verdict(int jobs_in_flight, long now,
                                                         long wait_until)
{
	if (jobs_in_flight <= 0)
		return HOSTROLL_START_NOW;
	if (now < wait_until)
		return HOSTROLL_START_WAIT;
	return HOSTROLL_START_GIVE_UP;
}

#endif
