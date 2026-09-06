#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "json.h"

#include <stddef.h>
#include <time.h>

/*
 * ADR-0257: one scheduler for everything this platform does on a clock.
 *
 * Six periodic timers existed before this, four of them because an
 * operator had set an interval, each owning its own copy of "is it on",
 * "how often" and "when did it last run". Two of them back things up to
 * a disk on a schedule, and main.c already said so (issue #96): the
 * volume sweep rides the backup timer, two schedules on one tick with
 * separate bookkeeping. This is that observation carried to its end.
 *
 * A SCHEDULE IS STRUCTURED, AND THE STRING IS DISPLAY-ONLY.
 *
 * The decisive property of a schedule syntax is its failure mode, not
 * its expressiveness -- cron's real defect is that a mistyped
 * expression still parses and means something else. A structured body
 * has no syntax to mistype: a wrong field is a missing field, a wrong
 * value is out of range. schedule_describe() renders "daily at 02:00
 * for 3h" for a human, in ONE DIRECTION ONLY. Nothing reads it back, so
 * there is no syntax for anyone to get wrong.
 *
 * AN ACTION IS FROM A REGISTRY, NEVER FREE TEXT.
 *
 * Not a style preference: a free-text command field on a host with no
 * shell is a shell-exec endpoint wearing a friendly name. The valid
 * actions are knowable, so they are published rather than invited and
 * then rejected -- the same reasoning that makes srcupstream's channels
 * come from the discovery kind.
 */

#define SCHEDULE_NAME_MAX 64
#define SCHEDULE_ACTION_MAX 48
#define SCHEDULE_PARAMS_MAX 512
#define SCHEDULE_REASON_MAX 256
#define SCHEDULE_DESCRIBE_MAX 96
#define SCHEDULER_MAX_JOBS 64

enum schedule_kind {
	SCHEDULE_EVERY = 0, /* fixed period, anchored on the last run */
	SCHEDULE_DAILY,     /* wall-clock, local time */
	SCHEDULE_WEEKLY
};

enum schedule_error {
	SCHEDULE_OK = 0,
	SCHEDULE_ERR_INVALID_NAME,
	SCHEDULE_ERR_NOT_FOUND,
	SCHEDULE_ERR_DUPLICATE,
	SCHEDULE_ERR_FULL,
	SCHEDULE_ERR_NO_SUCH_ACTION,
	SCHEDULE_ERR_INVALID_SCHEDULE, /* the body's own shape or values */
	SCHEDULE_ERR_ACTION_FAILED,    /* it ran, and the action itself did not work */
	SCHEDULE_ERR_PERSIST
};

const char *schedule_strerror(enum schedule_error e);

/*
 * The outcome of the last run, in ADR-0256's own vocabulary rather than
 * a private one -- a scheduled backup that failed is a failure in
 * exactly the sense the pipeline view already means, and two words for
 * one idea is what ADR-0256 exists to prevent.
 */
struct schedule {
	char name[SCHEDULE_NAME_MAX];
	char action[SCHEDULE_ACTION_MAX];
	char params[SCHEDULE_PARAMS_MAX]; /* raw JSON object, "" when none */
	enum schedule_kind kind;
	int every_seconds;    /* SCHEDULE_EVERY */
	int hour;             /* DAILY/WEEKLY, 0-23 */
	int minute;           /* DAILY/WEEKLY, 0-59 */
	int weekday;          /* WEEKLY, 0=sun .. 6=sat */
	int window_minutes;   /* 0 = fires and is done; >0 = may keep starting work */
	int catch_up;         /* run once at startup if the time passed while down */
	int enabled;
	/*
	 * When this job started counting: its creation, or its last edit.
	 * Without it a job created at 20:00 with "daily at 02:00" looks
	 * like it MISSED today's 02:00, because last_run_at is 0 and every
	 * past occurrence is therefore later than it.
	 */
	long anchor_at;
	long last_run_at;     /* 0 = never */
	/*
	 * The occurrence that passed while the daemon was down and was not
	 * caught up. Recorded rather than silently swallowed: a schedule
	 * that quietly did not run is the exact thing this module exists to
	 * make visible.
	 */
	long last_skipped_at;
	long next_run_at;     /* computed, never persisted as authority */
	int last_ok;          /* meaningless when last_run_at == 0 */
	char last_reason[SCHEDULE_REASON_MAX];
	int in_use;
};

