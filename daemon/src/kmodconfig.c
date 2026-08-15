#include "kmodconfig.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kmodconfig_entry {
	char name[KMOD_NAME_MAX];
	char options[KMOD_OPTIONS_MAX];
	int autoload;
	int in_use;
};

static char g_state_path[512];
static struct kmodconfig_entry g_entries[KMODCONFIG_MAX];

static struct kmodconfig_entry *entry_find(const char *name)
{
	int i;

	for (i = 0; i < KMODCONFIG_MAX; i++) {
		if (g_entries[i].in_use && strcmp(g_entries[i].name, name) == 0)
			return &g_entries[i];
	}
	return NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int rc, i;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < KMODCONFIG_MAX; i++) {
		if (!g_entries[i].in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, g_entries[i].name);
		jw_key(&w, "options");
		jw_str(&w, g_entries[i].options);
		jw_key(&w, "autoload");
		jw_bool(&w, g_entries[i].autoload);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;
	int count = 0;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted kmod config\n", g_state_path);
		return -1;
	}
	if (root->u.array.count > KMODCONFIG_MAX) {
		json_free(root);
		fprintf(stderr, "%s: more entries persisted than KMODCONFIG_MAX\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *name = json_as_string(json_object_get(item, "name"));
		const char *options = json_as_string(json_object_get(item, "options"));
		const struct json_value *jautoload = json_object_get(item, "autoload");

		if (!kmod_name_is_valid(name) || options == NULL || strlen(options) >= KMOD_OPTIONS_MAX ||
		    jautoload == NULL || jautoload->type != JSON_BOOL) {
			json_free(root);
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			return -1;
		}

		memset(&g_entries[count], 0, sizeof(g_entries[count]));
		snprintf(g_entries[count].name, sizeof(g_entries[count].name), "%s", name);
		snprintf(g_entries[count].options, sizeof(g_entries[count].options), "%s", options);
		g_entries[count].autoload = jautoload->u.boolean;
		g_entries[count].in_use = 1;
		count++;
	}
	json_free(root);
	return 0;
}

int kmodconfig_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	memset(g_entries, 0, sizeof(g_entries));
	return load_state();
}

void kmodconfig_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

enum kmodconfig_error kmodconfig_set(const char *name, const char *options, int has_autoload,
                                      int autoload_value)
{
	struct kmodconfig_entry *e;
	int slot = -1;
	int i;

	if (!kmod_name_is_valid(name))
		return KMODCONFIG_ERR_INVALID_NAME;
	if (options != NULL && strlen(options) >= KMOD_OPTIONS_MAX)
		return KMODCONFIG_ERR_INVALID_NAME;

	e = entry_find(name);
	if (e == NULL) {
		for (i = 0; i < KMODCONFIG_MAX; i++) {
			if (!g_entries[i].in_use) {
				slot = i;
				break;
			}
		}
		if (slot < 0)
			return KMODCONFIG_ERR_FULL;
		e = &g_entries[slot];
		memset(e, 0, sizeof(*e));
		snprintf(e->name, sizeof(e->name), "%s", name);
		e->in_use = 1;
	}
	if (options != NULL)
		snprintf(e->options, sizeof(e->options), "%s", options);
	if (has_autoload)
		e->autoload = autoload_value ? 1 : 0;

	if (save_state() != 0)
		return KMODCONFIG_ERR_PERSIST_FAILED;
	return KMODCONFIG_OK;
}

enum kmodconfig_error kmodconfig_delete(const char *name)
{
	struct kmodconfig_entry *e = entry_find(name);

	if (e == NULL)
		return KMODCONFIG_ERR_NOT_FOUND;
	memset(e, 0, sizeof(*e));
	if (save_state() != 0)
		return KMODCONFIG_ERR_PERSIST_FAILED;
	return KMODCONFIG_OK;
}

const char *kmodconfig_get_options(const char *name)
{
	struct kmodconfig_entry *e = entry_find(name);

	return e == NULL ? NULL : e->options;
}

int kmodconfig_get_autoload(const char *name)
{
	struct kmodconfig_entry *e = entry_find(name);

	return e == NULL ? 0 : e->autoload;
}

static void write_one(const struct kmodconfig_entry *e, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, e->name);
	jw_key(w, "default_options");
	kmod_options_write_json(e->options, w);
	jw_key(w, "autoload");
	jw_bool(w, e->autoload);
	jw_obj_close(w);
}

int kmodconfig_write_json_one(const char *name, struct json_writer *w)
{
	struct kmodconfig_entry *e = entry_find(name);

	if (e == NULL)
		return 0;
	write_one(e, w);
	return 1;
}

void kmodconfig_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < KMODCONFIG_MAX; i++) {
		if (g_entries[i].in_use)
			write_one(&g_entries[i], w);
	}
	jw_arr_close(w);
}

void kmodconfig_foreach_autoload(void (*fn)(const char *name, const char *options, void *ctx),
                                  void *ctx)
{
	int i;

	for (i = 0; i < KMODCONFIG_MAX; i++) {
		if (g_entries[i].in_use && g_entries[i].autoload)
			fn(g_entries[i].name, g_entries[i].options, ctx);
	}
}
