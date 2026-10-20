/*
 * Folding CBS's JSONL build event stream into a build's phases and its
 * failing command (#388, #489, ADR-0275).
 *
 * The fixture lines are the REAL record shape, transcribed from cbs
 * v0.1.107's own emitter (src/report.c cbs_build_event_jsonl()): every
 * key, in its order, with strings always present (empty, never null) and
 * numbers always present. The event types and their phase/command fields
 * are the ones src/plan.c (phase-begin, phase-end) and src/exec.c
 * (command-begin, command-end) emit; command-end's status is 0 or 1 --
 * whether the exit matched what the recipe expected -- never the exit
 * code (exec.c, command_event_status).
 *
 * Pure: literal lines in, answers out. It is in SELFTESTS so a parsing
 * regression costs seconds rather than a floor run.
 */
#include "cbsevents.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void fail(const char *what)
{
	fprintf(stderr, "  FAIL: %s\n", what);
	g_failures++;
}

static void expect_state(const char *what, const struct cbs_build_progress *p, int phase,
                         enum cbs_phase_state want)
{
	if (p->state[phase] != want) {
		fprintf(stderr, "  FAIL: %s: %s is state %d, expected %d\n", what, cbs_phase_name(phase),
		        (int)p->state[phase], (int)want);
		g_failures++;
	}
}

/* One event line exactly as cbs writes it. */
static size_t ev(char *out, size_t out_size, const char *type, unsigned seq, const char *phase,
                 const char *command, const char *args, const char *log, const char *message,
                 int status, long duration_ms)
{
	int n = snprintf(out, out_size,
	                 "{\"version\":1,\"type\":\"%s\",\"sequence\":%u,\"timestamp_ms\":1792490000000,"
	                 "\"build_id\":\"b-1\",\"package\":\"pkgx\",\"package_version\":\"1.0\","
	                 "\"release\":1,\"arch\":\"x86_64\",\"phase\":\"%s\",\"command\":\"%s\","
	                 "\"arguments\":\"%s\",\"environment_names\":\"\",\"working_directory\":\"\","
	                 "\"log_path\":\"%s\",\"message\":\"%s\",\"path\":\"\",\"rule\":\"\","
	                 "\"status\":%d,\"duration_ms\":%ld,\"stdout_bytes\":0,\"stderr_bytes\":0,"
	                 "\"cpu_ms\":0,\"max_memory_bytes\":0,\"source_bytes\":0,"
	                 "\"fetch_duration_ms\":0,\"tree_bytes\":0,\"tree_files\":0,"
	                 "\"artifact_bytes\":0,\"prune_bytes\":0}\n",
	                 type, seq, phase, command, args, log, message, status, duration_ms);

	return n > 0 ? (size_t)n : 0;
}

static void feed(struct cbs_build_progress *p, const char *type, unsigned seq, const char *phase,
                 const char *command, const char *args, const char *log, const char *message,
                 int status, long duration_ms)
{
	char line[2048];
	size_t n = ev(line, sizeof(line), type, seq, phase, command, args, log, message, status,
	              duration_ms);

	if (cbs_progress_feed(p, line, n) != 0)
		fail("a well-formed event line was refused");
}

#define BIT(phase) (1u << (phase))

static void test_names(void)
{
	int i;

	for (i = 0; i < CBS_PHASE_COUNT; i++)
		if (cbs_phase_from_name(cbs_phase_name(i)) != i)
			fail("a phase name does not round-trip");
	if (cbs_phase_from_name("verify") != -1 || cbs_phase_from_name(NULL) != -1)
		fail("a word outside the vocabulary was accepted as a phase");
	if (strcmp(cbs_phase_state_status(CBS_PHASE_NOT_PLANNED), "not-implemented") != 0 ||
	    cbs_phase_state_status(CBS_PHASE_PENDING) != NULL ||
	    cbs_phase_state_status(CBS_PHASE_RUNNING) != NULL)
		fail("phase states map onto the wrong status words");
	for (i = CBS_PHASE_NOT_PLANNED; i <= CBS_PHASE_CANCELLED; i++)
		if (cbs_phase_state_from_word(cbs_phase_state_word((enum cbs_phase_state)i)) != i)
			fail("a phase state's stored word does not round-trip");
	if (cbs_phase_state_from_word("done") != -1)
		fail("an unknown stored state word was accepted");
}

