#include "pkgbad.h"

#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_path[PATH_MAX];
static struct pkgbad_entry g_marks[PKGBAD_MAX];
static int g_count;

static void write_entry(struct json_writer *w, const struct pkgbad_entry *e)
{
	jw_obj_open(w);
	jw_key(w, "package");
	jw_str(w, e->package);
	jw_key(w, "version");
	jw_str(w, e->version);
	jw_key(w, "image");
	jw_str(w, e->image);
	jw_key(w, "container");
	jw_str(w, e->container);
	jw_key(w, "from");
	jw_str(w, e->from);
	jw_key(w, "to");
	jw_str(w, e->to);
	jw_key(w, "reason");
	jw_str(w, e->reason);
	jw_key(w, "at");
	jw_int(w, e->at);
	jw_obj_close(w);
}

void pkgbad_write_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "bad_versions");
	jw_arr_open(w);
	for (i = 0; i < g_count; i++)
		write_entry(w, &g_marks[i]);
	jw_arr_close(w);
	jw_obj_close(w);
}

static int save(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	pkgbad_write_json(&w);
	rc = persist_atomic_write(g_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int find(const char *package, const char *version)
{
	int i;

	if (package == NULL || version == NULL)
		return -1;
	for (i = 0; i < g_count; i++)
		if (strcmp(g_marks[i].package, package) == 0 && strcmp(g_marks[i].version, version) == 0)
			return i;
	return -1;
}

const struct pkgbad_entry *pkgbad_find(const char *package, const char *version)
{
	int i = find(package, version);

	return i >= 0 ? &g_marks[i] : NULL;
}

int pkgbad_is_bad(const char *package, const char *version)
{
	return find(package, version) >= 0;
}

int pkgbad_add(const struct pkgbad_entry *e)
{
	int i;

	if (e == NULL || e->package[0] == '\0' || e->version[0] == '\0')
		return -1;
	i = find(e->package, e->version);
	if (i < 0) {
		if (g_count >= PKGBAD_MAX) {
			/* Full: the oldest mark gives way, never the newest failure. */
			memmove(&g_marks[0], &g_marks[1], sizeof(g_marks[0]) * (PKGBAD_MAX - 1));
			g_count--;
		}
		i = g_count++;
	}
	g_marks[i] = *e;
	return save();
}

static void remove_at(int i)
{
	memmove(&g_marks[i], &g_marks[i + 1], sizeof(g_marks[0]) * (size_t)(g_count - i - 1));
	g_count--;
}

int pkgbad_remove(const char *package, const char *version)
{
	int i = find(package, version);

	if (i < 0)
		return 1;
	remove_at(i);
	return save() == 0 ? 0 : -1;
}

int pkgbad_prune(int (*superseded)(const char *package, const char *version))
{
	int i, removed = 0;

	if (superseded == NULL)
		return 0;
	for (i = 0; i < g_count;) {
		if (superseded(g_marks[i].package, g_marks[i].version)) {
			remove_at(i);
			removed++;
		} else {
			i++;
		}
	}
	if (removed > 0)
		save();
	return removed;
}

int pkgbad_count(void)
{
	return g_count;
}

const struct pkgbad_entry *pkgbad_at(int i)
{
	return i >= 0 && i < g_count ? &g_marks[i] : NULL;
}

static void copy_field(const struct json_value *o, const char *key, char *out, size_t out_size)
{
	const char *s = json_as_string(json_object_get(o, key));

	snprintf(out, out_size, "%s", s != NULL ? s : "");
}

int pkgbad_init(const char *path)
{
	char *buf = NULL;
	size_t len = 0, i;
	struct json_value *root;
	const struct json_value *arr;

	if (path == NULL || (size_t)snprintf(g_path, sizeof(g_path), "%s", path) >= sizeof(g_path))
		return -1;
	g_count = 0;
	if (persist_read_file(g_path, &buf, &len) != 0 || buf == NULL)
		return 0;
	root = json_parse(buf, len);
	free(buf);
	arr = root != NULL ? json_object_get(root, "bad_versions") : NULL;
	for (i = 0; arr != NULL && arr->type == JSON_ARRAY && i < arr->u.array.count &&
	            g_count < PKGBAD_MAX;
	     i++) {
		const struct json_value *o = arr->u.array.items[i];
		struct pkgbad_entry *e = &g_marks[g_count];

		memset(e, 0, sizeof(*e));
		copy_field(o, "package", e->package, sizeof(e->package));
		copy_field(o, "version", e->version, sizeof(e->version));
		copy_field(o, "image", e->image, sizeof(e->image));
		copy_field(o, "container", e->container, sizeof(e->container));
		copy_field(o, "from", e->from, sizeof(e->from));
		copy_field(o, "to", e->to, sizeof(e->to));
		copy_field(o, "reason", e->reason, sizeof(e->reason));
		e->at = (long long)json_as_number(json_object_get(o, "at"));
		if (e->package[0] != '\0' && e->version[0] != '\0')
			g_count++;
	}
	json_free(root);
	return 0;
}
