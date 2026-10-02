#include "pkgsource.h"

#include "json.h"
#include "persist.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_path[PATH_MAX];
static char g_offers_dir[PATH_MAX];
static struct pkg_source g_sources[PKG_SOURCES_MAX];
static int g_count;
static int g_legacy_sync_interval; /* ADR-0257, from a migrated repo config */
static char g_migrated[PKG_SOURCE_NAME_MAX]; /* the source a legacy config became, this boot */

/* One offered item and the sources offering it, as a bit per source
 * index -- PKG_SOURCES_MAX is 32, so one uint32_t holds them all. */
struct offer {
	char item[PKG_SOURCE_ITEM_MAX];
	uint32_t by;
};
static struct offer *g_offers;
static int g_offer_count;
static int g_offer_cap;

struct choice {
	char item[PKG_SOURCE_ITEM_MAX];
	char source[PKG_SOURCE_NAME_MAX];
};
static struct choice *g_choices;
static int g_choice_count;
static int g_choice_cap;

/* ---- validation ---- */

static int name_is_valid(const char *s)
{
	size_t i, n = s != NULL ? strlen(s) : 0;

	if (n == 0 || n >= PKG_SOURCE_NAME_MAX)
		return 0;
	if (!(islower((unsigned char)s[0]) || isdigit((unsigned char)s[0])))
		return 0;
	for (i = 1; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if (!(islower(c) || isdigit(c) || c == '.' || c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

static int kind_is_valid(const char *k)
{
	return k != NULL &&
	       (strcmp(k, "gitea") == 0 || strcmp(k, "github") == 0 || strcmp(k, "gitlab") == 0);
}

int pkgsource_kind_can_write(const char *kind)
{
	return kind != NULL && strcmp(kind, "gitea") == 0;
}

static enum pkgsource_error validate(const struct pkg_source *s, char *err, size_t err_size)
{
	if (!name_is_valid(s->name)) {
		snprintf(err, err_size, "a source name is [a-z0-9][a-z0-9._-]*, under %d characters",
		         PKG_SOURCE_NAME_MAX);
		return PKGSOURCE_ERR_INVALID;
	}
	if (s->url[0] == '\0' || strstr(s->url, "://") == NULL) {
		snprintf(err, err_size, "source %s needs a url, <scheme>://<host>/<owner>/<repo>",
		         s->name);
		return PKGSOURCE_ERR_INVALID;
	}
	if (!kind_is_valid(s->kind)) {
		snprintf(err, err_size, "source %s: kind is gitea, github or gitlab", s->name);
		return PKGSOURCE_ERR_INVALID;
	}
	if (s->write && !pkgsource_kind_can_write(s->kind)) {
		snprintf(err, err_size,
		         "source %s: write needs a gitea repository, the only forge with a commit "
		         "client (ADR-0323)",
		         s->name);
		return PKGSOURCE_ERR_INVALID;
	}
	return PKGSOURCE_OK;
}

/* ---- persistence ---- */

static int save(void)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "sources");
	jw_arr_open(&w);
	for (i = 0; i < g_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, g_sources[i].name);
		jw_key(&w, "url");
		jw_str(&w, g_sources[i].url);
		jw_key(&w, "kind");
		jw_str(&w, g_sources[i].kind);
		jw_key(&w, "ref");
		jw_str(&w, g_sources[i].ref);
		jw_key(&w, "token");
		jw_str(&w, g_sources[i].token);
		jw_key(&w, "write");
		jw_bool(&w, g_sources[i].write);
		jw_key(&w, "trust_keys");
		jw_bool(&w, g_sources[i].trust_keys);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_key(&w, "choices");
	jw_arr_open(&w);
	for (i = 0; i < g_choice_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "item");
		jw_str(&w, g_choices[i].item);
		jw_key(&w, "source");
		jw_str(&w, g_choices[i].source);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static int bool_field(const struct json_value *o, const char *k)
{
	const struct json_value *v = json_object_get(o, k);

	return v != NULL && v->type == JSON_BOOL && v->u.boolean;
}

static void copy_field(char *dst, size_t size, const struct json_value *o, const char *k,
                       const char *fallback)
{
	const char *s = json_as_string(json_object_get(o, k));

	snprintf(dst, size, "%s", s != NULL ? s : fallback);
}

static int choice_add(const char *item, const char *source);

static int load(const char *buf, size_t len)
{
	struct json_value *root = json_parse(buf, len);
	const struct json_value *arr;
	size_t i;

	if (root == NULL)
		return -1;
	arr = json_object_get(root, "sources");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		for (i = 0; i < arr->u.array.count && g_count < PKG_SOURCES_MAX; i++) {
			const struct json_value *o = arr->u.array.items[i];
			struct pkg_source *s = &g_sources[g_count];

			memset(s, 0, sizeof(*s));
			copy_field(s->name, sizeof(s->name), o, "name", "");
			copy_field(s->url, sizeof(s->url), o, "url", "");
			copy_field(s->kind, sizeof(s->kind), o, "kind", "");
			copy_field(s->ref, sizeof(s->ref), o, "ref", PKG_SOURCE_DEFAULT_REF);
			copy_field(s->token, sizeof(s->token), o, "token", "");
			s->write = bool_field(o, "write");
			s->trust_keys = bool_field(o, "trust_keys");
			/* A hand-edited file cannot smuggle in what the API refuses. */
			if (name_is_valid(s->name) && pkgsource_find(s->name) == NULL &&
			    kind_is_valid(s->kind) && s->url[0] != '\0')
				g_count++;
		}
	}
	arr = json_object_get(root, "choices");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		for (i = 0; i < arr->u.array.count; i++) {
			const struct json_value *o = arr->u.array.items[i];
			const char *item = json_as_string(json_object_get(o, "item"));
			const char *source = json_as_string(json_object_get(o, "source"));

			if (item != NULL && source != NULL && pkgsource_find(source) != NULL)
				choice_add(item, source);
		}
	}
	json_free(root);
	return 0;
}

/*
 * The single repository ADR-0121 kept becomes the first source. It is
 * named after its repository segment, so .95's LAN forge becomes
 * "cix-recipes"; a name that will not validate becomes "migrated".
 */
static void migrate_legacy(const char *buf, size_t len)
{
	struct json_value *root = json_parse(buf, len);
	struct pkg_source *s = &g_sources[0];
	const char *url, *slash;
	size_t n;

	if (root == NULL)
		return;
	url = json_as_string(json_object_get(root, "repo_url"));
	if (url == NULL || url[0] == '\0') {
		/* Cleared by an operator: stays cleared (ADR-0315). */
		json_free(root);
		return;
	}
	memset(s, 0, sizeof(*s));
	snprintf(s->url, sizeof(s->url), "%s", url);
	copy_field(s->kind, sizeof(s->kind), root, "repo_kind", PKG_SOURCE_DEFAULT_KIND);
	copy_field(s->ref, sizeof(s->ref), root, "ref", PKG_SOURCE_DEFAULT_REF);
	if (s->ref[0] == '\0')
		snprintf(s->ref, sizeof(s->ref), "%s", PKG_SOURCE_DEFAULT_REF);
	copy_field(s->token, sizeof(s->token), root, "auth_token", "");
	s->write = bool_field(root, "commit") && pkgsource_kind_can_write(s->kind);
	s->trust_keys = 1;
	{
		const struct json_value *iv = json_object_get(root, "sync_interval_seconds");

		if (iv != NULL)
			g_legacy_sync_interval = (int)json_as_number(iv);
	}

	/* <scheme>://<host>/<owner>/<repo>[.git][/...] -> <repo> */
	slash = strstr(url, "://");
	slash = slash != NULL ? strchr(slash + 3, '/') : NULL;
	slash = slash != NULL ? strchr(slash + 1, '/') : NULL;
	if (slash != NULL) {
		snprintf(s->name, sizeof(s->name), "%s", slash + 1);
		s->name[strcspn(s->name, "/")] = '\0';
		n = strlen(s->name);
		if (n > 4 && strcmp(s->name + n - 4, ".git") == 0)
			s->name[n - 4] = '\0';
		for (n = 0; s->name[n] != '\0'; n++)
			s->name[n] = (char)tolower((unsigned char)s->name[n]);
	}
	if (!name_is_valid(s->name))
		snprintf(s->name, sizeof(s->name), "%s", "migrated");
	if (kind_is_valid(s->kind))
		g_count = 1;
	json_free(root);
}

int pkgsource_init(const char *path, const char *legacy_path, const char *offers_dir)
{
	char *buf = NULL;
	size_t len = 0;

	snprintf(g_path, sizeof(g_path), "%s", path);
	snprintf(g_offers_dir, sizeof(g_offers_dir), "%s", offers_dir);
	g_count = 0;
	g_choice_count = 0;
	g_legacy_sync_interval = 0;
	g_migrated[0] = '\0';

	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		load(buf, len);
		free(buf);
		pkgsource_offers_load();
		return 0;
	}

	if (legacy_path != NULL && persist_read_file(legacy_path, &buf, &len) == 0 &&
	    buf != NULL) {
		migrate_legacy(buf, len);
		if (g_count == 1)
			snprintf(g_migrated, sizeof(g_migrated), "%s", g_sources[0].name);
		free(buf);
	} else {
		struct pkg_source *s = &g_sources[0];

		memset(s, 0, sizeof(*s));
		snprintf(s->name, sizeof(s->name), "%s", PKG_SOURCE_DEFAULT_NAME);
		snprintf(s->url, sizeof(s->url), "%s", PKG_SOURCE_DEFAULT_URL);
		snprintf(s->kind, sizeof(s->kind), "%s", PKG_SOURCE_DEFAULT_KIND);
		snprintf(s->ref, sizeof(s->ref), "%s", PKG_SOURCE_DEFAULT_REF);
		s->trust_keys = 1;
		g_count = 1;
	}
	if (save() != 0)
		return -1;
	/* Clean cut-over: the single-repository file is gone once its
	 * content lives here, so nothing can read a second copy. */
	if (legacy_path != NULL)
		unlink(legacy_path);
	pkgsource_offers_load();
	return 0;
}