/*
 * An action is a name, a one-line summary an operator can choose from,
 * and the function that does the work.
 *
 * The function ENQUEUES; it never performs heavy work inline. Every
 * registered action calls an entry point that already exists and
 * already respects its own queue, which is what keeps a scheduler from
 * becoming a way around "never two heavy builds at once".
 *
 * Returns 0 on success. On failure writes why into `reason` -- a
 * scheduled job that silently did nothing is the failure mode this
 * whole module exists to make visible.
 */
typedef int (*schedule_action_fn)(const char *params, char *reason, size_t reason_size);

struct schedule_action {
	const char *name;
	const char *summary;
	schedule_action_fn fn;
};

/*
 * Registration is explicit and happens at startup, from main.c, which
 * is the only place that can see every subsystem's own entry points.
 * Returns 0, or -1 if the table is full or the name is already taken.
 */
int scheduler_register_action(const char *name, const char *summary, schedule_action_fn fn);
size_t scheduler_action_count(void);
const struct schedule_action *scheduler_action_at(size_t i);
const struct schedule_action *scheduler_action_find(const char *name);

int scheduler_init(const char *state_path);
void scheduler_repoint(const char *state_path);

/*
 * Parses and validates a job from a request body, then stores it.
 *
 * `name` comes from the route, never from the body, so a rename cannot
 * be smuggled in through an update. On failure writes an operator-
 * readable reason naming the offending field and its valid range.
 */
enum schedule_error scheduler_set_from_json(const char *name, const char *body, size_t body_len,
                                             char *err, size_t err_size);
enum schedule_error scheduler_delete(const char *name);
const struct schedule *scheduler_find(const char *name);

/*
 * When this job should next fire, given now -- for DISPLAY. Returns 0
 * for a disabled job (never), which is different from one whose next
 * run has passed.
 *
 * NOT the due test. For a wall-clock job this is always strictly in the
 * future by construction, so "is it due" cannot be `next_run <= now` --
 * that comparison is never true and every daily job would silently
 * never fire. scheduler_run_due() compares the most recent occurrence
 * against what the job has already done instead.
 *
 * SCHEDULE_EVERY is anchored on the last run rather than on a fixed
 * epoch: a period that counts from an epoch fires immediately on every
 * daemon restart if the box has been down longer than the period, which
 * turns a reboot into a burst of work.
 */
long schedule_next_run(const struct schedule *s, long now);

/*
 * Is `now` still inside this job's window? Always true for a job with
 * no window, so a caller need not special-case one.
 */
int scheduler_in_window(const struct schedule *s, long now);

/* "daily at 02:00 for 3h". Display only -- see the header comment. */
void schedule_describe(const struct schedule *s, char *out, size_t out_size);

/*
 * Runs every job whose time has come, recording each outcome. Called
 * from one timer in main.c. `startup` is 1 on the first call after
 * boot, which is the only time `catch_up` is consulted.
 */
void scheduler_run_due(long now, int startup);

/* The soonest any job is due, or 0 when nothing is scheduled. */
long scheduler_next_due(long now);

/* Runs one job now regardless of its schedule, recording the outcome. */
enum schedule_error scheduler_run_now(const char *name, char *err, size_t err_size);

void scheduler_write_json(struct json_writer *w);
void scheduler_write_json_one(const struct schedule *s, struct json_writer *w);
void scheduler_write_actions_json(struct json_writer *w);

#endif /* SCHEDULER_H */
