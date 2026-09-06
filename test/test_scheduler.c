/*
 * ADR-0257's scheduler.
 *
 * The properties worth guarding are the ones a wrong answer would hide:
 *
 *  - EXACTLY ONE schedule form. Two present is refused, not resolved by
 *    precedence -- a caller who sent both believes something untrue
 *    about what they configured, and silently picking one is the whole
 *    failure mode a structured format exists to avoid.
 *  - An unknown action names the ones that exist. The registry is the
 *    security boundary (no free-text commands on a shell-less host),
 *    and a refusal that does not say what IS allowed makes the boundary
 *    feel arbitrary.
 *  - `every` is anchored on the LAST RUN, not on an epoch. An
 *    epoch-anchored period fires the moment the daemon starts whenever
 *    the box was down longer than one period, turning a reboot into a
 *    burst of work.
 *  - `describes` renders and is never parsed back.
 */
#include "scheduler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures;
static int g_action_runs;
static char g_state[256];

static void bad(const char *fmt, const char *a)
{
	fprintf(stderr, "  FAIL: ");
	fprintf(stderr, fmt, a);
	fprintf(stderr, "\n");
	g_failures++;
}

static int action_ok(const char *params, char *reason, size_t reason_size)
{
	(void)params;
	g_action_runs++;
	snprintf(reason, reason_size, "ran");
	return 0;
}

static int action_fails(const char *params, char *reason, size_t reason_size)
{
	(void)params;
	snprintf(reason, reason_size, "the disk is not mounted");
	return -1;
}

static void expect_set(const char *label, const char *name, const char *body, int want_ok,
                        const char *must_mention)
{
	char err[320];
	enum schedule_error e = scheduler_set_from_json(name, body, strlen(body), err, sizeof(err));

	if (want_ok && e != SCHEDULE_OK) {
		bad("%s should have been accepted", label);
		fprintf(stderr, "         %s\n", err);
		return;
	}
	if (!want_ok) {
		if (e == SCHEDULE_OK) {
			bad("%s was accepted and should not have been", label);
			return;
		}
		if (must_mention != NULL && strstr(err, must_mention) == NULL) {
			bad("%s refusal did not mention what it should", label);
			fprintf(stderr, "         %s\n", err);
			return;
		}
		printf("  %-34s refused: %.90s\n", label, err);
		return;
	}
	printf("  %-34s accepted\n", label);
}

