#include "serverhealth.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct serverhealth_record g_records[SERVERHEALTH_MAX];
static char g_state_path[PATH_MAX];

static struct serverhealth_record *find_slot(const char *kind, const char *container)
{
	int i;

	for (i = 0; i < SERVERHEALTH_MAX; i++) {
		if (g_records[i].in_use && strcmp(g_records[i].kind, kind) == 0 &&
		    strcmp(g_records[i].container, container) == 0)
			return &g_records[i];
	}
	return NULL;
}

static struct serverhealth_record *find_or_create(const char *kind, const char *container)
{
	struct serverhealth_record *r = find_slot(kind, container);
	int i;

	if (r != NULL)
		return r;
	for (i = 0; i < SERVERHEALTH_MAX; i++) {
		if (!g_records[i].in_use) {
			r = &g_records[i];
			memset(r, 0, sizeof(*r));
			snprintf(r->kind, sizeof(r->kind), "%s", kind);
			snprintf(r->container, sizeof(r->container), "%s", container);
			r->state = SERVERHEALTH_UNKNOWN;
			r->in_use = 1;
			return r;
		}
	}
	return NULL; /* table full -- health tracking degrades, nothing breaks */
}

/*
 * Only the drain flags are persisted (see struct serverhealth_record's
 * own comment): a drain is operator intent that must survive a restart,
 * while health itself is a live observation that must not be replayed
 * from disk as if it were still true.
 */
static int save_state(void)
{
	struct json_writer w;
	int rc, i;

	if (g_state_path[0] == '\0')
		return 0;
	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < SERVERHEALTH_MAX; i++) {
		if (!g_records[i].in_use || !g_records[i].drained)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "kind");
		jw_str(&w, g_records[i].kind);
		jw_key(&w, "container");
		jw_str(&w, g_records[i].container);
		jw_key(&w, "drained");
		jw_bool(&w, 1);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int serverhealth_init(const char *state_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;

	memset(g_records, 0, sizeof(g_records));
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return 0; /* unreadable is not fatal -- drains simply start clear */
	if (buf == NULL)
		return 0; /* never written yet */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted server-health state\n", g_state_path);
		return -1;
	}
	for (i = 0; i < root->u.array.count; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *kind = json_as_string(json_object_get(item, "kind"));
		const char *container = json_as_string(json_object_get(item, "container"));
		struct serverhealth_record *r;

		if (kind == NULL || container == NULL || kind[0] == '\0' || container[0] == '\0')
			continue;
		r = find_or_create(kind, container);
		if (r != NULL)
			r->drained = 1;
	}
	json_free(root);
	return 0;
}

void serverhealth_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

void serverhealth_record_result(const char *kind, const char *container, const char *probe, int ok,
                                 const char *err)
{
	struct serverhealth_record *r = find_or_create(kind, container);

	if (r == NULL)
		return;
	if (probe != NULL)
		snprintf(r->probe, sizeof(r->probe), "%s", probe);
	r->last_check_at = time(NULL);
	if (ok) {
		/* Recovery is never delayed or rate-limited: one good probe is
		 * enough to put a server back in service. */
		r->state = SERVERHEALTH_HEALTHY;
		r->consecutive_failures = 0;
		r->last_ok_at = r->last_check_at;
		r->last_error[0] = '\0';
		return;
	}
	r->consecutive_failures++;
	snprintf(r->last_error, sizeof(r->last_error), "%s", err != NULL ? err : "probe failed");
	if (r->consecutive_failures >= SERVERHEALTH_FAILURE_THRESHOLD)
		r->state = SERVERHEALTH_UNHEALTHY;
}

struct serverhealth_record *serverhealth_find(const char *kind, const char *container)
{
	return find_slot(kind, container);
}

int serverhealth_in_service(const char *kind, const char *container)
{
	struct serverhealth_record *r = find_slot(kind, container);

	if (r == NULL)
		return 1; /* not tracked yet -- never withhold a server we know nothing about */
	if (r->drained)
		return 0;
	return r->state != SERVERHEALTH_UNHEALTHY;
}

int serverhealth_set_drained(const char *kind, const char *container, int drained)
{
	struct serverhealth_record *r = find_or_create(kind, container);

	if (r == NULL)
		return -1;
	r->drained = drained ? 1 : 0;
	return save_state();
}

void serverhealth_forget(const char *container)
{
	int i, changed = 0;

	for (i = 0; i < SERVERHEALTH_MAX; i++) {
		if (g_records[i].in_use && strcmp(g_records[i].container, container) == 0) {
			if (g_records[i].drained)
				changed = 1;
			memset(&g_records[i], 0, sizeof(g_records[i]));
		}
	}
	if (changed)
		save_state();
}

static const char *state_name(enum serverhealth_state s)
{
	switch (s) {
	case SERVERHEALTH_HEALTHY: return "healthy";
	case SERVERHEALTH_UNHEALTHY: return "unhealthy";
	default: return "unknown";
	}
}

void serverhealth_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < SERVERHEALTH_MAX; i++) {
		struct serverhealth_record *r = &g_records[i];

		if (!r->in_use)
			continue;
		jw_obj_open(w);
		jw_key(w, "kind");
		jw_str(w, r->kind);
		jw_key(w, "container");
		jw_str(w, r->container);
		jw_key(w, "state");
		jw_str(w, state_name(r->state));
		jw_key(w, "drained");
		jw_bool(w, r->drained);
		jw_key(w, "in_service");
		jw_bool(w, !r->drained && r->state != SERVERHEALTH_UNHEALTHY);
		jw_key(w, "probe");
		if (r->probe[0] != '\0')
			jw_str(w, r->probe);
		else
			jw_null(w);
		jw_key(w, "last_check_at");
		if (r->last_check_at != 0)
			jw_int(w, (long long)r->last_check_at);
		else
			jw_null(w);
		jw_key(w, "last_ok_at");
		if (r->last_ok_at != 0)
			jw_int(w, (long long)r->last_ok_at);
		else
			jw_null(w);
		jw_key(w, "consecutive_failures");
		jw_int(w, r->consecutive_failures);
		jw_key(w, "last_error");
		if (r->last_error[0] != '\0')
			jw_str(w, r->last_error);
		else
			jw_null(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}
