#include "imagepolicy.h"
#include "persist.h"
#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGEPOLICY_MAX 128

struct policy_slot {
	char name[PKG_IMAGE_NAME_MAX];
	struct image_policy p;
	int in_use;
};

static struct policy_slot g_slots[IMAGEPOLICY_MAX];
static char g_state_path[512];

static const struct image_policy g_defaults = {
	IMAGE_POLICY_RECIPE_MANUAL, IMAGE_POLICY_APPLY_DECLARE, IMAGE_POLICY_DOWNGRADE_REFUSE
};

const char *imagepolicy_recipe_name(enum image_policy_recipe v)
{
	return v == IMAGE_POLICY_RECIPE_FOLLOW ? "follow" : "manual";
}

const char *imagepolicy_apply_name(enum image_policy_apply v)
{
	return v == IMAGE_POLICY_APPLY_CONVERGE ? "converge" : "declare";
}

const char *imagepolicy_downgrade_name(enum image_policy_downgrade v)
{
	return v == IMAGE_POLICY_DOWNGRADE_ALLOW ? "allow" : "refuse";
}

int imagepolicy_recipe_parse(const char *s, enum image_policy_recipe *out)
{
	if (s == NULL)
		return -1;
	if (strcmp(s, "manual") == 0)
		*out = IMAGE_POLICY_RECIPE_MANUAL;
	else if (strcmp(s, "follow") == 0)
		*out = IMAGE_POLICY_RECIPE_FOLLOW;
	else
		return -1;
	return 0;
}

int imagepolicy_apply_parse(const char *s, enum image_policy_apply *out)
{
	if (s == NULL)
		return -1;
	if (strcmp(s, "declare") == 0)
		*out = IMAGE_POLICY_APPLY_DECLARE;
	else if (strcmp(s, "converge") == 0)
		*out = IMAGE_POLICY_APPLY_CONVERGE;
	else
		return -1;
	return 0;
}

int imagepolicy_downgrade_parse(const char *s, enum image_policy_downgrade *out)
{
	if (s == NULL)
		return -1;
	if (strcmp(s, "refuse") == 0)
		*out = IMAGE_POLICY_DOWNGRADE_REFUSE;
	else if (strcmp(s, "allow") == 0)
		*out = IMAGE_POLICY_DOWNGRADE_ALLOW;
	else
		return -1;
	return 0;
}

static struct policy_slot *find(const char *name)
{
	int i;

	for (i = 0; i < IMAGEPOLICY_MAX; i++)
		if (g_slots[i].in_use && strcmp(g_slots[i].name, name) == 0)
			return &g_slots[i];
	return NULL;
}

static int is_default(const struct image_policy *p)
{
	return p->recipe == g_defaults.recipe && p->apply == g_defaults.apply &&
	       p->downgrade == g_defaults.downgrade;
}

void imagepolicy_write_json(struct json_writer *w, const char *name, const struct image_policy *p)
{
	jw_obj_open(w);
	jw_key(w, "image");
	jw_str(w, name);
	jw_key(w, "recipe");
	jw_str(w, imagepolicy_recipe_name(p->recipe));
	jw_key(w, "apply");
	jw_str(w, imagepolicy_apply_name(p->apply));
	jw_key(w, "downgrade");
	jw_str(w, imagepolicy_downgrade_name(p->downgrade));
	jw_obj_close(w);
}

static int save_state(void)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "images");
	jw_arr_open(&w);
	for (i = 0; i < IMAGEPOLICY_MAX; i++) {
		if (g_slots[i].in_use)
			imagepolicy_write_json(&w, g_slots[i].name, &g_slots[i].p);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int imagepolicy_init(const char *path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *arr;
	size_t i;
	int slot = 0;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	memset(g_slots, 0, sizeof(g_slots));

	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return 0; /* never configured -- every image on the defaults */
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed image policies, ignoring\n", path);
		return 0;
	}
	arr = json_object_get(root, "images");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		for (i = 0; i < arr->u.array.count && slot < IMAGEPOLICY_MAX; i++) {
			const struct json_value *o = arr->u.array.items[i];
			const char *name = json_as_string(json_object_get(o, "image"));
			struct image_policy p = g_defaults;

			if (name == NULL || strlen(name) >= sizeof(g_slots[slot].name) ||
			    imagepolicy_recipe_parse(json_as_string(json_object_get(o, "recipe")),
			                             &p.recipe) != 0 ||
			    imagepolicy_apply_parse(json_as_string(json_object_get(o, "apply")),
			                            &p.apply) != 0 ||
			    imagepolicy_downgrade_parse(json_as_string(json_object_get(o, "downgrade")),
			                                &p.downgrade) != 0)
				continue;
			snprintf(g_slots[slot].name, sizeof(g_slots[slot].name), "%s", name);
			g_slots[slot].p = p;
			g_slots[slot].in_use = 1;
			slot++;
		}
	}
	json_free(root);
	return 0;
}

void imagepolicy_get(const char *name, struct image_policy *out)
{
	const struct policy_slot *s = find(name);

	*out = s != NULL ? s->p : g_defaults;
}

int imagepolicy_set(const char *name, const struct image_policy *p)
{
	struct policy_slot *s = find(name);
	int i;

	if (strlen(name) >= sizeof(g_slots[0].name))
		return -1;
	if (is_default(p)) {
		if (s == NULL)
			return 0;
		s->in_use = 0;
		return save_state();
	}
	if (s == NULL) {
		for (i = 0; i < IMAGEPOLICY_MAX && s == NULL; i++)
			if (!g_slots[i].in_use)
				s = &g_slots[i];
		if (s == NULL)
			return -1;
		snprintf(s->name, sizeof(s->name), "%s", name);
		s->in_use = 1;
	}
	s->p = *p;
	return save_state();
}
