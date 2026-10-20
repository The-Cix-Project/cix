#include "cbsevents.h"
#include "json.h"

#include <stdio.h>
#include <string.h>

static const char *const g_phase_names[CBS_PHASE_COUNT] = {
	"prepare", "configure", "build", "check", "install", "finalize",
};

const char *cbs_phase_name(int phase)
{
	if (phase < 0 || phase >= CBS_PHASE_COUNT)
		return NULL;
	return g_phase_names[phase];
}

int cbs_phase_from_name(const char *name)
{
	int i;

	if (name == NULL)
		return -1;
	for (i = 0; i < CBS_PHASE_COUNT; i++)
		if (strcmp(name, g_phase_names[i]) == 0)
			return i;
	return -1;
}

const char *cbs_phase_state_status(enum cbs_phase_state s)
{
	switch (s) {
	case CBS_PHASE_NOT_PLANNED:
		return "not-implemented";
	case CBS_PHASE_OK:
		return "ok";
	case CBS_PHASE_FAILED:
		return "failed";
	case CBS_PHASE_BLOCKED:
		return "blocked";
	case CBS_PHASE_CANCELLED:
		return "cancelled";
	default:
		return NULL;
	}
}

static const char *const g_state_words[] = {
	"not-planned", "pending", "running", "ok", "failed", "blocked", "cancelled",
};

const char *cbs_phase_state_word(enum cbs_phase_state s)
{
	if ((int)s < 0 || (size_t)s >= sizeof(g_state_words) / sizeof(g_state_words[0]))
		return "pending";
	return g_state_words[s];
}

int cbs_phase_state_from_word(const char *word)
{
	size_t i;

	if (word == NULL)
		return -1;
	for (i = 0; i < sizeof(g_state_words) / sizeof(g_state_words[0]); i++)
		if (strcmp(word, g_state_words[i]) == 0)
			return (int)i;
	return -1;
}

void cbs_progress_init(struct cbs_build_progress *p, unsigned planned)
{
	int i;

	memset(p, 0, sizeof(*p));
	for (i = 0; i < CBS_PHASE_COUNT; i++) {
		p->state[i] = (planned & (1u << i)) != 0 || i == CBS_PHASE_FINALIZE ? CBS_PHASE_PENDING
		                                                                    : CBS_PHASE_NOT_PLANNED;
		p->duration_ms[i] = -1;
	}
	p->current = -1;
	p->failed = -1;
	p->build_end = -1;
}

static const char *str_field(const struct json_value *root, const char *key)
{
	const char *s = json_as_string(json_object_get(root, key));

	return s != NULL ? s : "";
}

static long num_field(const struct json_value *root, const char *key)
{
	const struct json_value *v = json_object_get(root, key);

	return v != NULL && v->type == JSON_NUMBER ? (long)json_as_number(v) : 0;
}

/* A phase the recipe did not declare still gets reported if cbs runs it,
 * so the plan never hides what really happened. */
static void phase_begin(struct cbs_build_progress *p, int phase)
{
	p->state[phase] = CBS_PHASE_RUNNING;
	p->current = phase;
	p->changed = 1;
}

static void phase_end(struct cbs_build_progress *p, int phase, int status, long duration_ms)
{
	p->state[phase] = status == 0 ? CBS_PHASE_OK : CBS_PHASE_FAILED;
	p->duration_ms[phase] = duration_ms;
	if (status != 0 && p->failed < 0)
		p->failed = phase;
	if (p->current == phase)
		p->current = -1;
	p->changed = 1;
}

int cbs_progress_feed(struct cbs_build_progress *p, const char *line, size_t len)
{
	struct json_value *root;
	const char *type;
	int phase, status;

	if (len == 0)
		return 0;
	root = json_parse(line, len);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		return -1;
	}
	/*
	 * `sequence` is deliberately not used. cbs documents it as rising
	 * within a build, and it does not: commands run in a phase-local
	 * COPY of the execution context (cbs v0.1.107 src/runtime.c:258,
	 * `CbsExecutionContext local = *context;`), whose counter advances
	 * and is never carried back, so a phase's phase-end can carry a
	 * lower number than the commands inside it. Measured on
	 * 192.168.15.95, 2026-10-20: dropping "older" events lost every
	 * phase-end. Each line is read exactly once anyway -- the reader
	 * keeps a byte offset -- so order is the file's.
	 */
	type = str_field(root, "type");
	phase = cbs_phase_from_name(str_field(root, "phase"));
	status = (int)num_field(root, "status");

	if (strcmp(type, "phase-begin") == 0 && phase >= 0) {
		phase_begin(p, phase);
	} else if (strcmp(type, "phase-end") == 0 && phase >= 0) {
		phase_end(p, phase, status, num_field(root, "duration_ms"));
	} else if (strcmp(type, "command-begin") == 0) {
		/* `arguments` is the whole argv, program included (cbs sets it
		 * from the command's own argument list); `command` alone is the
		 * program, for an event that carries no arguments. Measured: the
		 * two joined read "false false". */
		const char *args = str_field(root, "arguments");

		snprintf(p->last_command, sizeof(p->last_command), "%s",
		         args[0] != '\0' ? args : str_field(root, "command"));
		snprintf(p->last_log, sizeof(p->last_log), "%s", str_field(root, "log_path"));
	} else if (strcmp(type, "command-end") == 0 && status != 0) {
		/* The first failure is the one that ended the build; a later
		 * command can only be a recipe's own on_fail diagnostics. */
		if (p->failed_command[0] == '\0') {
			snprintf(p->failed_command, sizeof(p->failed_command), "%s",
			         p->last_command[0] != '\0' ? p->last_command : str_field(root, "command"));
			snprintf(p->failed_log, sizeof(p->failed_log), "%s", p->last_log);
			snprintf(p->failed_message, sizeof(p->failed_message), "%s",
			         str_field(root, "message"));
			p->changed = 1;
		}
	} else if (strcmp(type, "build-end") == 0) {
		p->build_end = status != 0 ? 1 : 0;
		/* finalize has no event of its own: it is what cbs does between
		 * the recipe's last phase and build-end. So it succeeded with the
		 * build, and it is what failed when every phase ended well and
		 * the build did not. */
		if (status == 0) {
			p->state[CBS_PHASE_FINALIZE] = CBS_PHASE_OK;
		} else if (p->failed < 0 && p->current < 0) {
			p->state[CBS_PHASE_FINALIZE] = CBS_PHASE_FAILED;
			p->failed = CBS_PHASE_FINALIZE;
		}
		p->changed = 1;
	}
	json_free(root);
	return 0;
}

void cbs_progress_finish(struct cbs_build_progress *p, int succeeded, int cancelled)
{
	int i;

	if (p->current >= 0) {
		/* Begun and never ended: the build was killed inside it. */
		p->state[p->current] = cancelled ? CBS_PHASE_CANCELLED : CBS_PHASE_FAILED;
		if (!cancelled && p->failed < 0)
			p->failed = p->current;
		p->current = -1;
		p->changed = 1;
	}
	for (i = 0; i < CBS_PHASE_COUNT; i++) {
		if (p->state[i] != CBS_PHASE_PENDING)
			continue;
		/* A successful build with no build-end recorded still finished
		 * the tree, since cbs exits 0 only after writing the artifact. */
		p->state[i] = succeeded && i == CBS_PHASE_FINALIZE ? CBS_PHASE_OK
		              : succeeded                          ? CBS_PHASE_PENDING
		              : cancelled                          ? CBS_PHASE_CANCELLED
		                                                   : CBS_PHASE_BLOCKED;
		p->changed = 1;
	}
}
