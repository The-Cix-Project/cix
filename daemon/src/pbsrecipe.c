/*
 * Reading a PBS recipe's identity out of `cbs explain --json`
 * (ADR-0305). See pbsrecipe.h for why this is a separate translation
 * unit and why the accessors are getters rather than one struct fill.
 */
#include "pbsrecipe.h"

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pbs_explain {
	struct json_value *root;
};

/* "" rather than NULL for an absent string, so every caller can print
 * the result without a NULL check. json_as_string() already answers
 * NULL for a JSON null, which is what explain emits for an undeclared
 * upstream/toolchain. */
static const char *str_or_empty(const struct json_value *v)
{
	const char *s = json_as_string(v);

	return s != NULL ? s : "";
}

struct pbs_explain *pbs_explain_parse(const char *text, size_t len, char *err, size_t err_size)
{
	struct pbs_explain *ex;
	struct json_value *root;
	const struct json_value *v;

	if (text == NULL || len == 0) {
		snprintf(err, err_size, "explain output is empty");
		return NULL;
	}
	root = json_parse(text, len);
	if (root == NULL) {
		snprintf(err, err_size, "explain output is not valid JSON");
		return NULL;
	}

	/*
	 * Three fields CPDL guarantees: validate_package() requires
	 * exactly one version and exactly one release declaration, and a
	 * package node always has a name. Their absence does not mean a
	 * recipe declared nothing -- it means this JSON is not an explain
	 * document, and saying so is more useful than mapping empty
	 * fields onto a recipe and failing later on a name mismatch.
	 */
	if (json_as_string(json_object_get(root, "name")) == NULL) {
		snprintf(err, err_size, "explain output has no package name");
		json_free(root);
		return NULL;
	}
	if (json_as_string(json_object_get(root, "version")) == NULL) {
		snprintf(err, err_size, "explain output has no version");
		json_free(root);
		return NULL;
	}
	v = json_object_get(root, "release");
	if (v == NULL || v->type != JSON_NUMBER) {
		snprintf(err, err_size, "explain output has no release number");
		json_free(root);
		return NULL;
	}

	ex = calloc(1, sizeof(*ex));
	if (ex == NULL) {
		snprintf(err, err_size, "out of memory reading explain output");
		json_free(root);
		return NULL;
	}
	ex->root = root;
	return ex;
}

void pbs_explain_free(struct pbs_explain *ex)
{
	if (ex == NULL)
		return;
	json_free(ex->root);
	free(ex);
}

const char *pbs_explain_name(const struct pbs_explain *ex)
{
	if (ex == NULL)
		return "";
	return str_or_empty(json_object_get(ex->root, "name"));
}

int pbs_explain_version(const struct pbs_explain *ex, char *out, size_t out_size)
{
	const char *version;
	long long release;
	int n;

	if (ex == NULL || out == NULL || out_size == 0)
		return -1;
	version = str_or_empty(json_object_get(ex->root, "version"));
	release = (long long)json_as_number(json_object_get(ex->root, "release"));
	n = snprintf(out, out_size, "%s-%lld", version, release);
	if (n < 0 || (size_t)n >= out_size) {
		out[0] = '\0';
		return -1;
	}
	return 0;
}

static const struct json_value *sources_array(const struct pbs_explain *ex)
{
	const struct json_value *v;

	if (ex == NULL)
		return NULL;
	v = json_object_get(ex->root, "sources");
	if (v == NULL || v->type != JSON_ARRAY)
		return NULL;
	return v;
}

int pbs_explain_source_count(const struct pbs_explain *ex)
{
	const struct json_value *v = sources_array(ex);

	return v == NULL ? 0 : (int)v->u.array.count;
}

int pbs_explain_source(const struct pbs_explain *ex, int index, char *url, size_t url_size,
                        char *sha256, size_t sha256_size)
{
	const struct json_value *sources = sources_array(ex);
	const struct json_value *source;
	const struct json_value *urls;
	const char *first_url;
	const char *sha;

	if (sources == NULL || index < 0 || (size_t)index >= sources->u.array.count)
		return -1;
	source = sources->u.array.items[index];
	if (source == NULL || source->type != JSON_OBJECT)
		return -1;

	urls = json_object_get(source, "urls");
	if (urls == NULL || urls->type != JSON_ARRAY || urls->u.array.count == 0)
		return -1;
	first_url = json_as_string(urls->u.array.items[0]);
	sha = json_as_string(json_object_get(source, "sha256"));
	if (first_url == NULL || sha == NULL)
		return -1;

	if (strlen(first_url) >= url_size || strlen(sha) >= sha256_size)
		return -1;
	snprintf(url, url_size, "%s", first_url);
	snprintf(sha256, sha256_size, "%s", sha);
	return 0;
}

int pbs_explain_requires(const struct pbs_explain *ex, const char *role, const char *kind,
                          char *out, size_t out_size)
{
	const struct json_value *requires_obj;
	const struct json_value *group;
	const struct json_value *items;
	size_t i;
	size_t off = 0;

	if (ex == NULL || out == NULL || out_size == 0)
		return -1;
	out[0] = '\0';

	requires_obj = json_object_get(ex->root, "requires");
	if (requires_obj == NULL || requires_obj->type != JSON_OBJECT)
		return 0; /* declares no dependencies at all */
	group = json_object_get(requires_obj, role);
	if (group == NULL || group->type != JSON_OBJECT)
		return 0; /* no such role */
	items = json_object_get(group, kind);
	if (items == NULL || items->type != JSON_ARRAY)
		return 0; /* no such keyword in this role */

	for (i = 0; i < items->u.array.count; i++) {
		const char *value = json_as_string(items->u.array.items[i]);
		int n;

		if (value == NULL || value[0] == '\0')
			continue;
		n = snprintf(out + off, out_size - off, "%s%s", off > 0 ? " " : "", value);
		if (n < 0 || (size_t)n >= out_size - off) {
			out[0] = '\0';
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

const char *pbs_explain_upstream(const struct pbs_explain *ex)
{
	if (ex == NULL)
		return "";
	return str_or_empty(json_object_get(ex->root, "upstream"));
}

const char *pbs_explain_toolchain(const struct pbs_explain *ex)
{
	if (ex == NULL)
		return "";
	return str_or_empty(json_object_get(ex->root, "toolchain"));
}

const char *pbs_explain_toolchain_reason(const struct pbs_explain *ex)
{
	if (ex == NULL)
		return "";
	return str_or_empty(json_object_get(ex->root, "toolchain_reason"));
}

int pbs_explain_capability_count(const struct pbs_explain *ex)
{
	const struct json_value *v;

	if (ex == NULL)
		return 0;
	v = json_object_get(ex->root, "capabilities");
	if (v == NULL || v->type != JSON_NUMBER)
		return 0;
	return (int)json_as_number(v);
}

int pbs_explain_phase_count(const struct pbs_explain *ex)
{
	const struct json_value *v;

	if (ex == NULL)
		return 0;
	v = json_object_get(ex->root, "phases");
	if (v == NULL || v->type != JSON_ARRAY)
		return 0;
	return (int)v->u.array.count;
}
