#include "connthrottle.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct throttle_entry {
	char ip[CONNTHROTTLE_IP_MAX];
	int in_use;
	int fail_count;
	time_t window_start;
	time_t blocked_until; /* 0 = not currently blocked */
	time_t last_logged;   /* 0 = never logged yet -- connthrottle_should_log_failure() */
};

static char g_config_path[512];
static struct throttle_config g_config;
static struct throttle_entry g_table[CONNTHROTTLE_MAX_ENTRIES];

static int save_config(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "enabled");
	jw_bool(&w, g_config.enabled);
	jw_key(&w, "threshold");
	jw_int(&w, g_config.threshold);
	jw_key(&w, "window_seconds");
	jw_int(&w, g_config.window_seconds);
	jw_key(&w, "block_seconds");
	jw_int(&w, g_config.block_seconds);
	jw_key(&w, "log_interval_seconds");
	jw_int(&w, g_config.log_interval_seconds);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_config_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_config(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *v;

	if (persist_read_file(g_config_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* no persisted config yet -- defaults already set by caller */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed persisted connthrottle config\n", g_config_path);
		return -1;
	}

	v = json_object_get(root, "enabled");
	if (v != NULL && v->type == JSON_BOOL)
		g_config.enabled = v->u.boolean;
	v = json_object_get(root, "threshold");
	if (v != NULL)
		g_config.threshold = (int)json_as_number(v);
	v = json_object_get(root, "window_seconds");
	if (v != NULL)
		g_config.window_seconds = (int)json_as_number(v);
	v = json_object_get(root, "block_seconds");
	if (v != NULL)
		g_config.block_seconds = (int)json_as_number(v);
	v = json_object_get(root, "log_interval_seconds");
	if (v != NULL)
		g_config.log_interval_seconds = (int)json_as_number(v);

	json_free(root);
	return 0;
}

int connthrottle_config_init(const char *path)
{
	if (snprintf(g_config_path, sizeof(g_config_path), "%s", path) >= (int)sizeof(g_config_path))
		return -1;

	g_config.enabled = 1;
	g_config.threshold = 20;
	g_config.window_seconds = 60;
	g_config.block_seconds = 300;
	g_config.log_interval_seconds = 5;

	memset(g_table, 0, sizeof(g_table));

	return load_config();
}

struct throttle_config connthrottle_config_get(void)
{
	return g_config;
}

int connthrottle_config_set(int enabled_flag, int threshold, int window_seconds, int block_seconds,
                             int log_interval_seconds)
{
	if (threshold != -1 && (threshold < CONNTHROTTLE_THRESHOLD_MIN || threshold > CONNTHROTTLE_THRESHOLD_MAX))
		return -1;
	if (window_seconds != -1 && (window_seconds < CONNTHROTTLE_WINDOW_MIN || window_seconds > CONNTHROTTLE_WINDOW_MAX))
		return -1;
	if (block_seconds != -1 && (block_seconds < CONNTHROTTLE_BLOCK_MIN || block_seconds > CONNTHROTTLE_BLOCK_MAX))
		return -1;
	if (log_interval_seconds != -1 &&
	    (log_interval_seconds < CONNTHROTTLE_LOG_INTERVAL_MIN || log_interval_seconds > CONNTHROTTLE_LOG_INTERVAL_MAX))
		return -1;

	if (enabled_flag != -1)
		g_config.enabled = enabled_flag;
	if (threshold != -1)
		g_config.threshold = threshold;
	if (window_seconds != -1)
		g_config.window_seconds = window_seconds;
	if (block_seconds != -1)
		g_config.block_seconds = block_seconds;
	if (log_interval_seconds != -1)
		g_config.log_interval_seconds = log_interval_seconds;

	return save_config();
}

static struct throttle_entry *find_entry(const char *ip)
{
	int i;

	for (i = 0; i < CONNTHROTTLE_MAX_ENTRIES; i++) {
		if (g_table[i].in_use && strcmp(g_table[i].ip, ip) == 0)
			return &g_table[i];
	}
	return NULL;
}

static struct throttle_entry *find_free_slot(void)
{
	int i;

	for (i = 0; i < CONNTHROTTLE_MAX_ENTRIES; i++) {
		if (!g_table[i].in_use)
			return &g_table[i];
	}
	return NULL;
}

/* Shared by connthrottle_record_failure() and connthrottle_should_log_
 * failure() -- both key off the same per-IP table, deliberately (one
 * source of truth for "have we seen this IP before," not two). */
static struct throttle_entry *get_or_create_entry(const char *ip, time_t now)
{
	struct throttle_entry *e = find_entry(ip);

	if (e != NULL)
		return e;
	e = find_free_slot();
	if (e == NULL)
		return NULL; /* table full -- see connthrottle.h's own note on this */
	snprintf(e->ip, sizeof(e->ip), "%s", ip);
	e->in_use = 1;
	e->fail_count = 0;
	e->window_start = now;
	e->blocked_until = 0;
	e->last_logged = 0;
	return e;
}

/*
 * Loopback is never throttled, deliberately: kanxeoctl's own default
 * --host= is 127.0.0.1 (a real operator's local CLI use, or this
 * daemon's own test suite, both connect from exactly there), and a
 * block applies uniformly across both listeners -- tripping it from
 * loopback would lock out the daemon's own local admin access
 * entirely, the same class of hazard as a firewall rule that can shut
 * out its own operator. A hostile source is, by definition, never
 * loopback.
 */
static int is_loopback(const char *ip)
{
	return strcmp(ip, "127.0.0.1") == 0;
}

int connthrottle_should_block(const char *ip)
{
	struct throttle_entry *e;

	if (!g_config.enabled || is_loopback(ip))
		return 0;

	e = find_entry(ip);
	if (e == NULL || e->blocked_until == 0)
		return 0;

	if (time(NULL) >= e->blocked_until) {
		/* Block window elapsed -- clear it and start this IP fresh,
		 * rather than leaving a stale blocked_until that every
		 * future should_block() call would need to re-check against
		 * "now" anyway. */
		e->blocked_until = 0;
		e->fail_count = 0;
		return 0;
	}
	return 1;
}

void connthrottle_record_failure(const char *ip)
{
	struct throttle_entry *e;
	time_t now;

	if (!g_config.enabled)
		return;

	now = time(NULL);
	e = get_or_create_entry(ip, now);
	if (e == NULL)
		return;

	/* Outside the rolling window -- start counting fresh rather than
	 * accumulating failures across unrelated incidents. */
	if (now - e->window_start > g_config.window_seconds) {
		e->window_start = now;
		e->fail_count = 0;
	}

	e->fail_count++;
	if (e->fail_count >= g_config.threshold)
		e->blocked_until = now + g_config.block_seconds;
}

int connthrottle_should_log_failure(const char *ip)
{
	time_t now = time(NULL);
	struct throttle_entry *e = get_or_create_entry(ip, now);

	if (e == NULL)
		return 1; /* table full -- fail open, see this function's own doc comment */
	if (g_config.log_interval_seconds > 0 && e->last_logged != 0 &&
	    now - e->last_logged < g_config.log_interval_seconds)
		return 0;
	e->last_logged = now;
	return 1;
}

void connthrottle_record_success(const char *ip)
{
	struct throttle_entry *e = find_entry(ip);

	if (e == NULL)
		return;
	e->fail_count = 0;
	e->blocked_until = 0;
}

void connthrottle_write_status_json(struct json_writer *w)
{
	int i;
	time_t now = time(NULL);

	jw_arr_open(w);
	for (i = 0; i < CONNTHROTTLE_MAX_ENTRIES; i++) {
		struct throttle_entry *e = &g_table[i];
		int blocked;

		if (!e->in_use)
			continue;

		blocked = (e->blocked_until != 0 && now < e->blocked_until);

		jw_obj_open(w);
		jw_key(w, "ip");
		jw_str(w, e->ip);
		jw_key(w, "fail_count");
		jw_int(w, e->fail_count);
		jw_key(w, "blocked");
		jw_bool(w, blocked);
		jw_key(w, "blocked_until");
		jw_int(w, blocked ? (long long)e->blocked_until : 0);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}
