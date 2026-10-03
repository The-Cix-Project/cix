#include "srcrecord.h"

#include "json.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_dir[PATH_MAX];

int srcrecord_init(const char *dir)
{
	if (dir == NULL || (size_t)snprintf(g_dir, sizeof(g_dir), "%s", dir) >= sizeof(g_dir))
		return -1;
	return persist_mkdir_p(g_dir);
}

/* A package name is a file name here, so only what one may safely be. */
static int record_path(const char *package, char *out, size_t out_size)
{
	if (g_dir[0] == '\0' || package == NULL || package[0] == '\0' || package[0] == '.' ||
	    strspn(package, "abcdefghijklmnopqrstuvwxyz0123456789._+-") != strlen(package))
		return -1;
	return (size_t)snprintf(out, out_size, "%s/%s.json", g_dir, package) < out_size ? 0 : -1;
}

/* The record for `package`, or NULL when there is none. */
static struct json_value *load(const char *package)
{
	char path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	if (record_path(package, path, sizeof(path)) != 0 ||
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

/* Rewrites `package`'s document with `key` set to the JSON text `raw`
 * (or removed when NULL), keeping every other field. */
static int put_field(const char *package, const char *key, const char *raw)
{
	char path[PATH_MAX];
	struct json_value *root;
	struct json_writer w;
	size_t i;
	int rc;

	if (record_path(package, path, sizeof(path)) != 0)
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

int srcrecord_store_note(const char *package, const struct srcrecord_note *note)
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

int srcrecord_note(const char *package, struct srcrecord_note *out)
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

int srcrecord_store_candidate(const char *package, const struct srcrecord_candidate *c)
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

int srcrecord_candidate(const char *package, struct srcrecord_candidate *out)
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