int pkgsource_legacy_sync_interval_seconds(void)
{
	return g_legacy_sync_interval;
}

void pkgsource_clear_legacy_sync_interval(void)
{
	g_legacy_sync_interval = 0;
}

const char *pkgsource_migrated(void)
{
	return g_migrated[0] != '\0' ? g_migrated : NULL;
}

void pkgsource_repoint(const char *path, const char *offers_dir)
{
	snprintf(g_path, sizeof(g_path), "%s", path);
	snprintf(g_offers_dir, sizeof(g_offers_dir), "%s", offers_dir);
}

/* ---- the list ---- */

int pkgsource_count(void)
{
	return g_count;
}

const struct pkg_source *pkgsource_at(int i)
{
	return i >= 0 && i < g_count ? &g_sources[i] : NULL;
}

const struct pkg_source *pkgsource_find(const char *name)
{
	int i;

	for (i = 0; name != NULL && i < g_count; i++)
		if (strcmp(g_sources[i].name, name) == 0)
			return &g_sources[i];
	return NULL;
}

static int index_of(const char *name)
{
	int i;

	for (i = 0; name != NULL && i < g_count; i++)
		if (strcmp(g_sources[i].name, name) == 0)
			return i;
	return -1;
}

enum pkgsource_error pkgsource_add(const struct pkg_source *s, char *err, size_t err_size)
{
	struct pkg_source copy = *s;
	enum pkgsource_error e;

