#include "containerdef.h"
#include "json.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct container_def g_defs[CONTAINERDEF_MAX];
static char g_state_path[PATH_MAX];

struct container_def *containerdef_find(const char *name)
{
	int i;

	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		if (g_defs[i].in_use && strcmp(g_defs[i].name, name) == 0)
			return &g_defs[i];
	}
	return NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int rc, i, j;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		struct container_def *d = &g_defs[i];

		if (!d->in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, d->name);
		jw_key(&w, "depends_on");
		jw_arr_open(&w);
		for (j = 0; j < d->depends_on_count; j++)
			jw_str(&w, d->depends_on[j]);
		jw_arr_close(&w);
		jw_key(&w, "readiness");
		if (d->has_readiness) {
			jw_obj_open(&w);
			jw_key(&w, "tcp_port");
			jw_int(&w, d->readiness_tcp_port);
			jw_key(&w, "timeout_seconds");
			jw_int(&w, d->readiness_timeout_seconds);
			jw_obj_close(&w);
		} else {
			jw_null(&w);
		}
		jw_key(&w, "restart_policy");
		jw_str(&w, d->restart_policy);
		jw_key(&w, "restart_delay_seconds");
		jw_int(&w, d->restart_delay_seconds);
		jw_key(&w, "stopped");
		jw_bool(&w, d->stopped);
		jw_key(&w, "body");
		jw_str(&w, d->body);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int containerdef_add(const char *name, const char *body, size_t body_len,
                      const char depends_on[][REGISTRY_NAME_MAX], int depends_on_count,
                      int has_readiness, int readiness_tcp_port, int readiness_timeout_seconds,
                      const char *restart_policy, int restart_delay_seconds)
{
	struct container_def *d = containerdef_find(name);
	int i;

	if (d == NULL) {
		int slot = -1;

		for (i = 0; i < CONTAINERDEF_MAX; i++) {
			if (!g_defs[i].in_use) {
				slot = i;
				break;
			}
		}
		if (slot < 0)
			return -1;
		d = &g_defs[slot];
	} else {
		free(d->body);
	}

	memset(d->name, 0, sizeof(d->name));
	strncpy(d->name, name, sizeof(d->name) - 1);

	d->body = malloc(body_len + 1);
	if (d->body == NULL)
		return -1;
	memcpy(d->body, body, body_len);
	d->body[body_len] = '\0';
	d->body_len = body_len;

	d->depends_on_count = depends_on_count;
	for (i = 0; i < depends_on_count; i++) {
		memset(d->depends_on[i], 0, sizeof(d->depends_on[i]));
		strncpy(d->depends_on[i], depends_on[i], sizeof(d->depends_on[i]) - 1);
	}

	d->has_readiness = has_readiness;
	d->readiness_tcp_port = readiness_tcp_port;
	d->readiness_timeout_seconds = readiness_timeout_seconds;

	snprintf(d->restart_policy, sizeof(d->restart_policy), "%s", restart_policy);
	d->restart_delay_seconds = restart_delay_seconds;
	d->stopped = 0;              /* a fresh create/redefine is definitionally not stopped */
	d->consecutive_failures = 0; /* ...and not backed off either */

	d->in_use = 1;

	return save_state();
}

int containerdef_remove(const char *name)
{
	struct container_def *d = containerdef_find(name);

	if (d == NULL)
		return 0;
	free(d->body);
	memset(d, 0, sizeof(*d));
	return save_state();
}

int containerdef_set_stopped(const char *name, int stopped)
{
	struct container_def *d = containerdef_find(name);

	if (d == NULL)
		return 0;
	d->stopped = stopped;
	return save_state();
}

/*
 * Recursive DFS, post-order: name's own depends_on (each recursed
 * into first) land in out_order before name itself -- the same shape
 * pkg.c's own resolve_chain() already established for pkg_depends,
 * applied here to depends_on instead. done[] dedupes a name already
 * resolved via another branch (a legitimate diamond, not a cycle);
 * visiting[] (the current DFS path) catches a genuine cycle. Returns
 * 0 if name was resolved (already done, or newly added to out_order),
 * -1 if it couldn't be (unknown definition, or a cycle) -- logged
 * here, not returned as a message, since this runs at boot/
 * reconciliation time with no single HTTP response to attach it to.
 */
