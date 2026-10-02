#include "srctrust.h"

#include "persist.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_path[PATH_MAX];
static char g_origins[SRCTRUST_MAX][SRCTRUST_ORIGIN_MAX];
static int g_count;

int srctrust_origin_of(const char *url, char *out, size_t out_size)
{
	const char *authority, *end, *at;
	size_t scheme_len, host_len, i, n;

	if (url == NULL || out == NULL || out_size == 0)
		return -1;
	out[0] = '\0';
	if (strncmp(url, "https://", 8) == 0)
		scheme_len = 5;
	else if (strncmp(url, "http://", 7) == 0)
		scheme_len = 4;
	else
		return -1;
	authority = url + scheme_len + 3;
	end = authority + strcspn(authority, "/?#");
	/* Credentials are not part of an origin -- and must never reach the list. */
	at = memchr(authority, '@', (size_t)(end - authority));
	if (at != NULL)
		authority = at + 1;
	host_len = (size_t)(end - authority);
	if (host_len == 0 || scheme_len + 3 + host_len >= out_size)
		return -1;
	memcpy(out, url, scheme_len + 3);
	n = scheme_len + 3;
	for (i = 0; i < host_len; i++) {
		unsigned char c = (unsigned char)authority[i];

		if (!isalnum(c) && c != '.' && c != '-' && c != ':' && c != '[' && c != ']') {
			out[0] = '\0';
			return -1;
		}
		out[n++] = (char)tolower(c);
	}
	out[n] = '\0';
	return 0;
}

int srctrust_trusts(const char *url)
{
	char origin[SRCTRUST_ORIGIN_MAX];
	int i;

	if (srctrust_origin_of(url, origin, sizeof(origin)) != 0)
		return 0;
	for (i = 0; i < g_count; i++)
		if (strcmp(g_origins[i], origin) == 0)
			return 1;
	return 0;
}

static int save(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	srctrust_write_json(&w);
	rc = persist_atomic_write(g_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int srctrust_set(const char *const *origins, int count, char *err, size_t err_size)
{
	char next[SRCTRUST_MAX][SRCTRUST_ORIGIN_MAX];
	int i, j;

	if (count < 0 || count > SRCTRUST_MAX) {
		snprintf(err, err_size, "at most %d trusted origins", SRCTRUST_MAX);
		return -1;
	}
	for (i = 0; i < count; i++) {
		if (origins[i] == NULL ||
		    srctrust_origin_of(origins[i], next[i], sizeof(next[i])) != 0 ||
		    strcmp(next[i], origins[i]) != 0) {
			snprintf(err, err_size,
			         "\"%s\" is not an origin: scheme://host[:port], lowercase, with no "
			         "credentials and no path",
			         origins[i] != NULL ? origins[i] : "");
			return -1;
		}
		for (j = 0; j < i; j++)
			if (strcmp(next[j], next[i]) == 0) {
				snprintf(err, err_size, "\"%s\" is listed twice", origins[i]);
				return -1;
			}
	}
	memcpy(g_origins, next, sizeof(next));
	g_count = count;
	if (save() != 0) {
		snprintf(err, err_size, "could not save the trusted origins");
		return -1;
	}
	return 0;
}

int srctrust_count(void)
{
	return g_count;
}

const char *srctrust_at(int i)
{
	return i >= 0 && i < g_count ? g_origins[i] : NULL;
}

void srctrust_write_json(struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "origins");
	jw_arr_open(w);
	for (i = 0; i < g_count; i++)
		jw_str(w, g_origins[i]);
	jw_arr_close(w);
	jw_obj_close(w);
}

/* A missing or malformed file is an empty list: nothing is trusted. */
static void load(void)
{
	char *buf = NULL;
	size_t len = 0, i;
	struct json_value *root;
	const struct json_value *arr;

	g_count = 0;
	if (persist_read_file(g_path, &buf, &len) != 0 || buf == NULL)
		return;
	root = json_parse(buf, len);
	free(buf);
	arr = root != NULL ? json_object_get(root, "origins") : NULL;
	for (i = 0; arr != NULL && arr->type == JSON_ARRAY && i < arr->u.array.count &&
	            g_count < SRCTRUST_MAX;
	     i++) {
		const char *o = json_as_string(arr->u.array.items[i]);
		char canon[SRCTRUST_ORIGIN_MAX];

		if (o != NULL && srctrust_origin_of(o, canon, sizeof(canon)) == 0 &&
		    strcmp(canon, o) == 0)
			snprintf(g_origins[g_count++], SRCTRUST_ORIGIN_MAX, "%s", canon);
	}
	json_free(root);
}

int srctrust_init(const char *path)
{
	if (path == NULL || (size_t)snprintf(g_path, sizeof(g_path), "%s", path) >= sizeof(g_path))
		return -1;
	load();
	return 0;
}

void srctrust_repoint(const char *path)
{
	snprintf(g_path, sizeof(g_path), "%s", path);
}
