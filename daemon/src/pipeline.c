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
};

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