	err[0] = '\0';
	if (copy.ref[0] == '\0')
		snprintf(copy.ref, sizeof(copy.ref), "%s", PKG_SOURCE_DEFAULT_REF);
	e = validate(&copy, err, err_size);
	if (e != PKGSOURCE_OK)
		return e;
	if (pkgsource_find(copy.name) != NULL) {
		snprintf(err, err_size, "a source named %s already exists", copy.name);
		return PKGSOURCE_ERR_EXISTS;
	}
	if (g_count >= PKG_SOURCES_MAX) {
		snprintf(err, err_size, "a host holds at most %d sources", PKG_SOURCES_MAX);
		return PKGSOURCE_ERR_FULL;
	}
	g_sources[g_count++] = copy;
	if (save() != 0) {
		g_count--;
		snprintf(err, err_size, "could not save the source list");
		return PKGSOURCE_ERR_PERSIST;
	}
	return PKGSOURCE_OK;
}

enum pkgsource_error pkgsource_update(const char *name, const char *url, const char *kind,
                                      const char *ref, const char *token, int write,
                                      int trust_keys, char *err, size_t err_size)
{
	int i = index_of(name);
	struct pkg_source copy;
	enum pkgsource_error e;

	err[0] = '\0';
	if (i < 0) {
		snprintf(err, err_size, "no source named %s", name != NULL ? name : "");
		return PKGSOURCE_ERR_NOT_FOUND;
	}
	copy = g_sources[i];
	if (url != NULL)
		snprintf(copy.url, sizeof(copy.url), "%s", url);
	if (kind != NULL)
		snprintf(copy.kind, sizeof(copy.kind), "%s", kind);
	if (ref != NULL && ref[0] != '\0')
		snprintf(copy.ref, sizeof(copy.ref), "%s", ref);
	if (token != NULL)
		snprintf(copy.token, sizeof(copy.token), "%s", token);
	if (write == 0 || write == 1)
		copy.write = write;
	if (trust_keys == 0 || trust_keys == 1)
		copy.trust_keys = trust_keys;
	e = validate(&copy, err, err_size);
	if (e != PKGSOURCE_OK)
		return e;
	{
		struct pkg_source before = g_sources[i];

		g_sources[i] = copy;
		if (save() != 0) {
			g_sources[i] = before;
			snprintf(err, err_size, "could not save the source list");
			return PKGSOURCE_ERR_PERSIST;
		}
	}
	return PKGSOURCE_OK;
}

