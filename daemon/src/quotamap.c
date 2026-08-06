#include "quotamap.h"
#include "json.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Project id 0 is reserved (an untagged inode's fsx_projid reads back
 * as 0 -- see linux/fs.h's own struct fsxattr comment) and must never
 * be handed out as a real assignment, so real allocation starts here.
 */
#define QUOTAMAP_FIRST_ID 1000

struct quotamap_entry {
	char name[REGISTRY_NAME_MAX];
	uint32_t project_id;
	int in_use;
};

static struct quotamap_entry g_entries[QUOTAMAP_MAX];
static uint32_t g_next_id;
static char g_state_path[PATH_MAX];

static int save_state(void)
{
	struct json_writer w;
	int rc, i;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "next_id");
	jw_int(&w, (long long)g_next_id);
	jw_key(&w, "entries");
	jw_arr_open(&w);
	for (i = 0; i < QUOTAMAP_MAX; i++) {
		if (!g_entries[i].in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, g_entries[i].name);
		jw_key(&w, "project_id");
		jw_int(&w, (long long)g_entries[i].project_id);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *jentries, *jnext_id;
	size_t i;
	int count = 0;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL) {
		g_next_id = QUOTAMAP_FIRST_ID;
		return 0;
	}

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_OBJECT) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted quota project-id map\n", g_state_path);
		return -1;
	}

	jnext_id = json_object_get(root, "next_id");
	g_next_id = jnext_id != NULL ? (uint32_t)json_as_number(jnext_id) : QUOTAMAP_FIRST_ID;
	if (g_next_id < QUOTAMAP_FIRST_ID)
		g_next_id = QUOTAMAP_FIRST_ID;

	jentries = json_object_get(root, "entries");
	if (jentries != NULL && jentries->type == JSON_ARRAY) {
		if (jentries->u.array.count > QUOTAMAP_MAX) {
			json_free(root);
			fprintf(stderr, "%s: more entries persisted than QUOTAMAP_MAX\n", g_state_path);
			return -1;
		}
		for (i = 0; i < jentries->u.array.count; i++) {
			const struct json_value *item = jentries->u.array.items[i];
			const char *name = json_as_string(json_object_get(item, "name"));
			const struct json_value *jpid = json_object_get(item, "project_id");

			if (name == NULL || jpid == NULL) {
				json_free(root);
				fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
				return -1;
			}
			snprintf(g_entries[count].name, sizeof(g_entries[count].name), "%s", name);
			g_entries[count].project_id = (uint32_t)json_as_number(jpid);
			g_entries[count].in_use = 1;
			count++;
		}
	}
	json_free(root);
	return 0;
}

int quotamap_init(const char *state_path)
{
	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >=
	    (int)sizeof(g_state_path))
		return -1;
	memset(g_entries, 0, sizeof(g_entries));
	g_next_id = QUOTAMAP_FIRST_ID;
	return load_state();
}

int quotamap_get_or_assign(const char *name, uint32_t *out_projid)
{
	int i, free_slot = -1;

	for (i = 0; i < QUOTAMAP_MAX; i++) {
		if (!g_entries[i].in_use) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (strcmp(g_entries[i].name, name) == 0) {
			*out_projid = g_entries[i].project_id;
			return 0;
		}
	}

	if (free_slot < 0)
		return -1;

	snprintf(g_entries[free_slot].name, sizeof(g_entries[free_slot].name), "%s", name);
	g_entries[free_slot].project_id = g_next_id;
	g_entries[free_slot].in_use = 1;
	g_next_id++;

	if (save_state() != 0) {
		g_entries[free_slot].in_use = 0;
		g_next_id--;
		return -1;
	}

	*out_projid = g_entries[free_slot].project_id;
	return 0;
}
