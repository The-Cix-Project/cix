/*
 * installq.c -- explicit installs waiting for work in flight to end
 * (ADR-0393, #711). See installq.h.
 */
#include "installq.h"
#include "daemonpaths.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct installq_entry g_q[INSTALLQ_MAX];
static int g_n;
static int g_loaded;

static void copy_str(const struct json_value *o, const char *key, char *out, size_t size)
{
	const char *s = json_as_string(json_object_get(o, key));

	snprintf(out, size, "%s", s != NULL ? s : "");
}

static int field_true(const struct json_value *o, const char *key)
{
	const struct json_value *v = json_object_get(o, key);

	return v != NULL && v->type == JSON_BOOL && v->u.boolean;
}

/* Read once, at first use, after the paths are set. */
static void load(void)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;
	const struct json_value *arr;
	size_t i;

	if (g_loaded)
		return;
	g_loaded = 1;
	g_n = 0;
	if (persist_read_file(INSTALL_QUEUE_PATH, &buf, &len) != 0 || buf == NULL)
		return;
	root = json_parse(buf, len);
	free(buf);
	arr = root != NULL ? json_object_get(root, "queued") : NULL;
	for (i = 0; arr != NULL && arr->type == JSON_ARRAY && i < arr->u.array.count &&
	            g_n < INSTALLQ_MAX;
	     i++) {
		const struct json_value *o = arr->u.array.items[i];
		struct installq_entry *e = &g_q[g_n];

		memset(e, 0, sizeof(*e));
		copy_str(o, "name", e->name, sizeof(e->name));
		copy_str(o, "image", e->image, sizeof(e->image));
		if (e->name[0] == '\0' || e->image[0] == '\0')
			continue;
		copy_str(o, "version", e->version, sizeof(e->version));
		copy_str(o, "waiting_for", e->waiting_for, sizeof(e->waiting_for));
		e->upgrade = field_true(o, "upgrade");
		e->keep_on_failure = field_true(o, "keep_on_failure");
		e->queued_at = (long long)json_as_number(json_object_get(o, "queued_at"));
		g_n++;
	}
	json_free(root);
}

void installq_write_json(struct json_writer *w)
{
	int i;

	load();
	jw_obj_open(w);
	jw_key(w, "queued");
	jw_arr_open(w);
	for (i = 0; i < g_n; i++) {
		jw_obj_open(w);
		jw_key(w, "position");
		jw_int(w, i + 1);
		jw_key(w, "name");
		jw_str(w, g_q[i].name);
		jw_key(w, "image");
		jw_str(w, g_q[i].image);
		jw_key(w, "version");
		if (g_q[i].version[0] != '\0')
			jw_str(w, g_q[i].version);
		else
			jw_null(w);
		jw_key(w, "upgrade");
		jw_bool(w, g_q[i].upgrade);
		jw_key(w, "keep_on_failure");
		jw_bool(w, g_q[i].keep_on_failure);
		jw_key(w, "queued_at");
		jw_int(w, g_q[i].queued_at);
		jw_key(w, "waiting_for");
		jw_str(w, g_q[i].waiting_for);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "depth");
	jw_int(w, g_n);
	jw_key(w, "capacity");
	jw_int(w, INSTALLQ_MAX);
	jw_obj_close(w);
}

static int save(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	installq_write_json(&w);
	rc = persist_atomic_write(INSTALL_QUEUE_PATH, w.buf, w.len);
	jw_free(&w);
	return rc == 0 ? 0 : -1;
}

static int find(const char *name, const char *image)
{
	int i;

	for (i = 0; i < g_n; i++)
		if (strcmp(g_q[i].name, name) == 0 && strcmp(g_q[i].image, image) == 0)
			return i;
	return -1;
}

int installq_add(const struct installq_entry *e, int *existing)
{
	int at;

	load();
	at = find(e->name, e->image);
	if (at >= 0) {
		if (existing != NULL)
			*existing = at + 1;
		return -2;
	}
	if (g_n >= INSTALLQ_MAX)
		return -1;
	g_q[g_n] = *e;
	g_n++;
	if (save() != 0) {
		g_n--;
		return -1;
	}
	return g_n;
}

int installq_count(void)
{
	load();
	return g_n;
}

const struct installq_entry *installq_at(int i)
{
	load();
	return i >= 0 && i < g_n ? &g_q[i] : NULL;
}

int installq_remove(const char *name, const char *image)
{
	int at;

	load();
	at = find(name, image);
	if (at < 0)
		return -1;
	memmove(&g_q[at], &g_q[at + 1], (size_t)(g_n - at - 1) * sizeof(g_q[0]));
	g_n--;
	save();
	return 0;
}