static int visit(const char *name, char out_order[][REGISTRY_NAME_MAX], int *count,
                  char visiting[][REGISTRY_NAME_MAX], int *visiting_count,
                  char done[][REGISTRY_NAME_MAX], int *done_count)
{
	struct container_def *def;
	int i;

	for (i = 0; i < *done_count; i++) {
		if (strcmp(done[i], name) == 0)
			return 0;
	}
	for (i = 0; i < *visiting_count; i++) {
		if (strcmp(visiting[i], name) == 0) {
			fprintf(stderr,
			        "containerdef: circular depends_on involving '%s' -- skipped\n", name);
			return -1;
		}
	}

	def = containerdef_find(name);
	if (def == NULL) {
		fprintf(stderr, "containerdef: '%s' depends_on an unknown definition -- skipped\n",
		        name);
		return -1;
	}

	strncpy(visiting[*visiting_count], name, REGISTRY_NAME_MAX - 1);
	visiting[*visiting_count][REGISTRY_NAME_MAX - 1] = '\0';
	(*visiting_count)++;

	for (i = 0; i < def->depends_on_count; i++) {
		if (visit(def->depends_on[i], out_order, count, visiting, visiting_count, done,
		          done_count) != 0) {
			(*visiting_count)--;
			return -1;
		}
	}

	(*visiting_count)--;

	strncpy(done[*done_count], name, REGISTRY_NAME_MAX - 1);
	done[*done_count][REGISTRY_NAME_MAX - 1] = '\0';
	(*done_count)++;

	strncpy(out_order[*count], name, REGISTRY_NAME_MAX - 1);
	out_order[*count][REGISTRY_NAME_MAX - 1] = '\0';
	(*count)++;
	return 0;
}

int containerdef_resolve_order(char out_order[][REGISTRY_NAME_MAX])
{
	char visiting[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	char done[CONTAINERDEF_MAX][REGISTRY_NAME_MAX];
	int visiting_count = 0, done_count = 0, count = 0;
	int i;

	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		if (!g_defs[i].in_use)
			continue;
		/* Return value ignored -- a failure was already logged by
		 * visit() itself, and simply means this definition (and
		 * whichever of its own dependents already recursed into it
		 * this pass) isn't in out_order; the loop just moves on. */
		visit(g_defs[i].name, out_order, &count, visiting, &visiting_count, done, &done_count);
	}
	return count;
}

static void write_stopped_def_json_one(struct container_def *d, struct json_writer *w)
{
	struct json_value *root;
	const char *image;
	int j;

	root = json_parse(d->body, d->body_len);
	image = root != NULL ? json_as_string(json_object_get(root, "image")) : NULL;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, d->name);
	jw_key(w, "image");
	jw_str(w, image != NULL ? image : "");
	jw_key(w, "status");
	jw_str(w, "stopped");
	jw_key(w, "paused");
	jw_bool(w, 0);
	jw_key(w, "pid");
	jw_null(w);
	jw_key(w, "exit_status");
	jw_null(w);
	jw_key(w, "networks");
	jw_arr_open(w);
	jw_arr_close(w);
	jw_key(w, "ip_forward");
	jw_bool(w, 0);
	jw_key(w, "devices");
	jw_arr_open(w);
	jw_arr_close(w);
	jw_key(w, "interfaces");
	jw_arr_open(w);
	jw_arr_close(w);
	jw_key(w, "files");
	jw_arr_open(w);
	jw_arr_close(w);
	jw_key(w, "sysctls");
	jw_obj_open(w);
	jw_obj_close(w);
	jw_key(w, "restart");
	jw_str(w, d->restart_policy);
	jw_key(w, "restart_delay_seconds");
	jw_int(w, d->restart_delay_seconds);
	jw_key(w, "stopped");
	jw_bool(w, 1);
	jw_key(w, "depends_on");
	jw_arr_open(w);
	for (j = 0; j < d->depends_on_count; j++)
		jw_str(w, d->depends_on[j]);
	jw_arr_close(w);
	jw_key(w, "readiness");
	if (d->has_readiness) {
		jw_obj_open(w);
		jw_key(w, "tcp_port");
		jw_int(w, d->readiness_tcp_port);
		jw_key(w, "timeout_seconds");
		jw_int(w, d->readiness_timeout_seconds);
		jw_obj_close(w);
	} else {
		jw_null(w);
	}
	jw_obj_close(w);

	json_free(root);
}

void containerdef_write_json_stopped_list(struct json_writer *w)
{
	int i;

	for (i = 0; i < CONTAINERDEF_MAX; i++) {
		if (g_defs[i].in_use && g_defs[i].stopped)
			write_stopped_def_json_one(&g_defs[i], w);
	}
}

