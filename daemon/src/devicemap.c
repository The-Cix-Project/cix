#include "devicemap.h"
#include "disk.h"
#include "logstore.h"
#include "namecheck.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct devicemap_entry {
	char name[DEVICEMAP_NAME_MAX];
	enum devicematch_kind kind;
	char selector[DEVICEMAP_SELECTOR_MAX];
	int in_use;
};

static char g_state_path[512];
static struct devicemap_entry g_maps[DEVICEMAP_MAX];

/* Persisted mappings load_state() left out. It runs before the log
 * store opens, so devicemap_log_skipped() says it afterwards. */
static int g_skipped;

void devicemap_log_skipped(void)
{
	if (g_skipped > 0)
		logstore_write("cixd", "error",
		               "%d device mapping(s) in %s skipped at startup: not one this daemon can "
		               "resolve, such as an exact mapping on a kernel name (ADR-0345)",
		               g_skipped, g_state_path);
}

static struct devicemap_entry *map_find(const char *name)
{
	int i;

	for (i = 0; i < DEVICEMAP_MAX; i++) {
		if (g_maps[i].in_use && strcmp(g_maps[i].name, name) == 0)
			return &g_maps[i];
	}
	return NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int rc, i;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < DEVICEMAP_MAX; i++) {
		if (!g_maps[i].in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, g_maps[i].name);
		jw_key(&w, "kind");
		jw_str(&w, devicematch_kind_word(g_maps[i].kind));
		jw_key(&w, "selector");
		jw_str(&w, g_maps[i].selector);
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
		fprintf(stderr, "%s: malformed persisted device mappings\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count && count < DEVICEMAP_MAX; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *name = json_as_string(json_object_get(item, "name"));
		const char *kind_word = json_as_string(json_object_get(item, "kind"));
		const char *selector = json_as_string(json_object_get(item, "selector"));
		enum devicematch_kind kind;
		char normal[DEVICEMAP_SELECTOR_MAX];
		char err[256];

		/*
		 * Checked by the same rules a create is, with no records: a
		 * mapping saved under older rules (an exact on a disk: id,
		 * which ADR-0345 refuses) is said once and left out, never
		 * allowed to stop the daemon from starting.
		 */
		if (!simple_name_is_valid(name, DEVICEMAP_NAME_MAX) ||
		    devicematch_kind_parse(kind_word, &kind) != 0 ||
		    devicematch_check_selector(kind, selector, NULL, 0, normal, sizeof(normal), err,
		                               sizeof(err)) != 0) {
			g_skipped++;
			continue;
		}

		memset(&g_maps[count], 0, sizeof(g_maps[count]));
		snprintf(g_maps[count].name, sizeof(g_maps[count].name), "%s", name);
		g_maps[count].kind = kind;
		snprintf(g_maps[count].selector, sizeof(g_maps[count].selector), "%s", normal);
		g_maps[count].in_use = 1;
		count++;
	}
	json_free(root);
	return 0;
}

void devicemap_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

int devicemap_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	memset(g_maps, 0, sizeof(g_maps));
	return load_state();
}

