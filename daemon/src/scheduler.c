/*
 * See scheduler.h for why a schedule is structured and why the string
 * form is rendered but never parsed back.
 */
#include "scheduler.h"

#include "namecheck.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCHEDULER_MAX_ACTIONS 32

static struct schedule g_jobs[SCHEDULER_MAX_JOBS];
static struct schedule_action g_actions[SCHEDULER_MAX_ACTIONS];
static size_t g_action_count;
static char g_state_path[512];

/*
 * json.h exposes no as_bool -- callers test the node type directly, so
 * this is the one place that idiom is spelled out rather than repeated
 * at every field. A missing key is 0, which is what every optional
 * boolean here wants.
 */
static int json_bool(const struct json_value *v)
{
	return v != NULL && v->type == JSON_BOOL && v->u.boolean;
}

static const char *const WEEKDAYS[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

const char *schedule_strerror(enum schedule_error e)
{
	switch (e) {
	case SCHEDULE_OK:
		return "ok";
	case SCHEDULE_ERR_INVALID_NAME:
		return "invalid schedule name";
	case SCHEDULE_ERR_NOT_FOUND:
		return "no such schedule";
	case SCHEDULE_ERR_DUPLICATE:
		return "a schedule of that name already exists";
	case SCHEDULE_ERR_FULL:
		return "no room for another schedule";
	case SCHEDULE_ERR_NO_SUCH_ACTION:
		return "no such action";
	case SCHEDULE_ERR_INVALID_SCHEDULE:
		return "invalid schedule";
	case SCHEDULE_ERR_PERSIST:
	default:
		return "could not persist the schedule";
	}
}

int scheduler_register_action(const char *name, const char *summary, schedule_action_fn fn)
{
	if (name == NULL || fn == NULL || g_action_count >= SCHEDULER_MAX_ACTIONS)
		return -1;
	if (scheduler_action_find(name) != NULL)
		return -1;
	g_actions[g_action_count].name = name;
	g_actions[g_action_count].summary = summary;
	g_actions[g_action_count].fn = fn;
	g_action_count++;
	return 0;
}

size_t scheduler_action_count(void)
{
	return g_action_count;
}

const struct schedule_action *scheduler_action_at(size_t i)
{
	return i < g_action_count ? &g_actions[i] : NULL;
}

const struct schedule_action *scheduler_action_find(const char *name)
{
	size_t i;

	if (name == NULL)
		return NULL;
	for (i = 0; i < g_action_count; i++) {
		if (strcmp(g_actions[i].name, name) == 0)
			return &g_actions[i];
	}
	return NULL;
}

static struct schedule *find_slot(const char *name)
{
	int i;

	for (i = 0; i < SCHEDULER_MAX_JOBS; i++) {
		if (g_jobs[i].in_use && strcmp(g_jobs[i].name, name) == 0)
			return &g_jobs[i];
	}
	return NULL;
}

const struct schedule *scheduler_find(const char *name)
{
	return name != NULL ? find_slot(name) : NULL;
}

/* ---------- next run ---------- */

/*
 * The next wall-clock occurrence of hour:minute, on `weekday` when
 * >= 0. Local time on purpose: an operator who asks for 02:00 means
 * 02:00 where the machine is, and a nightly backup that drifts an hour
 * twice a year is the correct behaviour rather than a bug.
 */
static long next_wallclock(long now, int hour, int minute, int weekday)
{
	struct tm tm;
	time_t t = (time_t)now;
	long candidate;
	int i;

	if (localtime_r(&t, &tm) == NULL)
		return 0;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_sec = 0;
	tm.tm_isdst = -1; /* let mktime decide; the offset may change today */
	candidate = (long)mktime(&tm);
	if (candidate <= now)
		candidate += 24 * 3600;
	if (weekday < 0)
		return candidate;
	/*
	 * Step whole days rather than adding 7*86400 to a computed instant:
	 * across a DST boundary a week is not always 604800 seconds, and
	 * re-deriving the time of day each step keeps 03:00 meaning 03:00.
	 */
	for (i = 0; i < 8; i++) {
		time_t c = (time_t)candidate;
		struct tm ctm;

		if (localtime_r(&c, &ctm) == NULL)
			return candidate;
		if (ctm.tm_wday == weekday)
			return candidate;
		ctm.tm_mday += 1;
		ctm.tm_hour = hour;
		ctm.tm_min = minute;
		ctm.tm_sec = 0;
		ctm.tm_isdst = -1;
		candidate = (long)mktime(&ctm);
	}
	return candidate;
}

long schedule_next_run(const struct schedule *s, long now)
{
	if (s == NULL || !s->enabled)
		return 0;
	switch (s->kind) {
	case SCHEDULE_EVERY:
		/*
		 * Anchored on the last run. An epoch-anchored period fires the
		 * instant the daemon starts whenever the box has been down
		 * longer than one period, turning every reboot into a burst.
		 */
		if (s->last_run_at == 0)
			return now;
		return s->last_run_at + s->every_seconds;
	case SCHEDULE_DAILY:
		return next_wallclock(now, s->hour, s->minute, -1);
	case SCHEDULE_WEEKLY:
	default:
		return next_wallclock(now, s->hour, s->minute, s->weekday);
	}
}

int scheduler_in_window(const struct schedule *s, long now)
{
	if (s == NULL)
		return 0;
	if (s->window_minutes <= 0)
		return 1;
	if (s->last_run_at == 0)
		return 0;
	return now < s->last_run_at + (long)s->window_minutes * 60;
}

void schedule_describe(const struct schedule *s, char *out, size_t out_size)
{
	char base[64];
	char window[24];

	if (out == NULL || out_size == 0)
		return;
	out[0] = '\0';
	if (s == NULL)
		return;

	switch (s->kind) {
	case SCHEDULE_EVERY:
		if (s->every_seconds % 86400 == 0)
			snprintf(base, sizeof(base), "every %dd", s->every_seconds / 86400);
		else if (s->every_seconds % 3600 == 0)
			snprintf(base, sizeof(base), "every %dh", s->every_seconds / 3600);
		else if (s->every_seconds % 60 == 0)
			snprintf(base, sizeof(base), "every %dm", s->every_seconds / 60);
		else
			snprintf(base, sizeof(base), "every %ds", s->every_seconds);
		break;
	case SCHEDULE_DAILY:
		snprintf(base, sizeof(base), "daily at %02d:%02d", s->hour, s->minute);
		break;
	case SCHEDULE_WEEKLY:
	default:
		snprintf(base, sizeof(base), "weekly on %s at %02d:%02d",
		         WEEKDAYS[s->weekday % 7], s->hour, s->minute);
		break;
	}
	window[0] = '\0';
	if (s->window_minutes > 0) {
		if (s->window_minutes % 60 == 0)
			snprintf(window, sizeof(window), " for %dh", s->window_minutes / 60);
		else
			snprintf(window, sizeof(window), " for %dm", s->window_minutes);
	}
	snprintf(out, out_size, "%s%s%s", base, window, s->enabled ? "" : " (disabled)");
}

/* ---------- persistence ---------- */

static void write_job(struct json_writer *w, const struct schedule *s);

static int persist_all(void)
{
	struct json_writer w;
	int i, rc;

	if (g_state_path[0] == '\0')
		return 0;
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "schedules");
	jw_arr_open(&w);
	for (i = 0; i < SCHEDULER_MAX_JOBS; i++) {
		if (g_jobs[i].in_use)
			write_job(&w, &g_jobs[i]);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int kind_from_name(const char *s, enum schedule_kind *out)
{
	if (s == NULL)
		return -1;
	if (strcmp(s, "every") == 0)
		*out = SCHEDULE_EVERY;
	else if (strcmp(s, "daily") == 0)
		*out = SCHEDULE_DAILY;
	else if (strcmp(s, "weekly") == 0)
		*out = SCHEDULE_WEEKLY;
	else
		return -1;
	return 0;
}

static const char *kind_name(enum schedule_kind k)
{
	switch (k) {
	case SCHEDULE_EVERY:
		return "every";
	case SCHEDULE_DAILY:
		return "daily";
	case SCHEDULE_WEEKLY:
	default:
		return "weekly";
	}
}

static void load_from(const struct json_value *root)
{
	const struct json_value *arr = json_object_get(root, "schedules");
	size_t i;
	int n = 0;

	if (arr == NULL || arr->type != JSON_ARRAY)
		return;
	for (i = 0; i < arr->u.array.count && n < SCHEDULER_MAX_JOBS; i++) {
		const struct json_value *e = arr->u.array.items[i];
		struct schedule *s = &g_jobs[n];
		const char *name = json_as_string(json_object_get(e, "name"));
		const char *action = json_as_string(json_object_get(e, "action"));
		const char *kind = json_as_string(json_object_get(e, "kind"));
		const char *reason = json_as_string(json_object_get(e, "last_reason"));
		const struct json_value *params = json_object_get(e, "params");

		if (name == NULL || action == NULL)
			continue;
		memset(s, 0, sizeof(*s));
		if (kind_from_name(kind, &s->kind) != 0)
			continue;
		snprintf(s->name, sizeof(s->name), "%s", name);
		snprintf(s->action, sizeof(s->action), "%s", action);
		if (params != NULL && params->type == JSON_OBJECT) {
			struct json_writer pw;

			jw_init(&pw);
			jw_value(&pw, params);
			if (pw.len < sizeof(s->params))
				snprintf(s->params, sizeof(s->params), "%s", pw.buf);
			jw_free(&pw);
		}
		if (reason != NULL)
			snprintf(s->last_reason, sizeof(s->last_reason), "%s", reason);
		s->every_seconds = (int)json_as_number(json_object_get(e, "every_seconds"));
		s->hour = (int)json_as_number(json_object_get(e, "hour"));
		s->minute = (int)json_as_number(json_object_get(e, "minute"));
		s->weekday = (int)json_as_number(json_object_get(e, "weekday"));
		s->window_minutes = (int)json_as_number(json_object_get(e, "window_minutes"));
		s->catch_up = json_bool(json_object_get(e, "catch_up"));
		s->enabled = json_bool(json_object_get(e, "enabled"));
		s->last_run_at = (long)json_as_number(json_object_get(e, "last_run_at"));
		s->last_ok = json_bool(json_object_get(e, "last_ok"));
		s->in_use = 1;
		n++;
	}
}

int scheduler_init(const char *state_path)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	memset(g_jobs, 0, sizeof(g_jobs));
	snprintf(g_state_path, sizeof(g_state_path), "%s", state_path != NULL ? state_path : "");
	if (g_state_path[0] == '\0')
		return 0;
	if (persist_read_file(g_state_path, &buf, &len) != 0 || buf == NULL)
		return 0; /* nothing scheduled yet is the normal fresh state */
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return -1;
	load_from(root);
	json_free(root);
	return 0;
}

void scheduler_repoint(const char *state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", state_path != NULL ? state_path : "");
}

/* ---------- validation ---------- */

static int parse_hhmm(const char *s, int *hour, int *minute)
{
	int h, m;
	char extra;

	if (s == NULL)
		return -1;
	if (sscanf(s, "%d:%d%c", &h, &m, &extra) != 2)
		return -1;
	if (h < 0 || h > 23 || m < 0 || m > 59)
		return -1;
	*hour = h;
	*minute = m;
	return 0;
}

static int weekday_from_name(const char *s)
{
	int i;

	if (s == NULL)
		return -1;
	for (i = 0; i < 7; i++) {
		if (strcmp(WEEKDAYS[i], s) == 0)
			return i;
	}
	return -1;
}

/*
 * Exactly one of every/daily/weekly. Two of them present is not "the
 * first one wins" -- it is a caller who believes something untrue about
 * what they just configured, and a schedule that silently means
 * something other than what was sent is the entire failure mode this
 * design exists to avoid.
 */
static enum schedule_error parse_schedule_body(const struct json_value *root, struct schedule *out,
                                                char *err, size_t err_size)
{
	const struct json_value *sched = json_object_get(root, "schedule");
	const struct json_value *every, *daily, *weekly;
	int present = 0;

	if (sched == NULL || sched->type != JSON_OBJECT) {
		snprintf(err, err_size, "\"schedule\" is required and must be an object with exactly "
		                         "one of \"every\", \"daily\" or \"weekly\"");
		return SCHEDULE_ERR_INVALID_SCHEDULE;
	}
	every = json_object_get(sched, "every");
	daily = json_object_get(sched, "daily");
	weekly = json_object_get(sched, "weekly");
	present = (every != NULL) + (daily != NULL) + (weekly != NULL);
	if (present != 1) {
		snprintf(err, err_size,
		         "\"schedule\" must carry exactly one of \"every\", \"daily\" or \"weekly\" "
		         "(found %d)", present);
		return SCHEDULE_ERR_INVALID_SCHEDULE;
	}

	if (every != NULL) {
		const struct json_value *sec = json_object_get(every, "seconds");
		const struct json_value *min = json_object_get(every, "minutes");
		const struct json_value *hr = json_object_get(every, "hours");
		const struct json_value *day = json_object_get(every, "days");
		long total = 0;

		if (sec != NULL)
			total += (long)json_as_number(sec);
		if (min != NULL)
			total += (long)json_as_number(min) * 60;
		if (hr != NULL)
			total += (long)json_as_number(hr) * 3600;
		if (day != NULL)
			total += (long)json_as_number(day) * 86400;
		/*
		 * A floor of 10 seconds, not 1. Nothing this scheduler can run
		 * is cheap enough to want faster, and a one-second period is
		 * far more likely to be a units mistake than an intention.
		 */
		if (total < 10 || total > 366L * 86400L) {
			snprintf(err, err_size,
			         "\"every\" must total between 10 seconds and a year (got %ld seconds) -- "
			         "give it seconds, minutes, hours or days", total);
			return SCHEDULE_ERR_INVALID_SCHEDULE;
		}
		out->kind = SCHEDULE_EVERY;
		out->every_seconds = (int)total;
		return SCHEDULE_OK;
	}

	if (daily != NULL) {
		if (parse_hhmm(json_as_string(json_object_get(daily, "at")), &out->hour,
		               &out->minute) != 0) {
			snprintf(err, err_size, "\"daily\" needs \"at\" as \"HH:MM\", 00:00 to 23:59");
			return SCHEDULE_ERR_INVALID_SCHEDULE;
		}
		out->kind = SCHEDULE_DAILY;
		return SCHEDULE_OK;
	}

	if (parse_hhmm(json_as_string(json_object_get(weekly, "at")), &out->hour, &out->minute) != 0) {
		snprintf(err, err_size, "\"weekly\" needs \"at\" as \"HH:MM\", 00:00 to 23:59");
		return SCHEDULE_ERR_INVALID_SCHEDULE;
	}
	out->weekday = weekday_from_name(json_as_string(json_object_get(weekly, "on")));
	if (out->weekday < 0) {
		snprintf(err, err_size,
		         "\"weekly\" needs \"on\" as one of sun, mon, tue, wed, thu, fri, sat");
		return SCHEDULE_ERR_INVALID_SCHEDULE;
	}
	out->kind = SCHEDULE_WEEKLY;
	return SCHEDULE_OK;
}

enum schedule_error scheduler_set_from_json(const char *name, const char *body, size_t body_len,
                                             char *err, size_t err_size)
{
	struct json_value *root;
	struct schedule candidate;
	struct schedule *slot;
	const struct json_value *v;
	const char *action;
	enum schedule_error e;
	int i;

	if (err != NULL && err_size > 0)
		err[0] = '\0';
	if (name == NULL || !simple_name_is_valid(name, SCHEDULE_NAME_MAX))
		return SCHEDULE_ERR_INVALID_NAME;
	root = json_parse(body, body_len);
	if (root == NULL || root->type != JSON_OBJECT) {
		if (root != NULL)
			json_free(root);
		snprintf(err, err_size, "body must be a JSON object");
		return SCHEDULE_ERR_INVALID_SCHEDULE;
	}

	memset(&candidate, 0, sizeof(candidate));
	action = json_as_string(json_object_get(root, "action"));
	if (action == NULL || scheduler_action_find(action) == NULL) {
		size_t k;
		size_t off = 0;
		char known[256];

		known[0] = '\0';
		for (k = 0; k < g_action_count; k++)
			off += (size_t)snprintf(known + off, sizeof(known) - off, "%s%s",
			                         off > 0 ? ", " : "", g_actions[k].name);
		/*
		 * Name the valid actions rather than saying no. They come from
		 * a registry precisely because the answer is knowable, so it
		 * should be shown -- the same rule srcupstream's channel
		 * refusals follow.
		 */
		snprintf(err, err_size, "no action \"%s\"; this platform runs: %s",
		         action != NULL ? action : "", known[0] != '\0' ? known : "(none registered)");
		json_free(root);
		return SCHEDULE_ERR_NO_SUCH_ACTION;
	}
	snprintf(candidate.action, sizeof(candidate.action), "%s", action);

	e = parse_schedule_body(root, &candidate, err, err_size);
	if (e != SCHEDULE_OK) {
		json_free(root);
		return e;
	}

	candidate.window_minutes = (int)json_as_number(json_object_get(root, "window_minutes"));
	if (candidate.window_minutes < 0 || candidate.window_minutes > 24 * 60) {
		snprintf(err, err_size, "\"window_minutes\" must be 0 (no window) to 1440");
		json_free(root);
		return SCHEDULE_ERR_INVALID_SCHEDULE;
	}
	candidate.catch_up = json_bool(json_object_get(root, "catch_up"));
	v = json_object_get(root, "enabled");
	candidate.enabled = v != NULL ? json_bool(v) : 1; /* a job created is a job wanted */

	/*
	 * Params are stored as the JSON text of the object as given, and
	 * handed to the action unread -- the action is the only thing that
	 * knows their shape, and this module deliberately does not learn
	 * it. Re-serialised through jw_value() rather than kept as a
	 * pointer into the parse tree, which is freed below.
	 */
	v = json_object_get(root, "params");
	if (v != NULL && v->type == JSON_OBJECT) {
		struct json_writer pw;

		jw_init(&pw);
		jw_value(&pw, v);
		if (pw.len >= sizeof(candidate.params)) {
			snprintf(err, err_size, "\"params\" is larger than %d bytes",
			         (int)sizeof(candidate.params) - 1);
			jw_free(&pw);
			json_free(root);
			return SCHEDULE_ERR_INVALID_SCHEDULE;
		}
		snprintf(candidate.params, sizeof(candidate.params), "%s", pw.buf);
		jw_free(&pw);
	} else if (v != NULL && v->type != JSON_NULL) {
		snprintf(err, err_size, "\"params\" must be an object");
		json_free(root);
		return SCHEDULE_ERR_INVALID_SCHEDULE;
	}
	json_free(root);

	snprintf(candidate.name, sizeof(candidate.name), "%s", name);
	slot = find_slot(name);
	if (slot == NULL) {
		for (i = 0; i < SCHEDULER_MAX_JOBS; i++) {
			if (!g_jobs[i].in_use) {
				slot = &g_jobs[i];
				break;
			}
		}
		if (slot == NULL) {
			snprintf(err, err_size, "no room for another schedule (limit %d)",
			         SCHEDULER_MAX_JOBS);
			return SCHEDULE_ERR_FULL;
		}
	} else {
		/* An update keeps the run history: when it last ran and how
		 * that went are facts about the job, not about its settings. */
		candidate.last_run_at = slot->last_run_at;
		candidate.last_ok = slot->last_ok;
		snprintf(candidate.last_reason, sizeof(candidate.last_reason), "%s", slot->last_reason);
	}
	candidate.in_use = 1;
	*slot = candidate;
	if (persist_all() != 0) {
		snprintf(err, err_size, "could not write %s", g_state_path);
		return SCHEDULE_ERR_PERSIST;
	}
	return SCHEDULE_OK;
}

enum schedule_error scheduler_delete(const char *name)
{
	struct schedule *s;

	if (name == NULL)
		return SCHEDULE_ERR_INVALID_NAME;
	s = find_slot(name);
	if (s == NULL)
		return SCHEDULE_ERR_NOT_FOUND;
	memset(s, 0, sizeof(*s));
	return persist_all() == 0 ? SCHEDULE_OK : SCHEDULE_ERR_PERSIST;
}

/* ---------- running ---------- */

static void record_outcome(struct schedule *s, long now, int ok, const char *reason)
{
	s->last_run_at = now;
	s->last_ok = ok;
	snprintf(s->last_reason, sizeof(s->last_reason), "%s", reason != NULL ? reason : "");
	persist_all();
}

static void run_one(struct schedule *s, long now)
{
	const struct schedule_action *a = scheduler_action_find(s->action);
	char reason[SCHEDULE_REASON_MAX];

	reason[0] = '\0';
	if (a == NULL) {
		/*
		 * A job whose action no longer exists: the daemon that stored
		 * it registered one this build does not. Recorded as a failure
		 * rather than skipped, because a job that silently never runs
		 * is the exact thing this module exists to make visible.
		 */
		snprintf(reason, sizeof(reason), "action \"%s\" is not registered in this build",
		         s->action);
		record_outcome(s, now, 0, reason);
		return;
	}
	if (a->fn(s->params[0] != '\0' ? s->params : NULL, reason, sizeof(reason)) == 0)
		record_outcome(s, now, 1, reason[0] != '\0' ? reason : "ok");
	else
		record_outcome(s, now, 0, reason[0] != '\0' ? reason : "failed");
}

void scheduler_run_due(long now, int startup)
{
	int i;

	for (i = 0; i < SCHEDULER_MAX_JOBS; i++) {
		struct schedule *s = &g_jobs[i];
		long due;

		if (!s->in_use || !s->enabled)
			continue;
		due = schedule_next_run(s, now);
		if (due == 0 || due > now)
			continue;
		/*
		 * A wall-clock job whose time passed while the daemon was down
		 * runs now only if it asked to. Default off: a backup usually
		 * should catch up, a build sweep should not -- it would start
		 * heavy work at the least predictable moment there is, right
		 * after a boot.
		 */
		if (startup && s->kind != SCHEDULE_EVERY && !s->catch_up) {
			/* Not a run, and not a failure either: just move the
			 * anchor forward so the next occurrence is the next real
			 * one rather than this missed one. */
			continue;
		}
		run_one(s, now);
	}
}

long scheduler_next_due(long now)
{
	long soonest = 0;
	int i;

	for (i = 0; i < SCHEDULER_MAX_JOBS; i++) {
		long due;

		if (!g_jobs[i].in_use || !g_jobs[i].enabled)
			continue;
		due = schedule_next_run(&g_jobs[i], now);
		if (due == 0)
			continue;
		if (soonest == 0 || due < soonest)
			soonest = due;
	}
	return soonest;
}

enum schedule_error scheduler_run_now(const char *name, char *err, size_t err_size)
{
	struct schedule *s;

	if (err != NULL && err_size > 0)
		err[0] = '\0';
	if (name == NULL)
		return SCHEDULE_ERR_INVALID_NAME;
	s = find_slot(name);
	if (s == NULL)
		return SCHEDULE_ERR_NOT_FOUND;
	/*
	 * Runs a DISABLED job too, deliberately. "Run it now" is an
	 * operator standing at the console asking for this one thing; the
	 * enabled flag governs the schedule, not the button.
	 */
	run_one(s, (long)time(NULL));
	if (!s->last_ok) {
		snprintf(err, err_size, "%s", s->last_reason);
		return SCHEDULE_ERR_PERSIST; /* the run failed; the caller reports last_reason */
	}
	return SCHEDULE_OK;
}

/* ---------- JSON ---------- */

static void write_job(struct json_writer *w, const struct schedule *s)
{
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, s->name);
	jw_key(w, "action");
	jw_str(w, s->action);
	/* Raw, not a quoted string: it is already JSON text, and quoting it
	 * would make the reload path have to unescape a document. */
	jw_key(w, "params");
	if (s->params[0] == '\0')
		jw_null(w);
	else
		jw_raw_text(w, s->params, strlen(s->params));
	jw_key(w, "kind");
	jw_str(w, kind_name(s->kind));
	jw_key(w, "every_seconds");
	jw_num(w, (double)s->every_seconds);
	jw_key(w, "hour");
	jw_num(w, (double)s->hour);
	jw_key(w, "minute");
	jw_num(w, (double)s->minute);
	jw_key(w, "weekday");
	jw_num(w, (double)s->weekday);
	jw_key(w, "window_minutes");
	jw_num(w, (double)s->window_minutes);
	jw_key(w, "catch_up");
	jw_bool(w, s->catch_up);
	jw_key(w, "enabled");
	jw_bool(w, s->enabled);
	jw_key(w, "last_run_at");
	jw_num(w, (double)s->last_run_at);
	jw_key(w, "last_ok");
	jw_bool(w, s->last_ok);
	jw_key(w, "last_reason");
	jw_str(w, s->last_reason);
	jw_obj_close(w);
}

