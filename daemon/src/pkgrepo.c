#include "pkgrepo.h"

#include "json.h"
#include "persist.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_path[PATH_MAX];
static struct pkg_repository g_repos[PKG_REPOSITORIES_MAX];
static int g_count;

static enum pkgsource_error validate(const struct pkg_repository *r, char *err, size_t err_size)
{
	if (!pkgsource_name_is_valid(r->name)) {
		snprintf(err, err_size, "a repository name is [a-z0-9][a-z0-9._-]*, under %d characters",
		         PKG_SOURCE_NAME_MAX);
		return PKGSOURCE_ERR_INVALID;
	}
	if (r->url[0] == '\0' || strstr(r->url, "://") == NULL) {
		snprintf(err, err_size, "repository %s needs a url, <scheme>://<host>[/<path>]",
		         r->name);
		return PKGSOURCE_ERR_INVALID;
	}
	return PKGSOURCE_OK;
}

static int save(void)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "repositories");
	jw_arr_open(&w);
	for (i = 0; i < g_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, g_repos[i].name);
		jw_key(&w, "url");
		jw_str(&w, g_repos[i].url);
		jw_key(&w, "token");
		jw_str(&w, g_repos[i].token);
		jw_key(&w, "push");
		jw_bool(&w, g_repos[i].push);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static void load(const char *buf, size_t len)
{
	struct json_value *root = json_parse(buf, len);
	const struct json_value *arr = root != NULL ? json_object_get(root, "repositories") : NULL;
	size_t i;

	if (arr != NULL && arr->type == JSON_ARRAY) {
		for (i = 0; i < arr->u.array.count && g_count < PKG_REPOSITORIES_MAX; i++) {
			const struct json_value *o = arr->u.array.items[i];
			const struct json_value *push = json_object_get(o, "push");
			struct pkg_repository *r = &g_repos[g_count];
			const char *s;

			memset(r, 0, sizeof(*r));
			s = json_as_string(json_object_get(o, "name"));
			snprintf(r->name, sizeof(r->name), "%s", s != NULL ? s : "");
			s = json_as_string(json_object_get(o, "url"));
			snprintf(r->url, sizeof(r->url), "%s", s != NULL ? s : "");
			s = json_as_string(json_object_get(o, "token"));
			snprintf(r->token, sizeof(r->token), "%s", s != NULL ? s : "");
			r->push = push != NULL && push->type == JSON_BOOL && push->u.boolean;
			if (pkgsource_name_is_valid(r->name) && r->url[0] != '\0')
				g_count++;
		}
	}
	json_free(root);
}

/*
 * The name a migrated server gets: its host, lower-cased, `:` written
 * `-` -- so 192.168.15.95's LAN cache is "192.168.15.31-8080" -- or
 * "migrated" when that does not make a valid name.
 */
static void name_from_url(const char *url, char *out, size_t out_size)
{
	const char *h = strstr(url, "://");
	size_t i = 0;

	h = h != NULL ? h + 3 : url;
	while (*h != '\0' && *h != '/' && i + 1 < out_size) {
		char c = (char)tolower((unsigned char)*h++);

		out[i++] = c == ':' ? '-' : c;
	}
	out[i] = '\0';
	if (!pkgsource_name_is_valid(out))
		snprintf(out, out_size, "%s", "migrated");
}

static void migrate_legacy(const char *buf, size_t len)
{
	struct json_value *root = json_parse(buf, len);
	const char *url = root != NULL ? json_as_string(json_object_get(root, "base_url")) : NULL;
	const char *token = root != NULL ? json_as_string(json_object_get(root, "auth_token")) : NULL;
	const struct json_value *push = root != NULL ? json_object_get(root, "push_enabled") : NULL;
	struct pkg_repository *r = &g_repos[0];

	/* ADR-0315: a saved empty url was a cleared config, and stays cleared. */
	if (url != NULL && url[0] != '\0') {
		memset(r, 0, sizeof(*r));
		snprintf(r->url, sizeof(r->url), "%s", url);
		snprintf(r->token, sizeof(r->token), "%s", token != NULL ? token : "");
		r->push = push != NULL && push->type == JSON_BOOL && push->u.boolean;
		name_from_url(url, r->name, sizeof(r->name));
		g_count = 1;
	}
	json_free(root);
}

