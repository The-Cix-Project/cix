#include "pkgpolicy.h"
#include "persist.h"
#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PKGPOLICY_MAX 256

struct policy_entry {
	char name[PKG_NAME_MAX];
	enum pkg_policy_kind kind;
	char version[PKG_VERSION_MAX];
	int in_use;
};

static struct policy_entry g_policies[PKGPOLICY_MAX];
static char g_state_path[512];

const char *pkgpolicy_kind_name(enum pkg_policy_kind kind)
{
	switch (kind) {
	case PKG_POLICY_NEWEST:
		return "newest";
	case PKG_POLICY_PINNED:
		return "pinned";
	case PKG_POLICY_HIGHEST:
	default:
		return "highest";
	}
}

int pkgpolicy_kind_parse(const char *s, enum pkg_policy_kind *out)
{
	if (s == NULL)
		return -1;
	if (strcmp(s, "highest") == 0) {
		*out = PKG_POLICY_HIGHEST;
		return 0;
	}
	if (strcmp(s, "newest") == 0) {
		*out = PKG_POLICY_NEWEST;
		return 0;
	}
	if (strcmp(s, "pinned") == 0) {
		*out = PKG_POLICY_PINNED;
		return 0;
	}
	return -1;
}

static struct policy_entry *find(const char *name)
{
	int i;

	for (i = 0; i < PKGPOLICY_MAX; i++)
		if (g_policies[i].in_use && strcmp(g_policies[i].name, name) == 0)
			return &g_policies[i];
	return NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "policies");
	jw_arr_open(&w);
	for (i = 0; i < PKGPOLICY_MAX; i++) {
		if (!g_policies[i].in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, g_policies[i].name);
		jw_key(&w, "policy");
		jw_str(&w, pkgpolicy_kind_name(g_policies[i].kind));
		jw_key(&w, "version");
		if (g_policies[i].version[0] != '\0')
			jw_str(&w, g_policies[i].version);
		else
			jw_null(&w);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int pkgpolicy_init(const char *path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *arr;
	size_t i;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	memset(g_policies, 0, sizeof(g_policies));

	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return 0; /* never configured -- everything on the default */
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed package policies, ignoring\n", path);
		return 0;
	}
	arr = json_object_get(root, "policies");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		int slot = 0;

		for (i = 0; i < arr->u.array.count && slot < PKGPOLICY_MAX; i++) {
			const char *name = json_as_string(json_object_get(arr->u.array.items[i], "name"));
			const char *policy = json_as_string(json_object_get(arr->u.array.items[i], "policy"));
			const char *version =
			    json_as_string(json_object_get(arr->u.array.items[i], "version"));
			enum pkg_policy_kind kind;

			if (name == NULL || pkgpolicy_kind_parse(policy, &kind) != 0)
				continue;
			snprintf(g_policies[slot].name, sizeof(g_policies[slot].name), "%s", name);
			g_policies[slot].kind = kind;
			snprintf(g_policies[slot].version, sizeof(g_policies[slot].version), "%s",
			         version != NULL ? version : "");
			g_policies[slot].in_use = 1;
			slot++;
		}
	}
	json_free(root);
	return 0;
}

void pkgpolicy_repoint(const char *path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
}

enum pkg_policy_kind pkgpolicy_get(const char *name, char *out_version, size_t out_version_size)
{
	struct policy_entry *p;

	if (out_version != NULL && out_version_size > 0)
		out_version[0] = '\0';
	if (name == NULL)
		return PKG_POLICY_HIGHEST;
	p = find(name);
	if (p == NULL)
		return PKG_POLICY_HIGHEST;
	if (out_version != NULL && out_version_size > 0)
		snprintf(out_version, out_version_size, "%s", p->version);
	return p->kind;
}

int pkgpolicy_set(const char *name, enum pkg_policy_kind kind, const char *version)
{
	struct policy_entry *p;
	int i;

	if (name == NULL || name[0] == '\0')
		return -1;
	/* A pin without a version is not a pin -- it is a policy that would
	 * silently mean "highest" while claiming to hold something. */
	if (kind == PKG_POLICY_PINNED && (version == NULL || version[0] == '\0'))
		return -1;

	p = find(name);
	if (p == NULL) {
		for (i = 0; i < PKGPOLICY_MAX; i++) {
			if (!g_policies[i].in_use) {
				p = &g_policies[i];
				break;
			}
		}
		if (p == NULL)
			return -1;
		snprintf(p->name, sizeof(p->name), "%s", name);
		p->in_use = 1;
	}
	p->kind = kind;
	snprintf(p->version, sizeof(p->version), "%s",
	         (kind == PKG_POLICY_PINNED && version != NULL) ? version : "");
	return save_state();
}

int pkgpolicy_clear(const char *name)
{
	struct policy_entry *p = name != NULL ? find(name) : NULL;

	if (p == NULL)
		return 0; /* already the default -- not an error */
	memset(p, 0, sizeof(*p));
	return save_state();
}

void pkgpolicy_write_json(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < PKGPOLICY_MAX; i++) {
		if (!g_policies[i].in_use)
			continue;
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, g_policies[i].name);
		jw_key(w, "policy");
		jw_str(w, pkgpolicy_kind_name(g_policies[i].kind));
		jw_key(w, "version");
		if (g_policies[i].version[0] != '\0')
			jw_str(w, g_policies[i].version);
		else
			jw_null(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}