static void offers_path(const char *source, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s.offers", g_offers_dir, source);
}

enum pkgsource_error pkgsource_remove(const char *name, char *err, size_t err_size)
{
	int i = index_of(name);
	int j;
	char path[PATH_MAX];

	err[0] = '\0';
	if (i < 0) {
		snprintf(err, err_size, "no source named %s", name != NULL ? name : "");
		return PKGSOURCE_ERR_NOT_FOUND;
	}
	offers_path(g_sources[i].name, path, sizeof(path));
	for (j = 0; j < g_choice_count;) {
		if (strcmp(g_choices[j].source, g_sources[i].name) == 0)
			g_choices[j] = g_choices[--g_choice_count];
		else
			j++;
	}
	for (j = i; j < g_count - 1; j++)
		g_sources[j] = g_sources[j + 1];
	g_count--;
	if (save() != 0) {
		snprintf(err, err_size, "could not save the source list");
		return PKGSOURCE_ERR_PERSIST;
	}
	unlink(path);
	pkgsource_offers_load();
	return PKGSOURCE_OK;
}

void pkgsource_write_json_list(struct json_writer *w)
{
	int i;

	jw_arr_open(w);
	for (i = 0; i < g_count; i++) {
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, g_sources[i].name);
		jw_key(w, "url");
		jw_str(w, g_sources[i].url);
		jw_key(w, "kind");
		jw_str(w, g_sources[i].kind);
		jw_key(w, "ref");
		jw_str(w, g_sources[i].ref);
		jw_key(w, "token_set");
		jw_bool(w, g_sources[i].token[0] != '\0');
		jw_key(w, "write");
		jw_bool(w, g_sources[i].write);
		jw_key(w, "trust_keys");
		jw_bool(w, g_sources[i].trust_keys);
		jw_obj_close(w);
	}
	jw_arr_close(w);
}

void pkgsource_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "sources");
	pkgsource_write_json_list(w);
	jw_obj_close(w);
}

/* ---- offers ---- */

