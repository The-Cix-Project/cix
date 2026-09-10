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

/*
 * The list ADR-0256 fixes, in its order, plus the four ADR-0269 adds so
 * the same vocabulary covers images, deployments and the host (#371).
 *
 * This assertion is the whole point of the file: it fired when #371
 * added four stages, which is exactly ADR-0256's "a deliberate act with
 * an ADR, not a quiet enum append" working as designed. Updating it is
 * the deliberate act; the ADR is the record of why.
 */
static const char *const EXPECTED[] = {
	"discover", "resolve", "authenticate", "author", "fetch",
	"unpack", "build", "install", "publish", "roll", "deploy",
	"acquire", "assemble", "stage", "verify"
};

/*
 * Each kind's path, asserted whole. A kind's stage list is a contract
 * the API publishes and a renderer draws lanes from, so a stage
 * quietly appearing in or leaving one is a contract change.
 */
struct kind_row {
	enum pipeline_kind kind;
	const char *name;
	const char *stages[16];
};

static const struct kind_row KINDS[] = {
	{ PIPELINE_KIND_PACKAGE, "package",
	  { "discover", "resolve", "authenticate", "author", "fetch", "unpack", "build", "install",
	    "publish", "roll", NULL } },
	{ PIPELINE_KIND_IMAGE, "image",
	  { "author", "resolve", "acquire", "install", "publish", "roll", NULL } },
	{ PIPELINE_KIND_DEPLOYMENT, "deployment",
	  { "author", "resolve", "acquire", "deploy", "verify", NULL } },
	{ PIPELINE_KIND_HOST, "host", { "build", "assemble", "stage", "deploy", NULL } },
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

	/*
	 * Every kind's stages, in order, and each one a real member of the
	 * single stage enum -- the failure to catch is a kind growing a
	 * private vocabulary, which is the thing ADR-0256 exists to prevent
	 * and the thing generalising it could most easily reintroduce.
	 */
	if ((int)(sizeof(KINDS) / sizeof(KINDS[0])) != PIPELINE_KIND_COUNT) {
		fprintf(stderr, "  FAIL: %d kinds declared, %d expected\n", PIPELINE_KIND_COUNT,
		        (int)(sizeof(KINDS) / sizeof(KINDS[0])));
		return 1;
	}
	for (i = 0; i < PIPELINE_KIND_COUNT; i++) {
		enum pipeline_stage got[PIPELINE_STAGE_COUNT];
		int n = pipeline_kind_stages(KINDS[i].kind, got);
		int want;

		if (strcmp(pipeline_kind_name(KINDS[i].kind), KINDS[i].name) != 0)
			bad("kind name is not %s", KINDS[i].name);
		for (want = 0; KINDS[i].stages[want] != NULL; want++)
			;
		if (n != want) {
			bad("kind %s does not have the stage count its contract declares", KINDS[i].name);
			continue;
		}
		for (j = 0; j < n; j++) {
			if (strcmp(pipeline_stage_name(got[j]), KINDS[i].stages[j]) != 0) {
				bad("kind %s has a stage where its contract says otherwise", KINDS[i].name);
				break;
			}
		}
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

	/*
	 * ADR-0272: a status survives a round trip through its own name.
	 *
	 * The run store writes a status as text and reads it back on the
	 * next start, so a name-to-value gap does not fail loudly -- it
	 * silently re-reads every historical run as whatever the fallback
	 * is, which for a status means a page full of failures reported as
	 * successes. Asserted in both directions rather than by reading the
	 * table, which is the thing being checked.
	 */
	for (i = 0; i < (int)(sizeof(statuses) / sizeof(statuses[0])); i++) {
		enum pipeline_status back;

		if (pipeline_status_from_name(statuses[i], &back) != 0) {
			bad("status name %s is not recognised by its own parser", statuses[i]);
			continue;
		}
		if ((int)back != i)
			bad("status %s round-trips to a different status", statuses[i]);
	}
	if (pipeline_status_from_name("succeeded", NULL) == 0 ||
	    pipeline_status_from_name(NULL, NULL) == 0)
		bad("a name that is not a status was accepted%s", "");

	if (g_failures > 0) {
		printf("test_pipeline: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("  %d stages, %d statuses, all names fixed by ADR-0256\n", PIPELINE_STAGE_COUNT,
	        (int)(sizeof(statuses) / sizeof(statuses[0])));
	printf("test_pipeline: all checks passed\n");
	return 0;
}
