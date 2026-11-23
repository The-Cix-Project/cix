/*
 * depsecret.c -- the secrets each deployment was applied with
 * (ADR-0392, #724). See depsecret.h.
 */
#include "depsecret.h"
#include "daemonpaths.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The whole file, or NULL when there is none or it does not parse. */
static struct json_value *load(void)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	if (persist_read_file(DEPLOYMENT_SECRETS_PATH, &buf, &len) != 0 || buf == NULL)
		return NULL;
	root = json_parse(buf, len);
	free(buf);
	if (root != NULL && root->type != JSON_OBJECT) {
		json_free(root);
		return NULL;
	}
	return root;
}

static int is_string(const struct json_value *v)
{
	return v != NULL && v->type == JSON_STRING && v->u.string != NULL;
}

int depsecret_merge(const char *deployment, const struct json_value *given,
                    struct json_writer *w)
{
	struct json_value *root = load();
	const struct json_value *kept = root != NULL ? json_object_get(root, deployment) : NULL;
	size_t i;
	int added = 0;

	jw_obj_open(w);
	if (given != NULL && given->type == JSON_OBJECT) {
		for (i = 0; i < given->u.object.count; i++) {
			if (!is_string(given->u.object.values[i]))
				continue;
			jw_key(w, given->u.object.keys[i]);
			jw_str(w, given->u.object.values[i]->u.string);
		}
	}
	if (kept != NULL && kept->type == JSON_OBJECT) {
		for (i = 0; i < kept->u.object.count; i++) {
			const char *key = kept->u.object.keys[i];

			if (!is_string(kept->u.object.values[i]))
				continue;
			if (given != NULL && given->type == JSON_OBJECT &&
			    is_string(json_object_get(given, key)))
				continue; /* a given value replaces the kept one */
			jw_key(w, key);
			jw_str(w, kept->u.object.values[i]->u.string);
			added++;
		}
	}
	jw_obj_close(w);
	json_free(root);
	return added;
}

/*
 * Rewrites the file with `deployment`'s secrets replaced by `merged`
 * (NULL: removed), every other deployment's carried over unchanged.
 */
static int rewrite(const char *deployment, const struct json_value *merged)
{
	struct json_value *root = load();
	struct json_writer w;
	size_t i;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	if (root != NULL) {
		for (i = 0; i < root->u.object.count; i++) {
			if (strcmp(root->u.object.keys[i], deployment) == 0)
				continue;
			jw_key(&w, root->u.object.keys[i]);
			jw_value(&w, root->u.object.values[i]);
		}
	}
	if (merged != NULL) {
		jw_key(&w, deployment);
		jw_value(&w, merged);
	}
	jw_obj_close(&w);
	json_free(root);
	rc = persist_atomic_write_private(DEPLOYMENT_SECRETS_PATH, w.buf, w.len);
	jw_free(&w);
	return rc == 0 ? 0 : -1;
}

int depsecret_keep(const char *deployment, const struct json_value *given)
{
	struct json_writer w;
	struct json_value *merged;
	int rc;

	if (given == NULL || given->type != JSON_OBJECT || given->u.object.count == 0)
		return 0; /* nothing new to keep */
	jw_init(&w);
	depsecret_merge(deployment, given, &w);
	merged = json_parse(w.buf, w.len);
	jw_free(&w);
	if (merged == NULL)
		return -1;
	rc = rewrite(deployment, merged);
	json_free(merged);
	return rc;
}

int depsecret_forget(const char *deployment)
{
	struct json_value *root = load();
	int had = root != NULL && json_object_get(root, deployment) != NULL;

	json_free(root);
	if (!had)
		return 0;
	return rewrite(deployment, NULL);
}
