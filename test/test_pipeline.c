/*
 * ADR-0256's shared vocabulary.
 *
 * These eleven names are API enum values, CLI column contents and web
 * labels simultaneously, so a rename here is a contract change across
 * three surfaces at once. This test is the thing that makes such a
 * rename visible in a diff rather than shipping quietly -- the same
 * reason test_toolchain_policy asserts a count.
 *
 * It also guards the two properties the model depends on: that the
 * stage list is ORDERED (so "the earliest non-ok stage" is meaningful)
 * and that `unpack` sits between `fetch` and `build`, which is the one
 * behaviour change ADR-0256 makes to existing failure reporting.
 */
#include "pipeline.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void bad(const char *fmt, const char *a)
{
	fprintf(stderr, "  FAIL: ");
	fprintf(stderr, fmt, a);
	fprintf(stderr, "\n");
	g_failures++;
}

/* The list ADR-0256 fixes, in its order. */
static const char *const EXPECTED[] = {
	"discover", "resolve", "authenticate", "author", "fetch",
	"unpack", "build", "install", "publish", "roll", "deploy"
};

int main(void)
{
	int i, j;
	const char *const statuses[] = { "ok", "blocked", "failed", "cancelled", "not-implemented" };

	printf("test_pipeline\n");

	if (PIPELINE_STAGE_COUNT != (int)(sizeof(EXPECTED) / sizeof(EXPECTED[0]))) {
		fprintf(stderr, "  FAIL: %d stages, ADR-0256 fixes %d -- adding one is a "
		                "deliberate act with an ADR, not a quiet enum append\n",
		        PIPELINE_STAGE_COUNT, (int)(sizeof(EXPECTED) / sizeof(EXPECTED[0])));
		return 1;
	}

	for (i = 0; i < PIPELINE_STAGE_COUNT; i++) {
		enum pipeline_stage got;

		if (strcmp(pipeline_stage_name((enum pipeline_stage)i), EXPECTED[i]) != 0) {
			bad("stage %s is not where ADR-0256 puts it",
			     pipeline_stage_name((enum pipeline_stage)i));
			continue;
		}
		/* Round-trips, so a client can send back what it was given. */
		if (pipeline_stage_from_name(EXPECTED[i], &got) != 0 || (int)got != i) {
			bad("stage %s does not round-trip through its own name", EXPECTED[i]);
			continue;
		}
		/* Every stage says what a failure there reads as -- eleven
		 * stages must not grow twelve ways of describing themselves. */
		if (pipeline_stage_verb((enum pipeline_stage)i)[0] == '\0') {
			bad("stage %s has no failure verb", EXPECTED[i]);
			continue;
		}
		for (j = 0; j < i; j++) {
			if (strcmp(EXPECTED[i], EXPECTED[j]) == 0)
				bad("stage %s appears twice", EXPECTED[i]);
		}
	}

	/*
	 * The one behaviour change: unpacking is its own stage, strictly
	 * after downloading and strictly before compiling. If this ordering
	 * ever collapses, a corrupt archive goes back to reporting as a
	 * build failure and sends readers to the wrong log.
	 */
	if (!(PIPELINE_FETCH < PIPELINE_UNPACK && PIPELINE_UNPACK < PIPELINE_BUILD))
		bad("unpack is not between fetch and build%s", "");

	for (i = 0; i < (int)(sizeof(statuses) / sizeof(statuses[0])); i++) {
		if (strcmp(pipeline_status_name((enum pipeline_status)i), statuses[i]) != 0)
			bad("status %s is not where ADR-0256 puts it",
			     pipeline_status_name((enum pipeline_status)i));
	}

	/* Out-of-range never returns a plausible-looking name. */
	if (strcmp(pipeline_stage_name((enum pipeline_stage)PIPELINE_STAGE_COUNT), "unknown") != 0 ||
	    strcmp(pipeline_status_name((enum pipeline_status)99), "unknown") != 0)
		bad("an out-of-range value produced a real-looking name%s", "");
	if (pipeline_stage_from_name("compile", NULL) == 0)
		bad("a name that is not a stage was accepted%s", "");

	if (g_failures > 0) {
		printf("test_pipeline: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("  %d stages, %d statuses, all names fixed by ADR-0256\n", PIPELINE_STAGE_COUNT,
	        (int)(sizeof(statuses) / sizeof(statuses[0])));
	printf("test_pipeline: all checks passed\n");
	return 0;
}
