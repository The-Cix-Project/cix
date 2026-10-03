#include "upstreamkeys.h"

#include "persist.h"
#include "pgpverify.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct upstream_key {
	char package[64];
	char fingerprint[41];
	char *armored; /* owned */
	size_t len;
	long long added_at;
	char source[64]; /* the source whose catalogue supplied it; "" for an operator's */
};

static char g_path[PATH_MAX];
static struct upstream_key g_keys[UPSTREAMKEYS_MAX];
static int g_count;

int upstreamkeys_normalise(const char *in, char out[41])
{
	size_t n = 0;

	if (in == NULL)
		return -1;
	for (; *in != '\0'; in++) {
		if (*in == ' ')
			continue;
		if (!isxdigit((unsigned char)*in) || n >= 40)
			return -1;
		out[n++] = (char)toupper((unsigned char)*in);
	}
	out[n] = '\0';
	return n == 40 ? 0 : -1;
}

static int find(const char *package, const char *fingerprint)
{
	char fpr[41];
	int i;

	if (package == NULL || upstreamkeys_normalise(fingerprint, fpr) != 0)
		return -1;
	for (i = 0; i < g_count; i++)
		if (strcmp(g_keys[i].package, package) == 0 && strcmp(g_keys[i].fingerprint, fpr) == 0)
			return i;
	return -1;
}

static int save(void)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "keys");
	jw_arr_open(&w);
	for (i = 0; i < g_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "package");
		jw_str(&w, g_keys[i].package);
		jw_key(&w, "fingerprint");
		jw_str(&w, g_keys[i].fingerprint);
		jw_key(&w, "key");
		jw_str(&w, g_keys[i].armored);
		jw_key(&w, "added_at");
		jw_int(&w, g_keys[i].added_at);
		if (g_keys[i].source[0] != '\0') {
			jw_key(&w, "source");
			jw_str(&w, g_keys[i].source);
		}
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

static void clear_all(void)
{
	int i;

	for (i = 0; i < g_count; i++)
		free(g_keys[i].armored);
	g_count = 0;
}

int upstreamkeys_init(const char *path)
{
	char *buf = NULL;
	size_t len = 0, i;
	struct json_value *root;
	const struct json_value *arr;

	if (path == NULL || (size_t)snprintf(g_path, sizeof(g_path), "%s", path) >= sizeof(g_path))
		return -1;
	clear_all();
	if (persist_read_file(g_path, &buf, &len) != 0 || buf == NULL)
		return 0;
	root = json_parse(buf, len);
	free(buf);
	arr = root != NULL ? json_object_get(root, "keys") : NULL;
	for (i = 0; arr != NULL && arr->type == JSON_ARRAY && i < arr->u.array.count &&
	            g_count < UPSTREAMKEYS_MAX;
	     i++) {
		const struct json_value *o = arr->u.array.items[i];
		const char *p = json_as_string(json_object_get(o, "package"));
		const char *f = json_as_string(json_object_get(o, "fingerprint"));
		const char *k = json_as_string(json_object_get(o, "key"));
		struct upstream_key *e = &g_keys[g_count];

		if (p == NULL || k == NULL || strlen(p) >= sizeof(e->package) ||
		    upstreamkeys_normalise(f, e->fingerprint) != 0)
			continue;
		e->armored = strdup(k);
		if (e->armored == NULL)
			continue;
		snprintf(e->package, sizeof(e->package), "%s", p);
		e->len = strlen(k);
		e->added_at = (long long)json_as_number(json_object_get(o, "added_at"));
		{
			const char *src = json_as_string(json_object_get(o, "source"));

			snprintf(e->source, sizeof(e->source), "%s", src != NULL ? src : "");
		}
		g_count++;
	}
	json_free(root);
	return 0;
}

/*
 * Validates `armored` against the pinned fingerprint and stores it, for
 * `source` ("" for an operator). Does not save. 0, or -1 with err.
 */
static int put_key(const char *package, const char *pinned, const char *armored,
                   size_t armored_len, long long now, const char *source, char *err,
                   size_t err_size)
{
	char actual[PGP_FINGERPRINT_HEX_MAX], why[256];
	char *copy;
	int i;

	if (package == NULL || package[0] == '\0' || strlen(package) >= sizeof(g_keys[0].package)) {
		snprintf(err, err_size, "invalid package name");
		return -1;
	}
	if (armored == NULL || armored_len == 0 || armored_len > UPSTREAMKEYS_KEY_MAX) {
		snprintf(err, err_size, "key must be an ASCII-armored public key of at most %d bytes",
		         UPSTREAMKEYS_KEY_MAX);
		return -1;
	}
	why[0] = '\0';
	if (pgp_key_fingerprint(armored, armored_len, actual, why, sizeof(why)) != PGP_VERIFY_OK) {
		snprintf(err, err_size, "the key cannot be used to verify a release: %s", why);
		return -1;
	}
	if (strcmp(actual, pinned) != 0) {
		snprintf(err, err_size, "the key's fingerprint is %s, not the pinned %s -- nothing added",
		         actual, pinned);
		return -1;
	}
	copy = malloc(armored_len + 1);
	if (copy == NULL) {
		snprintf(err, err_size, "out of memory");
		return -1;
	}
	memcpy(copy, armored, armored_len);
	copy[armored_len] = '\0';

	i = find(package, pinned);
	if (i < 0) {
		if (g_count >= UPSTREAMKEYS_MAX) {
			free(copy);
			snprintf(err, err_size, "the store holds %d keys already", UPSTREAMKEYS_MAX);
			return -1;
		}
		i = g_count++;
		snprintf(g_keys[i].package, sizeof(g_keys[i].package), "%s", package);
		snprintf(g_keys[i].fingerprint, sizeof(g_keys[i].fingerprint), "%s", pinned);
	} else {
		free(g_keys[i].armored);
	}
	g_keys[i].armored = copy;
	g_keys[i].len = armored_len;
	g_keys[i].added_at = now;
	snprintf(g_keys[i].source, sizeof(g_keys[i].source), "%s", source);
	return 0;
}

