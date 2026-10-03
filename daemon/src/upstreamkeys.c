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
		g_count++;
	}
	json_free(root);
	return 0;
}

int upstreamkeys_add(const char *package, const char *fingerprint, const char *armored,
                     size_t armored_len, long long now, char *err, size_t err_size)
{
	char pinned[41], actual[PGP_FINGERPRINT_HEX_MAX], why[256];
	struct upstream_key *e;
	char *copy;
	int i;

	if (package == NULL || package[0] == '\0' || strlen(package) >= sizeof(e->package)) {
		snprintf(err, err_size, "invalid package name");
		return -1;
	}
	if (upstreamkeys_normalise(fingerprint, pinned) != 0) {
		snprintf(err, err_size, "fingerprint must be 40 hexadecimal digits (a v4 key)");
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
	free(g_keys[i].armored);
	memmove(&g_keys[i], &g_keys[i + 1], sizeof(g_keys[0]) * (size_t)(g_count - i - 1));
	g_count--;
	return save() == 0 ? 0 : -1;
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
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}