static void test_success(void)
{
	struct cbs_build_progress p;

	cbs_progress_init(&p, BIT(CBS_PHASE_PREPARE) | BIT(CBS_PHASE_BUILD) | BIT(CBS_PHASE_INSTALL));
	expect_state("init", &p, CBS_PHASE_CONFIGURE, CBS_PHASE_NOT_PLANNED);
	expect_state("init", &p, CBS_PHASE_FINALIZE, CBS_PHASE_PENDING);
	feed(&p, "build-begin", 1, "", "", "", "", "", 0, 0);
	feed(&p, "phase-begin", 2, "prepare", "", "", "", "", 0, 0);
	if (p.current != CBS_PHASE_PREPARE)
		fail("phase-begin did not make that phase the current one");
	feed(&p, "phase-end", 3, "prepare", "", "", "", "", 0, 12);
	feed(&p, "phase-begin", 4, "build", "", "", "", "", 0, 0);
	feed(&p, "command-begin", 5, "build", "make", "-j1", "/build/cbsws/tmp/log.1", "", 0, 0);
	feed(&p, "command-end", 6, "build", "make", "", "", "", 0, 900);
	feed(&p, "phase-end", 7, "build", "", "", "", "", 0, 950);
	feed(&p, "phase-begin", 8, "install", "", "", "", "", 0, 0);
	feed(&p, "phase-end", 9, "install", "", "", "", "", 0, 40);
	feed(&p, "build-end", 10, "", "", "", "", "", 0, 0);
	cbs_progress_finish(&p, 1, 0);
	expect_state("success", &p, CBS_PHASE_PREPARE, CBS_PHASE_OK);
	expect_state("success", &p, CBS_PHASE_BUILD, CBS_PHASE_OK);
	expect_state("success", &p, CBS_PHASE_CHECK, CBS_PHASE_NOT_PLANNED);
	expect_state("success", &p, CBS_PHASE_INSTALL, CBS_PHASE_OK);
	expect_state("success", &p, CBS_PHASE_FINALIZE, CBS_PHASE_OK);
	if (p.failed != -1 || p.current != -1 || p.failed_command[0] != '\0')
		fail("a successful build reported a failure or a running phase");
	if (p.duration_ms[CBS_PHASE_BUILD] != 950)
		fail("a phase's duration was not taken from its phase-end");
}

static void test_check_fails(void)
{
	struct cbs_build_progress p;

	cbs_progress_init(&p, BIT(CBS_PHASE_BUILD) | BIT(CBS_PHASE_CHECK) | BIT(CBS_PHASE_INSTALL));
	feed(&p, "phase-begin", 1, "build", "", "", "", "", 0, 0);
	feed(&p, "phase-end", 2, "build", "", "", "", "", 0, 10);
	feed(&p, "phase-begin", 3, "install", "", "", "", "", 0, 0);
	feed(&p, "phase-end", 4, "install", "", "", "", "", 0, 10);
	p.changed = 0;
	feed(&p, "phase-begin", 5, "check", "", "", "", "", 0, 0);
	if (!p.changed)
		fail("a phase starting did not mark the progress changed");
	feed(&p, "command-begin", 6, "check", "make", "check", "/build/cbsws/tmp/cmd-7.log", "", 0,
	     0);
	feed(&p, "command-end", 7, "check", "make", "", "", "command failed", 1, 30);
	/* A recipe's on_fail diagnostics run after the failure, and must not
	 * replace the command that actually failed. */
	feed(&p, "command-begin", 8, "check", "ls", "-1", "/build/cbsws/tmp/cmd-8.log", "", 0, 0);
	feed(&p, "command-end", 9, "check", "ls", "", "", "command failed", 1, 1);
	feed(&p, "phase-end", 10, "check", "", "", "", "phase failed", 1, 40);
	feed(&p, "build-end", 11, "", "", "", "", "build failed", 1, 0);
	cbs_progress_finish(&p, 0, 0);
	if (p.failed != CBS_PHASE_CHECK)
		fail("a failing check phase was not named as the failed phase");
	if (strcmp(p.failed_command, "make check") != 0) {
		fprintf(stderr, "  FAIL: failed command is \"%s\", expected \"make check\"\n",
		        p.failed_command);
		g_failures++;
	}
	if (strcmp(p.failed_log, "/build/cbsws/tmp/cmd-7.log") != 0)
		fail("the failed command's log is not the one its command-begin named");
	if (strcmp(p.failed_message, "command failed") != 0)
		fail("the failed command's message was not kept");
	expect_state("check fails", &p, CBS_PHASE_CHECK, CBS_PHASE_FAILED);
	expect_state("check fails", &p, CBS_PHASE_FINALIZE, CBS_PHASE_BLOCKED);
}

