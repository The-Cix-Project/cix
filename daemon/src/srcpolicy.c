/*
 * See srcpolicy.h for why this is a different axis from pkgpolicy.h
 * even though both use the word "pinned".
 */
#include "srcpolicy.h"
#include "srcdepth.h"
#include "srcupstream.h"
#include "persist.h"
#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRCPOLICY_MAX 256
#define SRCPOLICY_DEFAULT_DEPTH "n"

struct srcpolicy_entry {
	char name[PKG_NAME_MAX];
	char channel[SRCPOLICY_CHANNEL_MAX];
	char depth[SRCPOLICY_DEPTH_MAX];
	int in_use;
};

static struct srcpolicy_entry g_entries[SRCPOLICY_MAX];
static char g_default_channel[SRCPOLICY_CHANNEL_MAX];
static char g_default_depth[SRCPOLICY_DEPTH_MAX] = SRCPOLICY_DEFAULT_DEPTH;
static char g_state_path[512];

static struct srcpolicy_entry *find(const char *name)
{
	int i;

	if (name == NULL)
		return NULL;
	for (i = 0; i < SRCPOLICY_MAX; i++)
		if (g_entries[i].in_use && strcmp(g_entries[i].name, name) == 0)
			return &g_entries[i];
	return NULL;
}

static int save_state(void)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "default");
	jw_obj_open(&w);
	jw_key(&w, "channel");
	if (g_default_channel[0] != '\0')
		jw_str(&w, g_default_channel);
	else
		jw_null(&w);
	jw_key(&w, "depth");
	jw_str(&w, g_default_depth);
	jw_obj_close(&w);
	jw_key(&w, "packages");
	jw_arr_open(&w);
	for (i = 0; i < SRCPOLICY_MAX; i++) {
		if (!g_entries[i].in_use)
			continue;
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, g_entries[i].name);
		jw_key(&w, "channel");
		if (g_entries[i].channel[0] != '\0')
			jw_str(&w, g_entries[i].channel);
		else
			jw_null(&w);
		jw_key(&w, "depth");
		jw_str(&w, g_entries[i].depth);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

void srcpolicy_repoint(const char *path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
}

int srcpolicy_init(const char *path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *def;
	const struct json_value *arr;
	size_t i;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	memset(g_entries, 0, sizeof(g_entries));
	g_default_channel[0] = '\0';
	snprintf(g_default_depth, sizeof(g_default_depth), "%s", SRCPOLICY_DEFAULT_DEPTH);

	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return 0; /* never configured -- everything on the default */
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		/* A stale-but-true default beats refusing to boot; same posture
		 * pkgpolicy_init() takes for the same reason. */
		fprintf(stderr, "%s: malformed source policies, ignoring\n", path);
		return 0;
	}
	def = json_object_get(root, "default");
	if (def != NULL) {
		const char *c = json_as_string(json_object_get(def, "channel"));
		const char *d = json_as_string(json_object_get(def, "depth"));

		if (c != NULL)
			snprintf(g_default_channel, sizeof(g_default_channel), "%s", c);
		if (d != NULL && srcdepth_parse(d, NULL, NULL) == SRCDEPTH_OK)
			snprintf(g_default_depth, sizeof(g_default_depth), "%s", d);
	}
	arr = json_object_get(root, "packages");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		int slot = 0;

		for (i = 0; i < arr->u.array.count && slot < SRCPOLICY_MAX; i++) {
			const char *name = json_as_string(json_object_get(arr->u.array.items[i], "name"));
			const char *c = json_as_string(json_object_get(arr->u.array.items[i], "channel"));
			const char *d = json_as_string(json_object_get(arr->u.array.items[i], "depth"));

			if (name == NULL || name[0] == '\0')
				continue;
			snprintf(g_entries[slot].name, sizeof(g_entries[slot].name), "%s", name);
			if (c != NULL)
				snprintf(g_entries[slot].channel, sizeof(g_entries[slot].channel), "%s", c);
			snprintf(g_entries[slot].depth, sizeof(g_entries[slot].depth), "%s",
			          (d != NULL && srcdepth_parse(d, NULL, NULL) == SRCDEPTH_OK)
			              ? d
			              : SRCPOLICY_DEFAULT_DEPTH);
			g_entries[slot].in_use = 1;
			slot++;
		}
	}
	json_free(root);
	return 0;
}

