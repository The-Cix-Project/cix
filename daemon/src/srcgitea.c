#include "srcgitea.h"

#include "json.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_dir[PATH_MAX];

#define TAGS_PAGE "/tags?limit=50&page=1"

int srcgitea_tags_url(const char *source, char *out, size_t out_size)
{
	static const char marker[] = "/api/v1/repos/";
	const char *repos, *owner_end, *repo_end;

	if (source == NULL || out == NULL || out_size == 0)
		return -1;
	out[0] = '\0';
	if (strncmp(source, "https://", 8) != 0 && strncmp(source, "http://", 7) != 0)
		return -1;
	repos = strstr(source, marker);
	if (repos == NULL)
		return -1;
	owner_end = strchr(repos + sizeof(marker) - 1, '/');
	if (owner_end == NULL || owner_end == repos + sizeof(marker) - 1)
		return -1;
	repo_end = strchr(owner_end + 1, '/');
	if (repo_end == NULL || repo_end == owner_end + 1 || strncmp(repo_end, "/archive/", 9) != 0)
		return -1;
	if ((size_t)snprintf(out, out_size, "%.*s%s", (int)(repo_end - source), source, TAGS_PAGE) >=
	    out_size) {
		out[0] = '\0';
		return -1;
	}
	return 0;
}

int srcgitea_version_of_tag(const char *tag, const char *tmpl, char *out, size_t out_size)
{
	const char *slot;
	size_t prefix, suffix, tag_len, ver_len;

	if (tag == NULL || tmpl == NULL || out == NULL || out_size == 0)
		return -1;
	out[0] = '\0';
	slot = strstr(tmpl, "{version}");
	if (slot == NULL)
		return -1;
	prefix = (size_t)(slot - tmpl);
	suffix = strlen(slot + 9);
	tag_len = strlen(tag);
	if (tag_len <= prefix + suffix || strncmp(tag, tmpl, prefix) != 0 ||
	    strcmp(tag + tag_len - suffix, slot + 9) != 0)
		return -1;
	ver_len = tag_len - prefix - suffix;
	if (ver_len >= out_size)
		return -1;
	memcpy(out, tag + prefix, ver_len);
	out[ver_len] = '\0';
	if (strpbrk(out, "/ \t\"{}") != NULL) {
		out[0] = '\0';
		return -1;
	}
	return 0;
}

int srcgitea_versions_from_listing(const char *json, size_t len, const char *tmpl,
                                   char out[][SRCUPSTREAM_VERSION_MAX], size_t max)
{
	struct json_value *root;
	size_t i;
	int n = 0;

	if (json == NULL || tmpl == NULL)
		return -1;
	root = json_parse(json, len);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		return -1;
	}
	for (i = 0; i < root->u.array.count && (size_t)n < max; i++) {
		const char *name = json_as_string(json_object_get(root->u.array.items[i], "name"));

		if (name != NULL &&
		    srcgitea_version_of_tag(name, tmpl, out[n], SRCUPSTREAM_VERSION_MAX) == 0)
			n++;
	}
	json_free(root);
	return n;
}

int srcgitea_init(const char *dir)
{
	if (dir == NULL || (size_t)snprintf(g_dir, sizeof(g_dir), "%s", dir) >= sizeof(g_dir))
		return -1;
	return persist_mkdir_p(g_dir);
}

/* A package name is a file name here, so only what one may safely be. */
static int cache_path(const char *package, char *out, size_t out_size)
{
	if (g_dir[0] == '\0' || package == NULL || package[0] == '\0' || package[0] == '.' ||
	    strspn(package, "abcdefghijklmnopqrstuvwxyz0123456789._+-") != strlen(package))
		return -1;
	return (size_t)snprintf(out, out_size, "%s/%s.json", g_dir, package) < out_size ? 0 : -1;
}