int pkgrepo_init(const char *path, const char *legacy_path)
{
	char *buf = NULL;
	size_t len = 0;

	snprintf(g_path, sizeof(g_path), "%s", path);
	g_count = 0;

	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		load(buf, len);
		free(buf);
		return 0;
	}
	if (legacy_path != NULL && persist_read_file(legacy_path, &buf, &len) == 0 &&
	    buf != NULL) {
		migrate_legacy(buf, len);
		free(buf);
	} else {
		struct pkg_repository *r = &g_repos[0];

		memset(r, 0, sizeof(*r));
		snprintf(r->name, sizeof(r->name), "%s", PKG_REPOSITORY_DEFAULT_NAME);
		snprintf(r->url, sizeof(r->url), "%s", PKG_REPOSITORY_DEFAULT_URL);
		g_count = 1;
	}
	if (save() != 0)
		return -1;
	/* Clean cut-over: once the list holds it, nothing can read a second copy. */
	if (legacy_path != NULL)
		unlink(legacy_path);
	return 0;
}

void pkgrepo_repoint(const char *path)
{
	snprintf(g_path, sizeof(g_path), "%s", path);
}

int pkgrepo_count(void)
{
	return g_count;
}

const struct pkg_repository *pkgrepo_at(int i)
{
	return i >= 0 && i < g_count ? &g_repos[i] : NULL;
}

static int index_of(const char *name)
{
	int i;

	for (i = 0; name != NULL && i < g_count; i++)
		if (strcmp(g_repos[i].name, name) == 0)
			return i;
	return -1;
}

const struct pkg_repository *pkgrepo_find(const char *name)
{
	int i = index_of(name);

	return i >= 0 ? &g_repos[i] : NULL;
}

int pkgrepo_push_count(void)
{
	int i, n = 0;

	for (i = 0; i < g_count; i++)
		if (g_repos[i].push)
			n++;
	return n;
}

enum pkgsource_error pkgrepo_add(const struct pkg_repository *r, char *err, size_t err_size)
{
	enum pkgsource_error e;

	err[0] = '\0';
	e = validate(r, err, err_size);
	if (e != PKGSOURCE_OK)
		return e;
	if (index_of(r->name) >= 0) {
		snprintf(err, err_size, "a repository named %s already exists", r->name);
		return PKGSOURCE_ERR_EXISTS;
	}
	if (g_count >= PKG_REPOSITORIES_MAX) {
		snprintf(err, err_size, "a host holds at most %d repositories", PKG_REPOSITORIES_MAX);
		return PKGSOURCE_ERR_FULL;
	}
	g_repos[g_count++] = *r;
	if (save() != 0) {
		g_count--;
		snprintf(err, err_size, "could not save the repository list");
		return PKGSOURCE_ERR_PERSIST;
	}
	return PKGSOURCE_OK;
}

enum pkgsource_error pkgrepo_update(const char *name, const char *url, const char *token,
                                    int push, char *err, size_t err_size)
{
	int i = index_of(name);
	struct pkg_repository copy, before;
	enum pkgsource_error e;

	err[0] = '\0';
	if (i < 0) {
		snprintf(err, err_size, "no repository named %s", name != NULL ? name : "");
		return PKGSOURCE_ERR_NOT_FOUND;
	}
	copy = g_repos[i];
	if (url != NULL)
		snprintf(copy.url, sizeof(copy.url), "%s", url);
	if (token != NULL)
		snprintf(copy.token, sizeof(copy.token), "%s", token);
	if (push == 0 || push == 1)
		copy.push = push;
	e = validate(&copy, err, err_size);
	if (e != PKGSOURCE_OK)
		return e;
	before = g_repos[i];
	g_repos[i] = copy;
	if (save() != 0) {
		g_repos[i] = before;
		snprintf(err, err_size, "could not save the repository list");
		return PKGSOURCE_ERR_PERSIST;
	}
	return PKGSOURCE_OK;
}

enum pkgsource_error pkgrepo_remove(const char *name, char *err, size_t err_size)
{
	int i = index_of(name);
	int j;

	err[0] = '\0';
	if (i < 0) {
		snprintf(err, err_size, "no repository named %s", name != NULL ? name : "");
		return PKGSOURCE_ERR_NOT_FOUND;
	}
	for (j = i; j < g_count - 1; j++)
		g_repos[j] = g_repos[j + 1];
	g_count--;
	if (save() != 0) {
		snprintf(err, err_size, "could not save the repository list");
		return PKGSOURCE_ERR_PERSIST;
	}
	return PKGSOURCE_OK;
}

void pkgrepo_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < g_count; i++) {
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, g_repos[i].name);
		jw_key(w, "url");
		jw_str(w, g_repos[i].url);
		jw_key(w, "token_set");
		jw_bool(w, g_repos[i].token[0] != '\0');
		jw_key(w, "push");
		jw_bool(w, g_repos[i].push);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

void pkgrepo_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "repositories");
	pkgrepo_write_json_list(w);
	jw_obj_close(w);
}