enum devicemap_error devicemap_create(const char *name, const char *kind_word, const char *selector,
                                      const struct discovered_device *recs, int n, char *err,
                                      size_t err_size)
{
	enum devicematch_kind kind;
	char normal[DEVICEMAP_SELECTOR_MAX];
	int slot = -1;
	int i;

	if (!simple_name_is_valid(name, DEVICEMAP_NAME_MAX))
		return DEVICEMAP_ERR_INVALID_NAME;
	if (devicematch_kind_parse(kind_word, &kind) != 0)
		return DEVICEMAP_ERR_INVALID_KIND;
	if (devicematch_check_selector(kind, selector, recs, n, normal, sizeof(normal), err,
	                               err_size) != 0)
		return DEVICEMAP_ERR_INVALID_SELECTOR;

	if (map_find(name) != NULL)
		return DEVICEMAP_ERR_DUPLICATE;

	for (i = 0; i < DEVICEMAP_MAX; i++) {
		if (!g_maps[i].in_use) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return DEVICEMAP_ERR_FULL;

	memset(&g_maps[slot], 0, sizeof(g_maps[slot]));
	snprintf(g_maps[slot].name, sizeof(g_maps[slot].name), "%s", name);
	g_maps[slot].kind = kind;
	snprintf(g_maps[slot].selector, sizeof(g_maps[slot].selector), "%s", normal);
	g_maps[slot].in_use = 1;

	if (save_state() != 0) {
		memset(&g_maps[slot], 0, sizeof(g_maps[slot]));
		return DEVICEMAP_ERR_PERSIST_FAILED;
	}
	return DEVICEMAP_OK;
}

enum devicemap_error devicemap_delete(const char *name)
{
	struct devicemap_entry *m = map_find(name);

	if (m == NULL)
		return DEVICEMAP_ERR_NOT_FOUND;
	memset(m, 0, sizeof(*m));
	if (save_state() != 0)
		return DEVICEMAP_ERR_PERSIST_FAILED;
	return DEVICEMAP_OK;
}

/* Every match's index into recs, and how many there are in all. */
static int match_all(const struct devicemap_entry *m, const struct discovered_device *recs, int n,
                     int idx[], int cap)
{
	return devicematch_find(m->kind, m->selector, recs, n, disk_probe_fs_uuid, idx, cap);
}

int devicemap_resolve(const char *name, const struct discovered_device *recs, int n,
                      const struct discovered_device *out[], int cap, char *err,
                      size_t err_size)
{
	static int idx[DEVICE_ENUM_MAX];
	const struct devicemap_entry *m = map_find(name);
	int found, k;

	if (m == NULL)
		return DEVICEMAP_NOT_A_MAPPING;
	found = match_all(m, recs, n, idx, DEVICE_ENUM_MAX);
	if (found > DEVICE_ENUM_MAX)
		found = DEVICE_ENUM_MAX;
	if (found > 1 && devicematch_exclusive(m->kind)) {
		snprintf(err, err_size,
		         "device mapping %s is ambiguous: its %s selector matches %s and %s%s -- name "
		         "one of them by an exact mapping on its port or slot",
		         m->name, devicematch_kind_word(m->kind), recs[idx[0]].id, recs[idx[1]].id,
		         found > 2 ? " and more" : "");
		return DEVICEMAP_AMBIGUOUS;
	}
	for (k = 0; k < found && k < cap; k++)
		out[k] = &recs[idx[k]];
	return k;
}

int devicemap_names_interface(const char *name)
{
	const struct devicemap_entry *m = map_find(name);

	return m != NULL && m->kind == DEVICEMATCH_MAC;
}

static void write_map_json_one(const struct devicemap_entry *m, const struct discovered_device *recs,
                               int n, struct json_writer *w)
{
	static int idx[DEVICE_ENUM_MAX];
	int found = match_all(m, recs, n, idx, DEVICE_ENUM_MAX);
	const char *state;
	int j;

	if (found > DEVICE_ENUM_MAX)
		found = DEVICE_ENUM_MAX;
	if (found == 0)
		state = "absent";
	else if (found > 1 && devicematch_exclusive(m->kind))
		state = "ambiguous";
	else
		state = "present";

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, m->name);
	jw_key(w, "kind");
	jw_str(w, devicematch_kind_word(m->kind));
	jw_key(w, "selector");
	jw_str(w, m->selector);
	jw_key(w, "state");
	jw_str(w, state);
	jw_key(w, "resolved_ids");
	jw_arr_open(w);
	for (j = 0; j < found; j++)
		jw_str(w, recs[idx[j]].id);
	jw_arr_close(w);
	jw_obj_close(w);
}

int devicemap_write_json_one(const char *name, const struct discovered_device *recs, int n,
                             struct json_writer *w)
{
	struct devicemap_entry *m = map_find(name);

	if (m == NULL)
		return 0;
	write_map_json_one(m, recs, n, w);
	return 1;
}

void devicemap_write_json_list(struct json_writer *w, const struct discovered_device *recs, int n)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < DEVICEMAP_MAX; i++) {
		if (g_maps[i].in_use)
			write_map_json_one(&g_maps[i], recs, n, w);
	}
	jw_arr_close(w);
}