static void test_finalize_fails(void)
{
	struct cbs_build_progress p;

	cbs_progress_init(&p, BIT(CBS_PHASE_BUILD));
	feed(&p, "phase-begin", 1, "build", "", "", "", "", 0, 0);
	feed(&p, "phase-end", 2, "build", "", "", "", "", 0, 10);
	feed(&p, "build-end", 3, "", "", "", "", "build failed", 1, 0);
	cbs_progress_finish(&p, 0, 0);
	if (p.failed != CBS_PHASE_FINALIZE)
		fail("a failure after every phase ended well was not named finalize");
	expect_state("finalize fails", &p, CBS_PHASE_BUILD, CBS_PHASE_OK);
}

static void test_killed_and_cancelled(void)
{
	struct cbs_build_progress p;

	cbs_progress_init(&p, BIT(CBS_PHASE_BUILD) | BIT(CBS_PHASE_INSTALL));
	feed(&p, "phase-begin", 1, "build", "", "", "", "", 0, 0);
	cbs_progress_finish(&p, 0, 0);
	if (p.failed != CBS_PHASE_BUILD)
		fail("a phase begun and never ended was not named as the failed one");
	expect_state("killed", &p, CBS_PHASE_INSTALL, CBS_PHASE_BLOCKED);

	cbs_progress_init(&p, BIT(CBS_PHASE_BUILD) | BIT(CBS_PHASE_INSTALL));
	feed(&p, "phase-begin", 1, "build", "", "", "", "", 0, 0);
	cbs_progress_finish(&p, 0, 1);
	expect_state("cancelled", &p, CBS_PHASE_BUILD, CBS_PHASE_CANCELLED);
	expect_state("cancelled", &p, CBS_PHASE_INSTALL, CBS_PHASE_CANCELLED);
	if (p.failed != -1)
		fail("a cancelled build named a failed phase");
}

static void test_replay_and_garbage(void)
{
	struct cbs_build_progress p;
	static const char garbage[] = "not json at all\n";

	cbs_progress_init(&p, BIT(CBS_PHASE_BUILD));
	feed(&p, "phase-begin", 5, "build", "", "", "", "", 0, 0);
	feed(&p, "phase-end", 6, "build", "", "", "", "", 0, 7);
	/* An older sequence is a re-read, and must not reopen the phase. */
	feed(&p, "phase-begin", 5, "build", "", "", "", "", 0, 0);
	expect_state("replay", &p, CBS_PHASE_BUILD, CBS_PHASE_OK);
	if (cbs_progress_feed(&p, garbage, sizeof(garbage) - 1) != -1)
		fail("a line that is not JSON was accepted");
	if (cbs_progress_feed(&p, "", 0) != 0)
		fail("an empty line was not ignored");
	/* An undeclared phase cbs nonetheless runs is reported, not hidden. */
	feed(&p, "phase-begin", 7, "configure", "", "", "", "", 0, 0);
	if (p.current != CBS_PHASE_CONFIGURE)
		fail("a phase outside the plan that cbs ran was not reported");
}

int main(void)
{
	printf("=== CBS build events (ADR-0275, #388, #489) ===\n");
	test_names();
	test_success();
	test_check_fails();
	test_finalize_fails();
	test_killed_and_cancelled();
	test_replay_and_garbage();

	if (g_failures != 0) {
		printf("CBS EVENTS TEST: FAIL (%d failure(s))\n", g_failures);
		return 1;
	}
	printf("CBS EVENTS TEST: PASS\n");
	return 0;
}