void srcpolicy_default_get(struct srcpolicy *out)
{
	if (out == NULL)
		return;
	memset(out, 0, sizeof(*out));
	snprintf(out->channel, sizeof(out->channel), "%s", g_default_channel);
	snprintf(out->depth, sizeof(out->depth), "%s", g_default_depth);
}

int srcpolicy_default_set(const char *channel, const char *depth, char *err, size_t err_size)
{
	char c[SRCPOLICY_CHANNEL_MAX];
	char d[SRCPOLICY_DEPTH_MAX];

	if (err != NULL && err_size > 0)
		err[0] = '\0';
	snprintf(c, sizeof(c), "%s", channel != NULL ? channel : "");
	snprintf(d, sizeof(d), "%s", (depth != NULL && depth[0] != '\0') ? depth
	                                                                  : SRCPOLICY_DEFAULT_DEPTH);
	if (srcdepth_parse(d, NULL, NULL) != SRCDEPTH_OK) {
		if (err != NULL)
			snprintf(err, err_size, "depth \"%s\" is not n, n-<lines> or n-<lines>.<releases>", d);
		return -1;
	}
	/*
	 * The default channel is NOT validated against any kind, on
	 * purpose: it is a preference applied wherever it fits, and there
	 * is no single kind to check it against. Validation happens where
	 * it is actually used -- srcpolicy_effective_for_kind() -- which is
	 * also the only place that can say which package it failed for.
	 */
	snprintf(g_default_channel, sizeof(g_default_channel), "%s", c);
	snprintf(g_default_depth, sizeof(g_default_depth), "%s", d);
	return save_state();
}

void srcpolicy_get(const char *name, struct srcpolicy *out)
{
	struct srcpolicy_entry *e;

	if (out == NULL)
		return;
	srcpolicy_default_get(out);
	e = find(name);
	if (e == NULL)
		return;
	snprintf(out->channel, sizeof(out->channel), "%s", e->channel);
	snprintf(out->depth, sizeof(out->depth), "%s", e->depth);
	out->explicit_entry = 1;
}

int srcpolicy_set(const char *name, const char *kind_name, const char *channel,
                   const char *depth, char *err, size_t err_size)
{
	struct srcpolicy_entry *e;
	const char *d = (depth != NULL && depth[0] != '\0') ? depth : SRCPOLICY_DEFAULT_DEPTH;
	const char *c = channel != NULL ? channel : "";
	int i;

	if (err != NULL && err_size > 0)
		err[0] = '\0';
	if (name == NULL || name[0] == '\0') {
		if (err != NULL)
			snprintf(err, err_size, "package name is required");
		return -1;
	}
	if (srcdepth_parse(d, NULL, NULL) != SRCDEPTH_OK) {
		if (err != NULL)
			snprintf(err, err_size, "depth \"%s\" is not n, n-<lines> or n-<lines>.<releases>", d);
		return -1;
	}
	/*
	 * Validate the channel against the recipe's own kind here, at
	 * configuration time, rather than letting it surface later as an
	 * empty resolution. "glibc has no channels" and "nothing found" are
	 * different sentences and only one of them names the mistake.
	 */
	if (kind_name != NULL && kind_name[0] != '\0') {
		if (srcupstream_check(kind_name, c, err, err_size) != SRCUPSTREAM_OK)
			return -1;
	} else if (c[0] != '\0') {
		if (err != NULL)
			snprintf(err, err_size,
			          "package \"%s\" declares no pkg_upstream, so it has no channels to choose "
			          "from; it is pinned",
			          name);
		return -1;
	}

	e = find(name);
	if (e == NULL) {
		for (i = 0; i < SRCPOLICY_MAX; i++) {
			if (!g_entries[i].in_use) {
				e = &g_entries[i];
				break;
			}
		}
		if (e == NULL) {
			if (err != NULL)
				snprintf(err, err_size, "too many package source policies (max %d)",
				          SRCPOLICY_MAX);
			return -1;
		}
		memset(e, 0, sizeof(*e));
		snprintf(e->name, sizeof(e->name), "%s", name);
		e->in_use = 1;
	}
	snprintf(e->channel, sizeof(e->channel), "%s", c);
	snprintf(e->depth, sizeof(e->depth), "%s", d);
	return save_state();
}