int containerdef_write_json_stopped_one(const char *name, struct json_writer *w)
{
	struct container_def *d = containerdef_find(name);

	if (d == NULL || !d->stopped)
		return 0;
	write_stopped_def_json_one(d, w);
	return 1;
}

static int parse_persisted_entry(const struct json_value *item, struct container_def *slot)
{
	const char *name = json_as_string(json_object_get(item, "name"));
	const char *body = json_as_string(json_object_get(item, "body"));
	const struct json_value *jdeps = json_object_get(item, "depends_on");
	const struct json_value *jready = json_object_get(item, "readiness");
	const struct json_value *jrestart_policy = json_object_get(item, "restart_policy");
	const struct json_value *jrestart_delay = json_object_get(item, "restart_delay_seconds");
	const struct json_value *jstopped = json_object_get(item, "stopped");
	size_t i;

	if (name == NULL || name[0] == '\0' || strlen(name) >= REGISTRY_NAME_MAX || body == NULL)
		return -1;

	memset(slot, 0, sizeof(*slot));
	strncpy(slot->name, name, sizeof(slot->name) - 1);

	slot->body_len = strlen(body);
	slot->body = malloc(slot->body_len + 1);
	if (slot->body == NULL)
		return -1;
	memcpy(slot->body, body, slot->body_len + 1); /* includes the NUL */

	if (jdeps != NULL) {
		if (jdeps->type != JSON_ARRAY) {
			free(slot->body);
			return -1;
		}
		for (i = 0; i < jdeps->u.array.count && slot->depends_on_count < CONTAINERDEF_MAX_DEPENDS;
		     i++) {
			const char *dep = json_as_string(jdeps->u.array.items[i]);

			if (dep == NULL) {
				free(slot->body);
				return -1;
			}
			strncpy(slot->depends_on[slot->depends_on_count], dep,
			        sizeof(slot->depends_on[slot->depends_on_count]) - 1);
			slot->depends_on_count++;
		}
	}

	if (jready != NULL && jready->type == JSON_OBJECT) {
		const struct json_value *jport = json_object_get(jready, "tcp_port");
		const struct json_value *jtimeout = json_object_get(jready, "timeout_seconds");

		if (jport == NULL) {
			free(slot->body);
			return -1;
		}
		slot->has_readiness = 1;
		slot->readiness_tcp_port = (int)json_as_number(jport);
		slot->readiness_timeout_seconds =
		    jtimeout != NULL ? (int)json_as_number(jtimeout) : 30;
	}

	if (jrestart_policy != NULL) {
		const char *rp = json_as_string(jrestart_policy);

		if (rp == NULL || (strcmp(rp, "always") != 0 && strcmp(rp, "on-failure") != 0 &&
		                    strcmp(rp, "unless-stopped") != 0)) {
			/* A value we ourselves wrote must be one of these three --
			 * a present-but-invalid value is real corruption, same
			 * posture as every other field here. */
			free(slot->body);
			return -1;
		}
		snprintf(slot->restart_policy, sizeof(slot->restart_policy), "%s", rp);
	} else {
		/* Backward compat: a file from Phase 13 parts 1-2 predates this
		 * field and only ever encoded restart:"always" implicitly, by
		 * the definition existing at all. */
		snprintf(slot->restart_policy, sizeof(slot->restart_policy), "always");
	}
	slot->restart_delay_seconds = jrestart_delay != NULL
	                                   ? (int)json_as_number(jrestart_delay)
	                                   : CONTAINERDEF_DEFAULT_RESTART_DELAY_SECONDS;
	slot->stopped = (jstopped != NULL && jstopped->type == JSON_BOOL && jstopped->u.boolean);

	slot->in_use = 1;
	return 0;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int rc = 0;
	int count = 0;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted container definitions\n", g_state_path);
		return -1;
	}
	if (root->u.array.count > CONTAINERDEF_MAX) {
		json_free(root);
		fprintf(stderr, "%s: more definitions persisted than CONTAINERDEF_MAX\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		if (parse_persisted_entry(root->u.array.items[i], &g_defs[count]) != 0) {
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			rc = -1;
			break;
		}
		count++;
	}
	json_free(root);
	return rc;
}

int containerdef_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >=
	    (int)sizeof(g_state_path))
		return -1;
	memset(g_defs, 0, sizeof(g_defs));
	return load_state();
}