void scheduler_write_json_one(const struct schedule *s, struct json_writer *w)
{
	char described[SCHEDULE_DESCRIBE_MAX];
	long now = (long)time(NULL);
	long next = schedule_next_run(s, now);

	schedule_describe(s, described, sizeof(described));
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, s->name);
	jw_key(w, "action");
	jw_str(w, s->action);
	/*
	 * The structured form is what a client edits; `describes` is the
	 * one-way rendering for a CLI column or a log line, and nothing
	 * ever parses it back (ADR-0257).
	 */
	jw_key(w, "schedule");
	jw_obj_open(w);
	if (s->kind == SCHEDULE_EVERY) {
		jw_key(w, "every");
		jw_obj_open(w);
		jw_key(w, "seconds");
		jw_num(w, (double)s->every_seconds);
		jw_obj_close(w);
	} else if (s->kind == SCHEDULE_DAILY) {
		char at[8];

		snprintf(at, sizeof(at), "%02d:%02d", s->hour, s->minute);
		jw_key(w, "daily");
		jw_obj_open(w);
		jw_key(w, "at");
		jw_str(w, at);
		jw_obj_close(w);
	} else {
		char at[8];

		snprintf(at, sizeof(at), "%02d:%02d", s->hour, s->minute);
		jw_key(w, "weekly");
		jw_obj_open(w);
		jw_key(w, "on");
		jw_str(w, WEEKDAYS[s->weekday % 7]);
		jw_key(w, "at");
		jw_str(w, at);
		jw_obj_close(w);
	}
	jw_obj_close(w);
	jw_key(w, "describes");
	jw_str(w, described);
	jw_key(w, "window_minutes");
	jw_num(w, (double)s->window_minutes);
	jw_key(w, "catch_up");
	jw_bool(w, s->catch_up);
	jw_key(w, "enabled");
	jw_bool(w, s->enabled);
	jw_key(w, "next_run_at");
	if (next == 0)
		jw_null(w);
	else
		jw_num(w, (double)next);
	jw_key(w, "last_run_at");
	if (s->last_run_at == 0)
		jw_null(w);
	else
		jw_num(w, (double)s->last_run_at);
	/* Null rather than false before the first run: "it has not run" and
	 * "it ran and failed" are different facts. */
	jw_key(w, "last_ok");
	if (s->last_run_at == 0)
		jw_null(w);
	else
		jw_bool(w, s->last_ok);
	jw_key(w, "last_reason");
	jw_str(w, s->last_reason);
	jw_obj_close(w);
}

void scheduler_write_json(struct json_writer *w)
{
	int i, n = 0;

	jw_obj_open(w);
	jw_key(w, "schedules");
	jw_arr_open(w);
	for (i = 0; i < SCHEDULER_MAX_JOBS; i++) {
		if (!g_jobs[i].in_use)
			continue;
		scheduler_write_json_one(&g_jobs[i], w);
		n++;
	}
	jw_arr_close(w);
	jw_key(w, "total");
	jw_num(w, (double)n);
	jw_obj_close(w);
}

void scheduler_write_actions_json(struct json_writer *w)
{
	size_t i;

	jw_obj_open(w);
	jw_key(w, "actions");
	jw_arr_open(w);
	for (i = 0; i < g_action_count; i++) {
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, g_actions[i].name);
		jw_key(w, "summary");
		jw_str(w, g_actions[i].summary != NULL ? g_actions[i].summary : "");
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}