int pkgsource_offers_write(const char *source, const char *const *items, int count)
{
	char path[PATH_MAX];
	size_t total = 1, off = 0;
	char *buf;
	int i, rc;

	if (index_of(source) < 0 || count < 0)
		return -1;
	for (i = 0; i < count; i++)
		total += strlen(items[i]) + 1;
	buf = malloc(total);
	if (buf == NULL)
		return -1;
	for (i = 0; i < count; i++)
		off += (size_t)sprintf(buf + off, "%s\n", items[i]);
	buf[off] = '\0';
	if (persist_mkdir_p(g_offers_dir) != 0) {
		free(buf);
		return -1;
	}
	offers_path(source, path, sizeof(path));
	rc = persist_atomic_write(path, buf, off);
	free(buf);
	return rc;
}

int pkgsource_offers_add(const char *source, const char *item)
{
	char path[PATH_MAX];
	char *buf = NULL, *line, *save_ptr = NULL;
	size_t len = 0;
	int found = 0, rc;

	if (index_of(source) < 0 || item == NULL || item[0] == '\0' ||
	    strlen(item) >= PKG_SOURCE_ITEM_MAX)
		return -1;
	offers_path(source, path, sizeof(path));
	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		char *copy = strdup(buf);

		for (line = copy != NULL ? strtok_r(copy, "\n", &save_ptr) : NULL;
		     line != NULL && !found; line = strtok_r(NULL, "\n", &save_ptr))
			found = strcmp(line, item) == 0;
		free(copy);
	}
	if (found) {
		free(buf);
		return 0;
	}
	{
		size_t n = len + strlen(item) + 2;
		char *out = malloc(n);

		if (out == NULL) {
			free(buf);
			return -1;
		}
		snprintf(out, n, "%.*s%s\n", (int)len, buf != NULL ? buf : "", item);
		free(buf);
		if (persist_mkdir_p(g_offers_dir) != 0) {
			free(out);
			return -1;
		}
		rc = persist_atomic_write(path, out, strlen(out));
		free(out);
	}
	if (rc == 0)
		rc = pkgsource_offers_load();
	return rc;
}

static int offer_cmp(const void *a, const void *b)
{
	return strcmp(((const struct offer *)a)->item, ((const struct offer *)b)->item);
}

static struct offer *offer_find(const char *item)
{
	struct offer key;

	if (g_offer_count == 0)
		return NULL;
	snprintf(key.item, sizeof(key.item), "%s", item);
	return bsearch(&key, g_offers, (size_t)g_offer_count, sizeof(*g_offers), offer_cmp);
}

/* Appended unsorted while loading; offers_load() sorts and merges. */
static int offer_append(const char *item, int source_index)
{
	if (g_offer_count == g_offer_cap) {
		int cap = g_offer_cap == 0 ? 512 : g_offer_cap * 2;
		struct offer *n = realloc(g_offers, (size_t)cap * sizeof(*n));

		if (n == NULL)
			return -1;
		g_offers = n;
		g_offer_cap = cap;
	}
	snprintf(g_offers[g_offer_count].item, sizeof(g_offers[g_offer_count].item), "%s", item);
	g_offers[g_offer_count].by = (uint32_t)1 << source_index;
	g_offer_count++;
	return 0;
}

int pkgsource_offers_load(void)
{
	int i, j, k;

	g_offer_count = 0;
	for (i = 0; i < g_count; i++) {
		char path[PATH_MAX];
		char *buf = NULL, *line, *save_ptr = NULL;
		size_t len = 0;

		offers_path(g_sources[i].name, path, sizeof(path));
		if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
			continue;
		for (line = strtok_r(buf, "\n", &save_ptr); line != NULL;
		     line = strtok_r(NULL, "\n", &save_ptr))
			if (line[0] != '\0' && strlen(line) < PKG_SOURCE_ITEM_MAX &&
			    offer_append(line, i) != 0) {
				free(buf);
				return -1;
			}
		free(buf);
	}
	if (g_offer_count == 0)
		return 0;
	qsort(g_offers, (size_t)g_offer_count, sizeof(*g_offers), offer_cmp);
	/* One entry per item, its sources OR'd together. */
	for (j = 0, k = 1; k < g_offer_count; k++) {
		if (strcmp(g_offers[j].item, g_offers[k].item) == 0)
			g_offers[j].by |= g_offers[k].by;
		else
			g_offers[++j] = g_offers[k];
	}
	g_offer_count = j + 1;
	return 0;
}

