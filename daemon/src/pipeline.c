/*
 * See pipeline.h for why stage and status are two axes rather than one
 * longer enum.
 */
#include "pipeline.h"

#include <string.h>

struct stage_row {
	const char *name;
	const char *verb;
};

/* Indexed by enum pipeline_stage -- order is the pipeline's own order,
 * which is what lets a caller say "the earliest non-ok stage". */
static const struct stage_row STAGES[PIPELINE_STAGE_COUNT] = {
	{ "discover",     "could not check for versions" },
	{ "resolve",      "could not resolve a version" },
	{ "authenticate", "could not verify the signature" },
	{ "author",       "no recipe for the resolved release" },
	{ "fetch",        "could not download" },
	{ "unpack",       "could not unpack" },
	{ "build",        "could not compile" },
	{ "install",      "could not install" },
	{ "publish",      "could not publish" },
	{ "roll",         "could not rebuild dependents" },
	{ "deploy",       "could not deploy" },
	{ "acquire",      "could not acquire what it depends on" },
	{ "assemble",     "could not assemble a control-plane root" },
	{ "stage",        "could not stage to a boot slot" },
	{ "verify",       "did not become ready" },
};

static const char *const KINDS[PIPELINE_KIND_COUNT] = {
	"package", "image", "deployment", "host"
};

/*
 * Each kind's ordered path through the one stage enum above (#371).
 *
 * Read these as answers to "what can stop this kind of thing", and the
 * overlaps as the point rather than as duplication:
 *
 * PACKAGE is ADR-0256's original eleven minus `deploy`. Deploy left
 * because it was never per-package -- ADR-0256 itself reports it ONCE
 * beside the package list rather than on every row, which is the same
 * observation this makes structural. It is the host's final stage now.
 *
 * IMAGE has no fetch/unpack/build of its own: an image does not compile
 * anything, it acquires artifacts that packages built. Its `install` IS
 * its composition step.
 *
 * DEPLOYMENT is the shortest and ends at `verify` rather than `deploy`,
 * because a started container is not yet a working one.
 *
 * HOST is where `deploy` ended up, behind the two steps that have to
 * succeed first and that fail independently of each other.
 */
/*
 * PIPELINE_STAGE_COUNT terminates each row. A hand-maintained length
 * beside a hand-written list is two statements of one fact, and the
 * one that drifts is always the number -- so there is no number.
 */
#define KIND_STAGES_END PIPELINE_STAGE_COUNT
static const enum pipeline_stage KIND_STAGES[PIPELINE_KIND_COUNT][PIPELINE_STAGE_COUNT + 1] = {
	{ PIPELINE_DISCOVER, PIPELINE_RESOLVE, PIPELINE_AUTHENTICATE, PIPELINE_AUTHOR,
	  PIPELINE_FETCH, PIPELINE_UNPACK, PIPELINE_BUILD, PIPELINE_INSTALL, PIPELINE_PUBLISH,
	  PIPELINE_ROLL, KIND_STAGES_END },
	{ PIPELINE_AUTHOR, PIPELINE_RESOLVE, PIPELINE_ACQUIRE, PIPELINE_INSTALL, PIPELINE_PUBLISH,
	  PIPELINE_ROLL, KIND_STAGES_END },
	{ PIPELINE_AUTHOR, PIPELINE_RESOLVE, PIPELINE_ACQUIRE, PIPELINE_DEPLOY, PIPELINE_VERIFY,
	  KIND_STAGES_END },
	{ PIPELINE_BUILD, PIPELINE_ASSEMBLE, PIPELINE_STAGE, PIPELINE_DEPLOY, KIND_STAGES_END },
};

const char *pipeline_kind_name(enum pipeline_kind k)
{
	if ((int)k < 0 || (int)k >= PIPELINE_KIND_COUNT)
		return "unknown";
	return KINDS[k];
}

int pipeline_kind_stages(enum pipeline_kind k, enum pipeline_stage *out)
{
	int i, n;

	if ((int)k < 0 || (int)k >= PIPELINE_KIND_COUNT || out == NULL)
		return 0;
	for (n = 0; KIND_STAGES[k][n] != KIND_STAGES_END && n < PIPELINE_STAGE_COUNT; n++)
		;
	for (i = 0; i < n; i++)
		out[i] = KIND_STAGES[k][i];
	return n;
}

static const char *const STATUSES[] = {
	"ok", "blocked", "failed", "cancelled", "not-implemented"
};

const char *pipeline_stage_name(enum pipeline_stage s)
{
	if ((int)s < 0 || s >= PIPELINE_STAGE_COUNT)
		return "unknown";
	return STAGES[s].name;
}

const char *pipeline_stage_verb(enum pipeline_stage s)
{
	if ((int)s < 0 || s >= PIPELINE_STAGE_COUNT)
		return "failed";
	return STAGES[s].verb;
}

const char *pipeline_status_name(enum pipeline_status s)
{
	if ((int)s < 0 || (size_t)s >= sizeof(STATUSES) / sizeof(STATUSES[0]))
		return "unknown";
	return STATUSES[s];
}

int pipeline_stage_from_name(const char *name, enum pipeline_stage *out)
{
	int i;

	if (name == NULL)
		return -1;
	for (i = 0; i < PIPELINE_STAGE_COUNT; i++) {
		if (strcmp(STAGES[i].name, name) == 0) {
			if (out != NULL)
				*out = (enum pipeline_stage)i;
			return 0;
		}
	}
	return -1;
}