int srcgitea_store_listing(const char *package, const char *json, size_t len, const char *tmpl,
                           long now)
{
	char versions[SRCUPSTREAM_MAX_CANDIDATES][SRCUPSTREAM_VERSION_MAX];
	char path[PATH_MAX];
	struct json_writer w;
	int n, i, rc;

	if (cache_path(package, path, sizeof(path)) != 0)
		return -1;
	n = srcgitea_versions_from_listing(json, len, tmpl, versions, SRCUPSTREAM_MAX_CANDIDATES);
	if (n < 0)
		return -1;
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "fetched_at");
	jw_int(&w, now);
	jw_key(&w, "versions");
	jw_arr_open(&w);
	for (i = 0; i < n; i++)
		jw_str(&w, versions[i]);
	jw_arr_close(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

/* The cache document for `package`, or NULL when there is none. */
static struct json_value *load(const char *package)
{
	char path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	if (cache_path(package, path, sizeof(path)) != 0 ||
	    persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return NULL;
	root = json_parse(buf, len);
	free(buf);
	if (root != NULL && root->type != JSON_OBJECT) {
		json_free(root);
		return NULL;
	}
	return root;
}

size_t srcgitea_candidates(const char *package, char out[][SRCUPSTREAM_VERSION_MAX], size_t max)
{
	struct json_value *root = load(package);
	const struct json_value *arr;
	size_t i, n = 0;

	if (root == NULL)
		return 0;
	arr = json_object_get(root, "versions");
	for (i = 0; arr != NULL && arr->type == JSON_ARRAY && i < arr->u.array.count && n < max; i++) {
		const char *v = json_as_string(arr->u.array.items[i]);

		if (v != NULL && v[0] != '\0' && strlen(v) < SRCUPSTREAM_VERSION_MAX)
			snprintf(out[n++], SRCUPSTREAM_VERSION_MAX, "%s", v);
	}
	json_free(root);
	return n;
}

long srcgitea_fetched_at(const char *package)
{
	struct json_value *root = load(package);
	long t;

	if (root == NULL)
		return 0;
	t = (long)json_as_number(json_object_get(root, "fetched_at"));
	json_free(root);
	return t;
}

int srcgitea_store_error(const char *package, const char *why, long now)
{
	char versions[SRCUPSTREAM_MAX_CANDIDATES][SRCUPSTREAM_VERSION_MAX];
	char path[PATH_MAX];
	struct json_writer w;
	size_t n, i;
	long fetched;
	int rc;

	if (cache_path(package, path, sizeof(path)) != 0 || why == NULL)
		return -1;
	/* What an earlier refresh found stays: a failed fetch makes the
	 * data old, not wrong, and fetched_at already says how old. */
	n = srcgitea_candidates(package, versions, SRCUPSTREAM_MAX_CANDIDATES);
	fetched = srcgitea_fetched_at(package);
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "fetched_at");
	jw_int(&w, fetched);
	jw_key(&w, "versions");
	jw_arr_open(&w);
	for (i = 0; i < n; i++)
		jw_str(&w, versions[i]);
	jw_arr_close(&w);
	jw_key(&w, "error");
	jw_str(&w, why);
	jw_key(&w, "error_at");
	jw_int(&w, now);
	jw_obj_close(&w);
	rc = persist_atomic_write(path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int srcgitea_error(const char *package, char *out, size_t out_size)
{
	struct json_value *root;
	const char *e;

	if (out == NULL || out_size == 0)
		return -1;
	out[0] = '\0';
	root = load(package);
	if (root == NULL)
		return 0;
	e = json_as_string(json_object_get(root, "error"));
	if (e != NULL)
		snprintf(out, out_size, "%s", e);
	json_free(root);
	return 0;
}

/* Rewrites `package`'s document with `key` set to the JSON text `raw`
 * (or removed when NULL), keeping every other field. */
static int put_field(const char *package, const char *key, const char *raw)
{
	char path[PATH_MAX];
	struct json_value *root;
	struct json_writer w;
	size_t i;
	int rc;

	if (cache_path(package, path, sizeof(path)) != 0)
		return -1;
	root = load(package);
	jw_init(&w);
	jw_obj_open(&w);
	for (i = 0; root != NULL && i < root->u.object.count; i++) {
		if (strcmp(root->u.object.keys[i], key) == 0)
			continue;
		jw_key(&w, root->u.object.keys[i]);
		jw_value(&w, root->u.object.values[i]);
	}
	if (raw != NULL) {
		jw_key(&w, key);
		jw_raw_text(&w, raw, strlen(raw));
	}
	jw_obj_close(&w);
	rc = persist_atomic_write(path, w.buf, w.len);
	jw_free(&w);
	json_free(root);
	return rc;
}

/* The string field `key` of object `o`, copied into out. */
static void copy_str(const struct json_value *o, const char *key, char *out, size_t out_size)
{
	const char *s = json_as_string(json_object_get(o, key));

	snprintf(out, out_size, "%s", s != NULL ? s : "");
}

int srcgitea_store_note(const char *package, const struct srcgitea_note *note)
{
	struct json_writer w;
	int rc;

	if (note == NULL)
		return put_field(package, "note", NULL);
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "version");
	jw_str(&w, note->version);
	jw_key(&w, "stage");
	jw_str(&w, note->stage);
	jw_key(&w, "status");
	jw_str(&w, note->status);
	jw_key(&w, "reason");
	jw_str(&w, note->reason);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	rc = put_field(package, "note", w.buf);
	jw_free(&w);
	return rc;
}

int srcgitea_note(const char *package, struct srcgitea_note *out)
{
	struct json_value *root = load(package);
	const struct json_value *n = root != NULL ? json_object_get(root, "note") : NULL;

	memset(out, 0, sizeof(*out));
	if (n == NULL || n->type != JSON_OBJECT) {
		json_free(root);
		return -1;
	}
	copy_str(n, "version", out->version, sizeof(out->version));
	copy_str(n, "stage", out->stage, sizeof(out->stage));
	copy_str(n, "status", out->status, sizeof(out->status));
	copy_str(n, "reason", out->reason, sizeof(out->reason));
	json_free(root);
	return 0;
}

int srcgitea_store_candidate(const char *package, const struct srcgitea_candidate *c)
{
	struct json_writer w;
	int rc;

	if (c == NULL)
		return put_field(package, "candidate", NULL);
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "version");
	jw_str(&w, c->version);
	jw_key(&w, "url");
	jw_str(&w, c->url);
	jw_key(&w, "sha256");
	jw_str(&w, c->sha256);
	jw_key(&w, "verification");
	jw_str(&w, c->verification);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	rc = put_field(package, "candidate", w.buf);
	jw_free(&w);
	return rc;
}

int srcgitea_candidate(const char *package, struct srcgitea_candidate *out)
{
	struct json_value *root = load(package);
	const struct json_value *c = root != NULL ? json_object_get(root, "candidate") : NULL;

	memset(out, 0, sizeof(*out));
	if (c == NULL || c->type != JSON_OBJECT) {
		json_free(root);
		return -1;
	}
	copy_str(c, "version", out->version, sizeof(out->version));
	copy_str(c, "url", out->url, sizeof(out->url));
	copy_str(c, "sha256", out->sha256, sizeof(out->sha256));
	copy_str(c, "verification", out->verification, sizeof(out->verification));
	json_free(root);
	return out->version[0] != '\0' && out->url[0] != '\0' && strlen(out->sha256) == 64 ? 0 : -1;
}

int srcgitea_expand(const char *tmpl, const char *version, char *out, size_t out_size)
{
	const char *p;
	size_t n = 0, major;

	if (tmpl == NULL || version == NULL || out == NULL || out_size == 0)
		return -1;
	major = strcspn(version, ".");
	for (p = tmpl; *p != '\0';) {
		const char *piece = p;
		size_t len = 1;

		if (strncmp(p, "{version}", 9) == 0) {
			piece = version;
			len = strlen(version);
			p += 9;
		} else if (strncmp(p, "{major}", 7) == 0) {
			piece = version;
			len = major;
			p += 7;
		} else {
			p++;
		}
		if (n + len >= out_size) {
			out[0] = '\0';
			return -1;
		}
		memcpy(out + n, piece, len);
		n += len;
	}
	out[n] = '\0';
	return 0;
}