static void remove_at(int i)
{
	free(g_keys[i].armored);
	memmove(&g_keys[i], &g_keys[i + 1], sizeof(g_keys[0]) * (size_t)(g_count - i - 1));
	g_count--;
}

int upstreamkeys_add(const char *package, const char *fingerprint, const char *armored,
                     size_t armored_len, long long now, char *err, size_t err_size)
{
	char pinned[41];
	int i;

	if (upstreamkeys_normalise(fingerprint, pinned) != 0) {
		snprintf(err, err_size, "fingerprint must be 40 hexadecimal digits (a v4 key)");
		return -1;
	}
	i = find(package, pinned);
	if (i >= 0 && g_keys[i].source[0] != '\0') {
		snprintf(err, err_size,
		         "source %s's catalogue supplies this key -- change it there (ADR-0326)",
		         g_keys[i].source);
		return -1;
	}
	if (put_key(package, pinned, armored, armored_len, now, "", err, err_size) != 0)
		return -1;
	if (save() != 0) {
		snprintf(err, err_size, "the key store could not be saved");
		return -1;
	}
	return 0;
}

int upstreamkeys_remove(const char *package, const char *fingerprint)
{
	int i = find(package, fingerprint);

	if (i < 0)
		return 1;
	if (g_keys[i].source[0] != '\0')
		return 2;
	remove_at(i);
	return save() == 0 ? 0 : -1;
}

int upstreamkeys_sync_source(const char *source, const struct upstreamkeys_offer *offers, int n,
                             long long now, int *refused, char *why, size_t why_size)
{
	char err[512];
	int i, adopted = 0;

	*refused = 0;
	if (why_size > 0)
		why[0] = '\0';
	if (source == NULL || source[0] == '\0')
		return -1;
	/* The helper's copy may predate an operator's change; the file is
	 * the store. */
	{
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s", g_path);
		if (path[0] != '\0')
			upstreamkeys_init(path);
	}
	for (i = 0; i < g_count;) {
		if (strcmp(g_keys[i].source, source) == 0)
			remove_at(i);
		else
			i++;
	}
	for (i = 0; i < n; i++) {
		char pinned[41];

		if (upstreamkeys_normalise(offers[i].fingerprint, pinned) != 0) {
			snprintf(err, sizeof(err), "%s: \"%s\" is not a 40-digit fingerprint",
			         offers[i].package, offers[i].fingerprint);
		} else if (put_key(offers[i].package, pinned, offers[i].armored, offers[i].len, now,
		                   source, err, sizeof(err)) == 0) {
			adopted++;
			continue;
		}
		if (*refused == 0 && why_size > 0)
			snprintf(why, why_size, "%s", err);
		(*refused)++;
	}
	return save() == 0 ? adopted : -1;
}

int upstreamkeys_retain_sources(const char *const *names, int n)
{
	int i, j, changed = 0;

	for (i = 0; i < g_count;) {
		int kept = g_keys[i].source[0] == '\0';

		for (j = 0; !kept && j < n; j++)
			kept = strcmp(g_keys[i].source, names[j]) == 0;
		if (kept) {
			i++;
			continue;
		}
		remove_at(i);
		changed = 1;
	}
	return changed && save() != 0 ? -1 : 0;
}

const char *upstreamkeys_find(const char *package, const char *fingerprint, size_t *len)
{
	int i = find(package, fingerprint);

	if (i < 0)
		return NULL;
	if (len != NULL)
		*len = g_keys[i].len;
	return g_keys[i].armored;
}

void upstreamkeys_write_json(const char *package, struct json_writer *w)
{
	int i;

	jw_obj_open(w);
	jw_key(w, "package");
	jw_str(w, package);
	jw_key(w, "keys");
	jw_arr_open(w);
	for (i = 0; i < g_count; i++) {
		if (strcmp(g_keys[i].package, package) != 0)
			continue;
		jw_obj_open(w);
		jw_key(w, "fingerprint");
		jw_str(w, g_keys[i].fingerprint);
		jw_key(w, "bytes");
		jw_int(w, (long long)g_keys[i].len);
		jw_key(w, "added_at");
		jw_int(w, g_keys[i].added_at);
		jw_key(w, "source");
		if (g_keys[i].source[0] != '\0')
			jw_str(w, g_keys[i].source);
		else
			jw_null(w);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}
