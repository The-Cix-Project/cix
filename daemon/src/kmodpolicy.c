#include "kmodpolicy.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ADR-0348: the module-load policy and its verdict. See kmodpolicy.h
 * for the contract; this file holds the one decision and the one
 * persisted copy of what it decides from.
 */

static char g_state_path[512];
static struct kmod_policy g_policy;
static int g_saved;

static const char *const g_autoload_names[] = {
	[KMOD_AUTOLOAD_OFF] = "off",
	[KMOD_AUTOLOAD_COLDPLUG] = "coldplug",
};

int kmod_names_equal(const char *a, const char *b)
{
	size_t i;

	for (i = 0;; i++) {
		char ca = a[i] == '-' ? '_' : a[i];
		char cb = b[i] == '-' ? '_' : b[i];

		if (ca != cb)
			return 0;
		if (ca == '\0')
			return 1;
	}
}

static int list_has(const char list[][KMOD_NAME_MAX], int count, const char *name)
{
	int i;

	for (i = 0; i < count; i++) {
		if (kmod_names_equal(list[i], name))
			return 1;
	}
	return 0;
}

int kmod_policy_blacklists(const struct kmod_policy *p, const char *name)
{
	return list_has(p->blacklist, p->blacklist_count, name);
}

static int origin_is_automatic(enum kmod_origin origin)
{
	return origin == KMOD_ORIGIN_COLDPLUG || origin == KMOD_ORIGIN_HOTPLUG;
}

enum kmod_verdict kmod_policy_verdict(const struct kmod_policy *p, const char *name,
                                      const char deps[][KMOD_NAME_MAX], int dep_count,
                                      enum kmod_origin origin, char *why, size_t why_size)
{
	int i;

	if (why_size > 0)
		why[0] = '\0';
	/* The owner's rule of 2026-08-27: the blacklist beats everything,
	 * whoever asks. */
	if (kmod_policy_blacklists(p, name)) {
		snprintf(why, why_size, "%s is blacklisted (kmod-policy)", name);
		return KMOD_REFUSE_BLACKLISTED;
	}
	/* modprobe loads what a module depends on, so a blacklisted
	 * dependency would load by the back door. */
	for (i = 0; i < dep_count; i++) {
		if (kmod_policy_blacklists(p, deps[i])) {
			snprintf(why, why_size, "%s depends on %s, which is blacklisted (kmod-policy)",
			         name, deps[i]);
			return KMOD_REFUSE_DEPENDENCY;
		}
	}
	if (origin_is_automatic(origin) && p->allowlist_count > 0 &&
	    !list_has(p->allowlist, p->allowlist_count, name)) {
		snprintf(why, why_size,
		         "%s is not on the allowlist, which binds automatic loads (kmod-policy)", name);
		return KMOD_REFUSE_NOT_ALLOWLISTED;
	}
	return KMOD_ALLOW;
}

int kmod_policy_autoload_covers(const struct kmod_policy *p, enum kmod_origin origin)
{
	switch (origin) {
	case KMOD_ORIGIN_COLDPLUG:
		return p->autoload == KMOD_AUTOLOAD_COLDPLUG;
	case KMOD_ORIGIN_HOTPLUG:
		return 0; /* no hotplug path exists yet */
	default:
		return 1;
	}
}

const char *kmod_autoload_name(enum kmod_autoload a)
{
	if ((int)a < 0 || (size_t)a >= sizeof(g_autoload_names) / sizeof(g_autoload_names[0]))
		return "off";
	return g_autoload_names[a];
}

static int autoload_from_name(const char *s, enum kmod_autoload *out)
{
	size_t i;

	if (s == NULL)
		return -1;
	for (i = 0; i < sizeof(g_autoload_names) / sizeof(g_autoload_names[0]); i++) {
		if (strcmp(s, g_autoload_names[i]) == 0) {
			*out = (enum kmod_autoload)i;
			return 0;
		}
	}
	return -1;
}