/* ---- choices and ownership ---- */

static int choice_index(const char *item)
{
	int i;

	for (i = 0; i < g_choice_count; i++)
		if (strcmp(g_choices[i].item, item) == 0)
			return i;
	return -1;
}

static int choice_add(const char *item, const char *source)
{
	int i = choice_index(item);

	if (i < 0) {
		if (g_choice_count == g_choice_cap) {
			int cap = g_choice_cap == 0 ? 32 : g_choice_cap * 2;
			struct choice *n = realloc(g_choices, (size_t)cap * sizeof(*n));

			if (n == NULL)
				return -1;
			g_choices = n;
			g_choice_cap = cap;
		}
		i = g_choice_count++;
		snprintf(g_choices[i].item, sizeof(g_choices[i].item), "%s", item);
	}
	snprintf(g_choices[i].source, sizeof(g_choices[i].source), "%s", source);
	return 0;
}

static void names_of(uint32_t by, char *out, size_t out_size)
{
	size_t off = 0;
	int i;

	out[0] = '\0';
	for (i = 0; i < g_count && off < out_size; i++)
		if (by & ((uint32_t)1 << i))
			off += (size_t)snprintf(out + off, out_size - off, "%s%s", off > 0 ? ", " : "",
			                        g_sources[i].name);
}

static int popcount32(uint32_t v)
{
	int n = 0;

	for (; v != 0; v &= v - 1)
		n++;
	return n;
}

/*
 * The operator's choice decides whenever it names a source this host
 * has: among several sources offering the name, for a name no source
 * offers yet (a recipe published here before its source's next sync
 * carries it), and over a single other source that later offers it --
 * ownership never moves on its own. Without a choice, the one source
 * that offers a name owns it, and several are a conflict.
 */
enum pkgsource_owner pkgsource_owner_of(const char *item, char *out, size_t out_size)
{
	const struct offer *o = offer_find(item);
	int c = choice_index(item);

	if (out_size > 0)
		out[0] = '\0';
	if (c >= 0 && index_of(g_choices[c].source) >= 0) {
		snprintf(out, out_size, "%s", g_choices[c].source);
		return PKGSOURCE_OWNER_ONE;
	}
	if (o == NULL || o->by == 0)
		return PKGSOURCE_OWNER_NONE;
	if (popcount32(o->by) == 1) {
		int i;

		for (i = 0; i < g_count; i++)
			if (o->by & ((uint32_t)1 << i))
				snprintf(out, out_size, "%s", g_sources[i].name);
		return PKGSOURCE_OWNER_ONE;
	}
	names_of(o->by, out, out_size);
	return PKGSOURCE_OWNER_CONFLICT;
}

