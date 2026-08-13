#include "devicemap.h"
#include "namecheck.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct devicemap_entry {
	char name[DEVICEMAP_NAME_MAX];
	enum devicemap_kind kind;
	char selector[DEVICEMAP_SELECTOR_MAX];
	int in_use;
};

static char g_state_path[512];
static struct devicemap_entry g_maps[DEVICEMAP_MAX];

static struct devicemap_entry *map_find(const char *name)
{
	int i;

	for (i = 0; i < DEVICEMAP_MAX; i++) {
		if (g_maps[i].in_use && strcmp(g_maps[i].name, name) == 0)
			return &g_maps[i];
	}
	return NULL;
}

static const char *kind_str(enum devicemap_kind kind)
{
	return kind == DEVICEMAP_EXACT ? "exact" : "vendor_model";
}

/* Splits "<vendor>:<product>" into two non-empty parts. Returns 0 on
 * success, -1 if selector isn't exactly that shape. */
static int split_vendor_model(const char *selector, char *vendor, size_t vendor_size,
                               char *product, size_t product_size)
{
	const char *colon = strchr(selector, ':');
	size_t vlen;

	if (colon == NULL || colon == selector || colon[1] == '\0')
		return -1;
	if (strchr(colon + 1, ':') != NULL)
		return -1; /* more than one ':' -- not this shape */
	vlen = (size_t)(colon - selector);
	if (vlen >= vendor_size || strlen(colon + 1) >= product_size)
		return -1;
	memcpy(vendor, selector, vlen);
	vendor[vlen] = '\0';
	snprintf(product, product_size, "%s", colon + 1);
	return 0;
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
		jw_str(&w, kind_str(g_maps[i].kind));
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
	if (root->u.array.count > DEVICEMAP_MAX) {
		json_free(root);
		fprintf(stderr, "%s: more mappings persisted than DEVICEMAP_MAX\n", g_state_path);
		return -1;
	}

	for (i = 0; i < root->u.array.count; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *name = json_as_string(json_object_get(item, "name"));
		const char *kind = json_as_string(json_object_get(item, "kind"));
		const char *selector = json_as_string(json_object_get(item, "selector"));

		if (!simple_name_is_valid(name, DEVICEMAP_NAME_MAX) || kind == NULL ||
		    selector == NULL || strlen(selector) >= DEVICEMAP_SELECTOR_MAX ||
		    (strcmp(kind, "exact") != 0 && strcmp(kind, "vendor_model") != 0)) {
			json_free(root);
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			return -1;
		}

		memset(&g_maps[count], 0, sizeof(g_maps[count]));
		snprintf(g_maps[count].name, sizeof(g_maps[count].name), "%s", name);
		g_maps[count].kind = strcmp(kind, "exact") == 0 ? DEVICEMAP_EXACT : DEVICEMAP_VENDOR_MODEL;
		snprintf(g_maps[count].selector, sizeof(g_maps[count].selector), "%s", selector);
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

enum devicemap_error devicemap_create(const char *name, const char *kind_str_in,
                                       const char *selector)
{
	enum devicemap_kind kind;
	int slot = -1;
	int i;

	if (!simple_name_is_valid(name, DEVICEMAP_NAME_MAX))
		return DEVICEMAP_ERR_INVALID_NAME;
	if (kind_str_in == NULL)
		return DEVICEMAP_ERR_INVALID_KIND;
	if (strcmp(kind_str_in, "exact") == 0)
		kind = DEVICEMAP_EXACT;
	else if (strcmp(kind_str_in, "vendor_model") == 0)
		kind = DEVICEMAP_VENDOR_MODEL;
	else
		return DEVICEMAP_ERR_INVALID_KIND;

	if (selector == NULL || selector[0] == '\0' || strlen(selector) >= DEVICEMAP_SELECTOR_MAX)
		return DEVICEMAP_ERR_INVALID_SELECTOR;
	if (kind == DEVICEMAP_VENDOR_MODEL) {
		char vendor[16], product[16];

		if (split_vendor_model(selector, vendor, sizeof(vendor), product, sizeof(product)) != 0)
			return DEVICEMAP_ERR_INVALID_SELECTOR;
	}

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
	snprintf(g_maps[slot].selector, sizeof(g_maps[slot].selector), "%s", selector);
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

int devicemap_resolve(const char *name, const char *os_containers_dir,
                       const struct discovered_device *out[], int cap)
{
	static struct discovered_device cache[DEVICE_ENUM_MAX];
	struct devicemap_entry *m = map_find(name);
	int n, i, found = 0;

	if (m == NULL)
		return -1;

	n = device_enumerate(cache, DEVICE_ENUM_MAX, os_containers_dir);

	if (m->kind == DEVICEMAP_EXACT) {
		for (i = 0; i < n && found < cap; i++) {
			if (strcmp(cache[i].id, m->selector) == 0) {
				out[found++] = &cache[i];
				break; /* exact -- at most one match by construction */
			}
		}
		return found;
	}

	{
		char vendor[16], product[16];

		if (split_vendor_model(m->selector, vendor, sizeof(vendor), product, sizeof(product)) != 0)
			return 0; /* can't happen -- validated at create time */
		for (i = 0; i < n && found < cap; i++) {
			if (cache[i].assignable && strcmp(cache[i].vendor_id, vendor) == 0 &&
			    strcmp(cache[i].product_id, product) == 0)
				out[found++] = &cache[i];
		}
	}
	return found;
}

static void write_map_json_one(const struct devicemap_entry *m, const char *os_containers_dir,
                                struct json_writer *w)
{
	const struct discovered_device *matches[DEVICE_ENUM_MAX];
	int n = devicemap_resolve(m->name, os_containers_dir, matches, DEVICE_ENUM_MAX);
	int j;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, m->name);
	jw_key(w, "kind");
	jw_str(w, kind_str(m->kind));
	jw_key(w, "selector");
	jw_str(w, m->selector);
	jw_key(w, "present");
	jw_bool(w, n > 0);
	jw_key(w, "resolved_ids");
	jw_arr_open(w);
	for (j = 0; j < n; j++)
		jw_str(w, matches[j]->id);
	jw_arr_close(w);
	jw_obj_close(w);
}

int devicemap_write_json_one(const char *name, const char *os_containers_dir, struct json_writer *w)
{
	struct devicemap_entry *m = map_find(name);

	if (m == NULL)
		return 0;
	write_map_json_one(m, os_containers_dir, w);
	return 1;
}

void devicemap_write_json_list(struct json_writer *w, const char *os_containers_dir)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < DEVICEMAP_MAX; i++) {
		if (g_maps[i].in_use)
			write_map_json_one(&g_maps[i], os_containers_dir, w);
	}
	jw_arr_close(w);
}