static void policy_default(struct kmod_policy *p)
{
	memset(p, 0, sizeof(*p));
	p->autoload = KMOD_AUTOLOAD_COLDPLUG;
}

/* A list of module names into list/count. -1 with err set on anything
 * that is not an array of distinct valid names. */
static int list_from_json(const struct json_value *arr, const char *field,
                          char list[][KMOD_NAME_MAX], int *count, char *err, size_t err_size)
{
	size_t i;

	if (arr->type != JSON_ARRAY) {
		snprintf(err, err_size, "%s must be an array of module names", field);
		return -1;
	}
	if (arr->u.array.count > KMODPOLICY_LIST_MAX) {
		snprintf(err, err_size, "%s holds at most %d names", field, KMODPOLICY_LIST_MAX);
		return -1;
	}
	*count = 0;
	for (i = 0; i < arr->u.array.count; i++) {
		const char *name = json_as_string(arr->u.array.items[i]);

		if (!kmod_name_is_valid(name)) {
			snprintf(err, err_size, "%s[%zu] is not a valid module name", field, i);
			return -1;
		}
		if (list_has(list, *count, name)) {
			snprintf(err, err_size, "%s names %s twice", field, name);
			return -1;
		}
		snprintf(list[*count], KMOD_NAME_MAX, "%s", name);
		(*count)++;
	}
	return 0;
}

int kmodpolicy_apply_json(const struct json_value *body, struct kmod_policy *inout, char *err,
                          size_t err_size)
{
	const struct json_value *j;

	if (body == NULL || body->type != JSON_OBJECT) {
		snprintf(err, err_size, "the body must be a JSON object");
		return -1;
	}
	j = json_object_get(body, "autoload");
	if (j != NULL && autoload_from_name(json_as_string(j), &inout->autoload) != 0) {
		snprintf(err, err_size, "autoload must be off or coldplug");
		return -1;
	}
	j = json_object_get(body, "blacklist");
	if (j != NULL && list_from_json(j, "blacklist", inout->blacklist, &inout->blacklist_count,
	                                err, err_size) != 0)
		return -1;
	j = json_object_get(body, "allowlist");
	if (j != NULL && list_from_json(j, "allowlist", inout->allowlist, &inout->allowlist_count,
	                                err, err_size) != 0)
		return -1;
	return 0;
}

static void write_list(struct json_writer *w, const char list[][KMOD_NAME_MAX], int count)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < count; i++)
		jw_str(w, list[i]);
	jw_arr_close(w);
}

static void write_policy_fields(struct json_writer *w, const struct kmod_policy *p)
{
	jw_key(w, "autoload");
	jw_str(w, kmod_autoload_name(p->autoload));
	jw_key(w, "blacklist");
	write_list(w, p->blacklist, p->blacklist_count);
	jw_key(w, "allowlist");
	write_list(w, p->allowlist, p->allowlist_count);
}

int kmodpolicy_save(const struct kmod_policy *p)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	write_policy_fields(&w, p);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	if (rc != 0)
		return -1;
	g_policy = *p;
	g_saved = 1;
	return 0;
}

int kmodpolicy_init(const char *state_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	struct kmod_policy loaded;
	char err[160];

	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >= (int)sizeof(g_state_path))
		return -1;
	policy_default(&g_policy);
	g_saved = 0;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* never saved: the default stands */

	root = json_parse(buf, len);
	free(buf);
	policy_default(&loaded);
	if (root == NULL || kmodpolicy_apply_json(root, &loaded, err, sizeof(err)) != 0) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted kmod policy\n", g_state_path);
		return -1;
	}
	json_free(root);
	g_policy = loaded;
	g_saved = 1;
	return 0;
}

const struct kmod_policy *kmodpolicy_current(void)
{
	return &g_policy;
}

int kmodpolicy_saved(void)
{
	return g_saved;
}

void kmodpolicy_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	write_policy_fields(w, &g_policy);
	jw_key(w, "saved");
	jw_bool(w, g_saved);
	jw_obj_close(w);
}
