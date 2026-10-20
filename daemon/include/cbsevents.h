#ifndef CIX_CBSEVENTS_H
#define CIX_CBSEVENTS_H

#include <stddef.h>

/*
 * #388, #489 (ADR-0275): where a CPDL build is, and what failed, read from
 * CBS's own JSONL build event stream rather than from the human log.
 *
 * cixd runs `cbs build ... --events human --events-file F` (cbs v0.1.107,
 * cix-build-system#282): the operator's log keeps the human reporter on
 * stderr, and F carries one JSON object per line -- phase-begin/phase-end,
 * command-begin/command-end, build-begin/build-end and others -- flushed
 * after every event. This module folds those lines into a progress record.
 * It is pure: it owns no file, no clock and no allocation, so a unit test
 * can drive it with literal lines.
 *
 * A phase is a diagnostic, never a gate (ADR-0275): nothing here decides
 * whether a build succeeded. The build container's exit does that.
 */

/* ADR-0275's closed vocabulary, in its order. The first five are CPDL's
 * own phase words; finalize is what cbs does after the recipe's last
 * phase -- the platform's finalize command (ADR-0251), pruning and
 * writing the artifact -- and has no event of its own. */
enum cbs_phase {
	CBS_PHASE_PREPARE = 0,
	CBS_PHASE_CONFIGURE,
	CBS_PHASE_BUILD,
	CBS_PHASE_CHECK,
	CBS_PHASE_INSTALL,
	CBS_PHASE_FINALIZE,
	CBS_PHASE_COUNT
};

enum cbs_phase_state {
	CBS_PHASE_NOT_PLANNED = 0, /* the recipe declares no such phase: not-implemented */
	CBS_PHASE_PENDING,         /* planned, not reached (yet) */
	CBS_PHASE_RUNNING,
	CBS_PHASE_OK,
	CBS_PHASE_FAILED,
	CBS_PHASE_BLOCKED,   /* planned, and an earlier failure meant it never ran */
	CBS_PHASE_CANCELLED, /* it was running when the build was cancelled */
};

#define CBS_EVENTS_COMMAND_MAX 192
#define CBS_EVENTS_LOG_MAX 128
#define CBS_EVENTS_MESSAGE_MAX 64

struct cbs_build_progress {
	enum cbs_phase_state state[CBS_PHASE_COUNT];
	long duration_ms[CBS_PHASE_COUNT]; /* -1 until the phase ends */
	int current;                       /* the running phase, or -1 */
	int failed;                        /* the phase that failed, or -1 */
	/* The command that failed, as cbs named it: the program, then its
	 * arguments. cbs reports only that a command did not exit with the
	 * status its recipe expected (command-end's status is 0 or 1), never
	 * the exit code itself, so none is kept. */
	char failed_command[CBS_EVENTS_COMMAND_MAX];
	/* cbs's per-command log, a path INSIDE the build container. It exists
	 * only while that container does (keep_on_failure keeps it). */
	char failed_log[CBS_EVENTS_LOG_MAX];
	char failed_message[CBS_EVENTS_MESSAGE_MAX];
	int build_end;               /* -1 unseen, 0 ok, 1 failed */
	unsigned long long sequence; /* the last event folded in */
	int changed;                 /* a phase or the failed command changed; the caller clears it */
	/* The latest command-begin, so a command-end can name what failed. */
	char last_command[CBS_EVENTS_COMMAND_MAX];
	char last_log[CBS_EVENTS_LOG_MAX];
};

/* "prepare" .. "finalize", or NULL out of range. */
const char *cbs_phase_name(int phase);
/* The phase a name means, or -1 for a word outside the vocabulary. */
int cbs_phase_from_name(const char *name);

/*
 * What a phase state is reported as: "ok", "failed", "blocked",
 * "cancelled" or "not-implemented" -- ADR-0256's status axis -- and NULL
 * for a phase that is planned but has no outcome yet (pending or running).
 */
const char *cbs_phase_state_status(enum cbs_phase_state s);

/* A state's own word ("not-planned", "pending", "running", "ok",
 * "failed", "blocked", "cancelled"), for the run store, and back. The
 * second returns -1 for a word it does not know. */
const char *cbs_phase_state_word(enum cbs_phase_state s);
int cbs_phase_state_from_word(const char *word);

/*
 * Starts a build's progress from its plan: bit (1u << phase) set for each
 * phase the recipe declares. finalize is always planned, since cbs always
 * finishes the tree after the recipe's phases.
 */
void cbs_progress_init(struct cbs_build_progress *p, unsigned planned);

/*
 * Folds one line of the stream in. A line that is not a JSON object, or
 * an event older than the last one folded, changes nothing. Returns 0, or
 * -1 for a line that did not parse.
 */
int cbs_progress_feed(struct cbs_build_progress *p, const char *line, size_t len);

/*
 * The build container exited. A phase still running was killed, so it
 * failed -- or was cancelled, if the build was. Planned phases that never
 * started are blocked when the build did not succeed.
 */
void cbs_progress_finish(struct cbs_build_progress *p, int succeeded, int cancelled);

#endif /* CIX_CBSEVENTS_H */
