/*
 * test_stallwatch_steps -- a slow handler names its slowest step (#619).
 *
 * The loop names a slow pass by the handler that took it, and a handler
 * that served no request only by its event kind: "event container (1749
 * ms)" was the whole record of four stalls on 192.168.15.95 on
 * 2026-10-19, with nothing saying which part of a build container's exit
 * held the loop. stallwatch_step() marks the steps inside a handler, and
 * the record then carries the slowest of them.
 *
 * Five things are pinned down here, each one a way it could regress
 * quietly:
 *
 *   1. the slowest step is the one named, not the last or the first;
 *   2. steps belong to the handler they ran in -- the next handler
 *      starts with none, so a handler with no steps is named exactly as
 *      before, and a step called outside any handler does nothing;
 *   3. while a step runs it is the in-flight activity the watchdog
 *      reads, so a step that never returns is what a stall names;
 *   4. a child forked inside a handler, which shares the page, cannot
 *      write that activity;
 *   5. a call made many times is tallied, and the two largest tallies
 *      are named after the slowest step -- the per-file calls of an
 *      install interleave, so no step can time them.
 *
 * Links stallwatch.c directly and drives it as the loop does, with no
 * daemon, so it runs in SELFTESTS on the box (#224). The watchdog it
 * forks exits when this process does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "json.h"
#include "stallwatch.h"

static int g_fail;

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) != 0)
		;
}

/* One loop pass holding one handler: the shape main()'s loop has. */
static void run_pass(const char *kind, const char *const *steps, const long *ms, int n)
{
	long long started;
	int i;

	stallwatch_heartbeat();
	started = stallwatch_handler_start();
	for (i = 0; i < n; i++) {
		if (steps[i] != NULL)
			stallwatch_step(steps[i]);
		sleep_ms(ms[i]);
	}
	stallwatch_handler_done(kind, started);
	stallwatch_activity_clear();
	stallwatch_pass_end();
}

/* The worst pass's name, as GET /v1/system/stalls reports it. */
static void worst_activity(char *out, size_t cap)
{
	struct json_writer w;
	struct json_value *v;
	const char *s;

	out[0] = '\0';
	jw_init(&w);
	stallwatch_write_loop_json(&w);
	v = json_parse(w.buf, w.len);
	s = v != NULL ? json_as_string(json_object_get(v, "worst_pass_activity")) : NULL;
	if (s != NULL)
		snprintf(out, cap, "%s", s);
	json_free(v);
	jw_free(&w);
}