int srcpolicy_clear(const char *name)
{
	struct srcpolicy_entry *e = find(name);

	if (e == NULL)
		return 0;
	memset(e, 0, sizeof(*e));
	return save_state();
}

int srcpolicy_effective_for_kind(const char *name, const char *kind_name,
                                  struct srcpolicy *out, char *err, size_t err_size)
{
	const struct srcupstream_kind *kind;
	struct srcpolicy p;

	if (err != NULL && err_size > 0)
		err[0] = '\0';
	srcpolicy_get(name, &p);
	if (out != NULL)
		*out = p;

	if (kind_name == NULL || kind_name[0] == '\0') {
		if (err != NULL)
			snprintf(err, err_size,
			          "package \"%s\" declares no pkg_upstream, so it cannot roll; it is pinned",
			          name);
		return -1;
	}
	kind = srcupstream_find(kind_name);
	if (kind == NULL) {
		if (err != NULL)
			snprintf(err, err_size, "package \"%s\" declares unknown upstream kind \"%s\"", name,
			          kind_name);
		return -1;
	}

	/*
	 * A DEFAULT CHANNEL IS A PREFERENCE, NOT A MANDATE.
	 *
	 * If the package has no entry of its own and the global default
	 * names a channel this kind does not publish, that is not an error
	 * in the default -- the default is right for the packages it fits.
	 * It simply does not apply here, so it is dropped and the kind is
	 * asked again with nothing chosen. A kind with no channels then
	 * succeeds (correct: there was never anything to choose), and one
	 * with channels reports that it needs an explicit choice for THIS
	 * package -- which is the actionable sentence.
	 */
	if (!p.explicit_entry && p.channel[0] != '\0' &&
	    srcupstream_check_channel(kind, p.channel, NULL, 0) != SRCUPSTREAM_OK) {
		p.channel[0] = '\0';
		if (out != NULL)
			out->channel[0] = '\0';
	}

	if (srcupstream_check_channel(kind, p.channel, err, err_size) != SRCUPSTREAM_OK)
		return -1;
	if (srcdepth_parse(p.depth, NULL, NULL) != SRCDEPTH_OK) {
		if (err != NULL)
			snprintf(err, err_size, "package \"%s\" has depth \"%s\", which is not valid", name,
			          p.depth);
		return -1;
	}
	return 0;
}

static void write_one(const char *name, const struct srcpolicy *p, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, name);
	jw_key(w, "channel");
	if (p->channel[0] != '\0')
		jw_str(w, p->channel);
	else
		jw_null(w);
	jw_key(w, "depth");
	jw_str(w, p->depth);
	jw_key(w, "inherited");
	jw_bool(w, !p->explicit_entry);
	jw_obj_close(w);
}

void srcpolicy_write_json_one(const char *name, struct json_writer *w)
{
	struct srcpolicy p;

	srcpolicy_get(name, &p);
	write_one(name, &p, w);
}

void srcpolicy_write_json(struct json_writer *w)
{
	struct srcpolicy d;
	int i;

	srcpolicy_default_get(&d);
	jw_obj_open(w);
	jw_key(w, "default");
	jw_obj_open(w);
	jw_key(w, "channel");
	if (d.channel[0] != '\0')
		jw_str(w, d.channel);
	else
		jw_null(w);
	jw_key(w, "depth");
	jw_str(w, d.depth);
	jw_obj_close(w);
	jw_key(w, "packages");
	jw_arr_open(w);
	for (i = 0; i < SRCPOLICY_MAX; i++) {
		struct srcpolicy p;

		if (!g_entries[i].in_use)
			continue;
		memset(&p, 0, sizeof(p));
		snprintf(p.channel, sizeof(p.channel), "%s", g_entries[i].channel);
		snprintf(p.depth, sizeof(p.depth), "%s", g_entries[i].depth);
		p.explicit_entry = 1;
		write_one(g_entries[i].name, &p, w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}