int main(void)
{
	const struct schedule *s;
	char described[SCHEDULE_DESCRIBE_MAX];
	char err[320];
	long now;

	snprintf(g_state, sizeof(g_state), "/tmp/cix_sched_test_%d.json", (int)getpid());
	unlink(g_state);
	printf("test_scheduler\n");

	if (scheduler_init(g_state) != 0) {
		fprintf(stderr, "  FAIL: init\n");
		return 1;
	}
	scheduler_register_action("test.ok", "always succeeds", action_ok);
	scheduler_register_action("test.fails", "always fails", action_fails);

	/* The registry is closed, and a refusal names what IS available. */
	expect_set("unknown action", "j1",
	            "{\"action\":\"rm -rf /\",\"schedule\":{\"every\":{\"hours\":1}}}", 0, "test.ok");

	/* Exactly one form. */
	expect_set("two forms at once", "j1",
	            "{\"action\":\"test.ok\",\"schedule\":{\"every\":{\"hours\":1},"
	            "\"daily\":{\"at\":\"02:00\"}}}",
	            0, "exactly one");
	expect_set("no form at all", "j1", "{\"action\":\"test.ok\",\"schedule\":{}}", 0,
	            "exactly one");
	expect_set("no schedule key", "j1", "{\"action\":\"test.ok\"}", 0, "required");

	/* Ranges are checked, and the message says the range. */
	expect_set("hour 25", "j1",
	            "{\"action\":\"test.ok\",\"schedule\":{\"daily\":{\"at\":\"25:00\"}}}", 0,
	            "23:59");
	expect_set("weekday \"funday\"", "j1",
	            "{\"action\":\"test.ok\",\"schedule\":{\"weekly\":{\"on\":\"funday\","
	            "\"at\":\"03:00\"}}}",
	            0, "sun, mon");
	/* A one-second period is a units mistake far more often than an
	 * intention, so the floor is 10. */
	expect_set("every 1 second", "j1",
	            "{\"action\":\"test.ok\",\"schedule\":{\"every\":{\"seconds\":1}}}", 0,
	            "10 seconds");

	expect_set("every 6h", "hourly-ish",
	            "{\"action\":\"test.ok\",\"schedule\":{\"every\":{\"hours\":6}}}", 1, NULL);
	expect_set("daily at 02:00 with a window", "nightly",
	            "{\"action\":\"test.ok\",\"schedule\":{\"daily\":{\"at\":\"02:00\"}},"
	            "\"window_minutes\":180}",
	            1, NULL);
	expect_set("weekly on sun at 03:00", "weekly-job",
	            "{\"action\":\"test.fails\",\"schedule\":{\"weekly\":{\"on\":\"sun\","
	            "\"at\":\"03:00\"}}}",
	            1, NULL);

	/* The rendered form -- display only, never read back. */
	s = scheduler_find("nightly");
	schedule_describe(s, described, sizeof(described));
	if (strcmp(described, "daily at 02:00 for 3h") != 0)
		bad("describes rendered \"%s\"", described);
	else
		printf("  describes                          %s\n", described);
	s = scheduler_find("hourly-ish");
	schedule_describe(s, described, sizeof(described));
	if (strcmp(described, "every 6h") != 0)
		bad("describes rendered \"%s\"", described);

	/*
	 * ANCHORED ON THE LAST RUN. A job that has never run is due now; one
	 * that ran a minute ago is due one period after THAT, not at the
	 * next multiple of some epoch.
	 */
	now = (long)time(NULL);
	s = scheduler_find("hourly-ish");
	if (schedule_next_run(s, now) != now)
		bad("a never-run periodic job is not due now%s", "");
	g_action_runs = 0;
	scheduler_run_due(now, 0);
	if (g_action_runs != 1)
		bad("expected exactly one action run, got a different count%s", "");
	s = scheduler_find("hourly-ish");
	if (schedule_next_run(s, now) != s->last_run_at + 6 * 3600)
		bad("next run is not one period after the last run%s", "");
	if (!s->last_ok || strcmp(s->last_reason, "ran") != 0)
		bad("a successful run was not recorded as one%s", "");

	/* A failing action records why, rather than looking like it worked. */
	if (scheduler_run_now("weekly-job", err, sizeof(err)) == SCHEDULE_OK)
		bad("a failing action reported success%s", "");
	s = scheduler_find("weekly-job");
	if (s->last_ok || strstr(s->last_reason, "not mounted") == NULL)
		bad("the failure reason was lost: \"%s\"", s->last_reason);
	else
		printf("  failed run recorded                %s\n", s->last_reason);

	/* A window bounds how long an action may keep starting work. */
	s = scheduler_find("nightly");
	if (scheduler_in_window(s, now) != 0)
		bad("a job that has never run was reported as inside its window%s", "");
	s = scheduler_find("hourly-ish");
	if (!scheduler_in_window(s, now))
		bad("a job with no window was reported as outside one%s", "");

	/* Survives a reload: settings and run history both. */
	if (scheduler_init(g_state) != 0)
		bad("reload failed%s", "");
	s = scheduler_find("nightly");
	if (s == NULL || s->window_minutes != 180 || s->hour != 2) {
		bad("a schedule did not survive a reload%s", "");
	} else {
		s = scheduler_find("weekly-job");
		if (s == NULL || s->last_ok || s->last_reason[0] == '\0')
			bad("the run history did not survive a reload%s", "");
		else
			printf("  reload kept settings and history\n");
	}

	if (scheduler_delete("nightly") != SCHEDULE_OK || scheduler_find("nightly") != NULL)
		bad("delete did not remove the schedule%s", "");
	if (scheduler_delete("nightly") != SCHEDULE_ERR_NOT_FOUND)
		bad("deleting a missing schedule was not reported as missing%s", "");

	/*
	 * The wall-clock timing tests run against an EMPTY scheduler. Every
	 * job above shares one action, so leaving them in place would make
	 * the run counter measure the whole table rather than the job under
	 * test -- which is how the first version of this test passed while
	 * daily jobs did not fire at all.
	 */
	scheduler_delete("hourly-ish");
	scheduler_delete("weekly-job");

	/*
	 * A WALL-CLOCK JOB MUST ACTUALLY FIRE. This is the case that was
	 * broken and briefly shipped: schedule_next_run() returns the NEXT
	 * occurrence, which is strictly in the future by construction, so a
	 * due test written as "next_run <= now" is never true and no daily
	 * job ever runs. The due test compares the most recent occurrence
	 * against what the job has already accounted for instead.
	 */
	expect_set("daily at 02:00", "daily-job",
	            "{\"action\":\"test.ok\",\"schedule\":{\"daily\":{\"at\":\"02:00\"}}}", 1, NULL);
	{
		struct tm tm;
		time_t t = (time_t)(now + 24 * 3600);
		long fire;

		/* Tomorrow's 02:00 plus half a minute -- the real timer fires
		 * within a minute of the instant, never exactly on it. */
		localtime_r(&t, &tm);
		tm.tm_hour = 2;
		tm.tm_min = 0;
		tm.tm_sec = 0;
		tm.tm_isdst = -1;
		fire = (long)mktime(&tm) + 30;

		g_action_runs = 0;
		scheduler_run_due(fire, 0);
		if (g_action_runs != 1)
			bad("a daily job did not fire at its own time%s", "");
		else
			printf("  daily job fired at its time\n");
		/* Exactly once: a later tick must not re-run one occurrence. */
		scheduler_run_due(fire + 60, 0);
		if (g_action_runs != 1)
			bad("a daily job fired twice for one occurrence%s", "");
		else
			printf("  and did not fire twice\n");
	}
	scheduler_delete("daily-job");

	/*
	 * A missed wall-clock run does NOT fire at startup unless the job
	 * asked to -- and the skip must STICK. Skipping by simply not
	 * running leaves the occurrence outstanding, so the very next tick
	 * (no longer "startup") runs it after all: a one-tick deferral
	 * wearing the word skip.
	 */
	expect_set("catch_up off (default)", "missed",
	            "{\"action\":\"test.ok\",\"schedule\":{\"daily\":{\"at\":\"02:00\"}}}", 1, NULL);
	{
		struct tm tm;
		time_t t = (time_t)(now + 24 * 3600);
		long fire;

		localtime_r(&t, &tm);
		tm.tm_hour = 2;
		tm.tm_min = 0;
		tm.tm_sec = 0;
		tm.tm_isdst = -1;
		fire = (long)mktime(&tm) + 30;

		g_action_runs = 0;
		scheduler_run_due(fire, 1);
		if (g_action_runs != 0)
			bad("a wall-clock job ran at startup without catch_up%s", "");
		scheduler_run_due(fire + 60, 0);
		if (g_action_runs != 0)
			bad("a skipped occurrence ran on the next tick anyway%s", "");
		else
			printf("  skipped occurrence stayed skipped\n");
		s = scheduler_find("missed");
		if (s->last_skipped_at == 0)
			bad("the skipped occurrence was not recorded at all%s", "");
	}
	scheduler_delete("missed");

	/* With catch_up on, the same missed occurrence DOES run. */
	expect_set("catch_up on", "caught",
	            "{\"action\":\"test.ok\",\"schedule\":{\"daily\":{\"at\":\"02:00\"}},"
	            "\"catch_up\":true}",
	            1, NULL);
	{
		struct tm tm;
		time_t t = (time_t)(now + 24 * 3600);
		long fire;

		localtime_r(&t, &tm);
		tm.tm_hour = 2;
		tm.tm_min = 0;
		tm.tm_sec = 0;
		tm.tm_isdst = -1;
		fire = (long)mktime(&tm) + 30;

		g_action_runs = 0;
		scheduler_run_due(fire, 1);
		if (g_action_runs != 1)
			bad("catch_up did not run the missed occurrence%s", "");
		else
			printf("  catch_up ran the missed occurrence\n");
	}
	scheduler_delete("caught");

	/*
	 * A job created now must not look like it missed today's earlier
	 * occurrence. Without an anchor, last_run_at of 0 makes every past
	 * occurrence look outstanding, so a job created this afternoon
	 * would fire for this morning.
	 */
	expect_set("created after today's time", "fresh",
	            "{\"action\":\"test.ok\",\"schedule\":{\"daily\":{\"at\":\"00:01\"}},"
	            "\"catch_up\":true}",
	            1, NULL);
	g_action_runs = 0;
	scheduler_run_due(now, 1);
	if (g_action_runs != 0)
		bad("a freshly created job fired for an occurrence before it existed%s", "");
	else
		printf("  a new job did not fire for a pre-creation occurrence\n");

	unlink(g_state);
	if (g_failures > 0) {
		printf("test_scheduler: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_scheduler: all checks passed\n");
	return 0;
}