int main(void)
{
	char dir[] = "/tmp/cix_test_stallsteps_XXXXXX";
	char records[64];
	char got[256];
	const char *cur;

	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(records, sizeof(records), "%s/stalls.jsonl", dir);
	if (stallwatch_start(records) != 0) {
		fprintf(stderr, "FAIL: stallwatch_start\n");
		return 1;
	}

	/* 1. The slowest step is named, between a quicker one either side. */
	{
		static const char *const steps[] = { "first, quick", "the slow one", "last, quick" };
		static const long ms[] = { 20, 150, 10 };

		run_pass("container", steps, ms, 3);
	}
	worst_activity(got, sizeof(got));
	if (strncmp(got, "event container (", 17) != 0 ||
	    strstr(got, ", slowest step the slow one (") == NULL) {
		fprintf(stderr, "FAIL: a handler with steps was named \"%s\"; want \"event container "
		                "(N ms), slowest step the slow one (M ms)\"\n", got);
		g_fail = 1;
	}

	/* 2. A step outside any handler does nothing, and the next handler
	 * starts with no steps: slower than the first pass, and named
	 * exactly as before #619. */
	stallwatch_step("stray, outside any handler");
	{
		static const char *const steps[] = { NULL };
		static const long ms[] = { 300 };

		run_pass("scheduler-timer", steps, ms, 1);
	}
	worst_activity(got, sizeof(got));
	if (strncmp(got, "event scheduler-timer (", 23) != 0 || strstr(got, "step") != NULL) {
		fprintf(stderr, "FAIL: a handler with no steps was named \"%s\"; want \"event "
		                "scheduler-timer (N ms)\" and no step\n", got);
		g_fail = 1;
	}

	/* 3. While a step runs, it is what the watchdog would name. */
	{
		long long started;

		stallwatch_heartbeat();
		started = stallwatch_handler_start();
		stallwatch_step("pkg install: produce the image version");
		cur = stallwatch_current_activity();
		if (cur == NULL || strcmp(cur, "step pkg install: produce the image version") != 0) {
			fprintf(stderr, "FAIL: the in-flight activity during a step was \"%s\"\n",
			        cur != NULL ? cur : "(null)");
			g_fail = 1;
		}
		/* And a request's step is named with the request. */
		stallwatch_activity("POST /v1/events/verify");
		stallwatch_step("GET /v1/pkg");
		cur = stallwatch_current_activity();
		if (cur == NULL || strcmp(cur, "POST /v1/events/verify, step GET /v1/pkg") != 0) {
			fprintf(stderr, "FAIL: the in-flight activity during a request's step was \"%s\"\n",
			        cur != NULL ? cur : "(null)");
			g_fail = 1;
		}
		stallwatch_handler_done("client", started);
		stallwatch_activity_clear();
		stallwatch_pass_end();
		cur = stallwatch_current_activity();
		if (cur == NULL || cur[0] != '\0') {
			fprintf(stderr, "FAIL: a step outlived its handler as the activity \"%s\"\n",
			        cur != NULL ? cur : "(null)");
			g_fail = 1;
		}
	}

	/* 4. A child forked inside a handler -- the build environment's
	 * composer runs image_produce_new_version(), which marks steps --
	 * shares the page, and must not write the daemon's activity. */
	{
		long long started;
		pid_t child;
		int status;

		stallwatch_heartbeat();
		started = stallwatch_handler_start();
		stallwatch_step("the daemon's step");
		child = fork();
		if (child == 0) {
			stallwatch_step("image version: snapshot the current rootfs forward");
			_exit(0);
		}
		if (child < 0 || waitpid(child, &status, 0) != child) {
			fprintf(stderr, "FAIL: fork/wait for the child step\n");
			g_fail = 1;
		}
		cur = stallwatch_current_activity();
		if (cur == NULL || strcmp(cur, "step the daemon's step") != 0) {
			fprintf(stderr, "FAIL: a forked child's step overwrote the activity: \"%s\"\n",
			        cur != NULL ? cur : "(null)");
			g_fail = 1;
		}
		stallwatch_handler_done("pkg-buildenv", started);
		stallwatch_activity_clear();
		stallwatch_pass_end();
	}

	/* 5. Tallies: the two kinds of call that took the most are named,
	 * largest first, with the larger one's count -- and a tally outside
	 * any handler counts nothing. Slower than check 2's pass, so it is
	 * the worst pass the loop reports. */
	stallwatch_tally("stray", stallwatch_tally_start());
	{
		static const long unlink_ms[] = { 30, 30, 30 };
		static const long copy_ms[] = { 160, 160 };
		long long started, t;
		int i;

		stallwatch_heartbeat();
		started = stallwatch_handler_start();
		stallwatch_step("image version: the caller's mutate step");
		for (i = 0; i < 3; i++) {
			t = stallwatch_tally_start();
			sleep_ms(unlink_ms[i]);
			stallwatch_tally("unlink", t);
		}
		for (i = 0; i < 2; i++) {
			t = stallwatch_tally_start();
			sleep_ms(copy_ms[i]);
			stallwatch_tally("copy", t);
		}
		t = stallwatch_tally_start();
		sleep_ms(10);
		stallwatch_tally("chmod", t);
		stallwatch_handler_done("container", started);
		stallwatch_activity_clear();
		stallwatch_pass_end();
	}
	worst_activity(got, sizeof(got));
	if (strncmp(got, "event container (", 17) != 0 ||
	    strstr(got, ", slowest step image version: the caller's mutate step (") == NULL ||
	    strstr(got, " ms), most in copy (") == NULL || strstr(got, " ms, 2 calls), then unlink (") == NULL ||
	    strstr(got, "stray") != NULL || strstr(got, "chmod") != NULL) {
		fprintf(stderr, "FAIL: a handler with tallies was named \"%s\"; want \"..., slowest step "
		                "image version: the caller's mutate step (N ms), most in copy (M ms, 2 "
		                "calls), then unlink (K ms)\"\n", got);
		g_fail = 1;
	}

	unlink(records);
	rmdir(dir);
	if (g_fail)
		return 1;
	printf("test_stallwatch_steps: PASS\n");
	return 0;
}