int pkgsource_resolve(const char *item, const char *requested, int writable_only, char *out,
                      size_t out_size, int *choose, char *err, size_t err_size)
{
	char owner[PKG_SOURCES_MAX * PKG_SOURCE_NAME_MAX];
	const char *name = strchr(item, ':') != NULL ? strchr(item, ':') + 1 : item;
	int kind_len = (int)(name - item) > 0 ? (int)(name - item) - 1 : 0;
	int have = requested != NULL && requested[0] != '\0';
	int i, found = -1, candidates = 0;

	*choose = 0;
	out[0] = '\0';
	err[0] = '\0';
	switch (pkgsource_owner_of(item, owner, sizeof(owner))) {
	case PKGSOURCE_OWNER_ONE:
		if (have && strcmp(requested, owner) != 0) {
			snprintf(err, err_size, "%.*s %s belongs to source %s, not %s", kind_len, item,
			         name, owner, requested);
			return -1;
		}
		snprintf(out, out_size, "%s", owner);
		return 0;
	case PKGSOURCE_OWNER_CONFLICT:
		if (!have) {
			snprintf(err, err_size,
			         "%.*s %s is offered by %s and no source has been chosen for it -- name "
			         "one, or choose one first (PUT /v1/pkg/source-ownership)",
			         kind_len, item, name, owner);
			return -1;
		}
		break;
	default:
		break;
	}
	if (have) {
		if (pkgsource_find(requested) == NULL) {
			snprintf(err, err_size, "no source named %s", requested);
			return -1;
		}
		snprintf(out, out_size, "%s", requested);
		*choose = 1;
		return 0;
	}
	/* Nobody owns it and the caller named no source. */
	for (i = 0; i < g_count; i++)
		if (!writable_only || g_sources[i].write) {
			found = i;
			candidates++;
		}
	if (candidates == 1) {
		snprintf(out, out_size, "%s", g_sources[found].name);
		*choose = 1;
		return 0;
	}
	if (candidates == 0 && !writable_only)
		return 0; /* a host with no source: the recipe is this host's alone */
	if (candidates == 0)
		snprintf(err, err_size,
		         "no source is writable on this host -- mark one with "
		         "`cixctl pkg source set NAME --write=on` (ADR-0324)");
	else
		snprintf(err, err_size, "%.*s %s is new and %d sources are %s -- name the one it "
		         "belongs to",
		         kind_len, item, name, candidates, writable_only ? "writable" : "configured");
	return -1;
}

enum pkgsource_error pkgsource_choose(const char *item, const char *source, char *err,
                                      size_t err_size)
{
	int c;

	err[0] = '\0';
	if (item == NULL || item[0] == '\0' || strchr(item, ':') == NULL ||
	    strlen(item) >= PKG_SOURCE_ITEM_MAX) {
		snprintf(err, err_size, "an item is <kind>:<name>, kind package, image or deployment");
		return PKGSOURCE_ERR_INVALID;
	}
	if (strncmp(item, "package:", 8) != 0 && strncmp(item, "image:", 6) != 0 &&
	    strncmp(item, "deployment:", 11) != 0) {
		snprintf(err, err_size, "an item is <kind>:<name>, kind package, image or deployment");
		return PKGSOURCE_ERR_INVALID;
	}
	if (source == NULL || source[0] == '\0') {
		c = choice_index(item);
		if (c >= 0)
			g_choices[c] = g_choices[--g_choice_count];
	} else {
		if (pkgsource_find(source) == NULL) {
			snprintf(err, err_size, "no source named %s", source);
			return PKGSOURCE_ERR_NOT_FOUND;
		}
		if (choice_add(item, source) != 0) {
			snprintf(err, err_size, "out of memory");
			return PKGSOURCE_ERR_PERSIST;
		}
	}
	if (save() != 0) {
		snprintf(err, err_size, "could not save the source list");
		return PKGSOURCE_ERR_PERSIST;
	}
	return PKGSOURCE_OK;
}

void pkgsource_write_ownership_json(struct json_writer *w)
{
	char names[PKG_SOURCES_MAX * PKG_SOURCE_NAME_MAX];
	int i;

	jw_obj_open(w);
	jw_key(w, "conflicts");
	jw_arr_open(w);
	for (i = 0; i < g_offer_count; i++) {
		char owner[PKG_SOURCE_NAME_MAX];

		if (popcount32(g_offers[i].by) < 2 ||
		    pkgsource_owner_of(g_offers[i].item, owner, sizeof(owner)) !=
		        PKGSOURCE_OWNER_CONFLICT)
			continue;
		names_of(g_offers[i].by, names, sizeof(names));
		jw_obj_open(w);
		jw_key(w, "item");
		jw_str(w, g_offers[i].item);
		jw_key(w, "offered_by");
		jw_str(w, names);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_key(w, "choices");
	jw_arr_open(w);
	for (i = 0; i < g_choice_count; i++) {
		jw_obj_open(w);
		jw_key(w, "item");
		jw_str(w, g_choices[i].item);
		jw_key(w, "source");
		jw_str(w, g_choices[i].source);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}
