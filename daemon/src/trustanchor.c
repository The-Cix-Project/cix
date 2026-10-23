/*
 * ADR-0350: an operator's trust anchors, and the combined bundle cixd's
 * fetches verify against. See daemon/include/trustanchor.h.
 */
#include "trustanchor.h"

#include "json.h"
#include "logstore.h"
#include "persist.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct trustanchor g_anchors[TRUSTANCHOR_MAX];
static int g_count;
static char g_store_path[PATH_MAX];
static char g_bundle_path[PATH_MAX];
static char g_public_path[PATH_MAX];
static int g_bundle_ok;

int trustanchor_count(void)
{
	return g_count;
}

const struct trustanchor *trustanchor_get(int i)
{
	return i >= 0 && i < g_count ? &g_anchors[i] : NULL;
}

const struct trustanchor *trustanchor_find(const char *fingerprint)
{
	int i;

	for (i = 0; fingerprint != NULL && i < g_count; i++)
		if (strcmp(g_anchors[i].info.fingerprint, fingerprint) == 0)
			return &g_anchors[i];
	return NULL;
}

const char *trustanchor_bundle(void)
{
	return g_bundle_ok ? g_bundle_path : NULL;
}

static void (*g_on_bundle)(const char *bundle);

void trustanchor_on_bundle(void (*fn)(const char *bundle))
{
	g_on_bundle = fn;
}

void trustanchor_write_json(struct json_writer *w, const struct trustanchor *a, int with_pem)
{
	jw_obj_open(w);
	jw_key(w, "fingerprint");
	jw_str(w, a->info.fingerprint);
	if (with_pem) {
		jw_key(w, "pem");
		jw_str(w, a->pem);
	}
	jw_key(w, "name");
	jw_str(w, a->info.name);
	jw_key(w, "subject");
	jw_str(w, a->info.subject);
	jw_key(w, "issuer");
	jw_str(w, a->info.issuer);
	jw_key(w, "not_after");
	jw_int(w, a->info.not_after);
	jw_key(w, "root");
	jw_bool(w, a->info.root);
	jw_key(w, "expired");
	jw_bool(w, a->info.expired);
	jw_key(w, "added_at");
	jw_int(w, a->added_at);
	jw_obj_close(w);
}

/*
 * The root's public roots, then every anchor, written by
 * write-and-rename. With no anchors there is nothing to combine, so the
 * file is removed and a fetch uses libcurl's default, exactly as before
 * ADR-0350. When the public half cannot be read the combined file is
 * not written either: a bundle of anchors alone would make every public
 * HTTPS source unreachable, which is worse than the anchors missing.
 */
static void write_bundle_file(void)
{
	char *pub = NULL, *out;
	size_t pub_len = 0, len, cap;
	int i;

	g_bundle_ok = 0;
	if (g_count == 0) {
		if (unlink(g_bundle_path) != 0 && errno != ENOENT)
			logstore_write("cixd", "warn", "trust anchors: could not remove %s: %s",
			                g_bundle_path, strerror(errno));
		return;
	}
	if (persist_read_file(g_public_path, &pub, &pub_len) != 0 || pub == NULL) {
		logstore_write("cixd", "error",
		                "trust anchors: cannot read the root's public bundle %s, so the %d "
		                "registered anchor(s) are not in force -- fetches use libcurl's default "
		                "(ADR-0350)",
		                g_public_path, g_count);
		free(pub);
		return;
	}
	cap = pub_len + 2;
	for (i = 0; i < g_count; i++)
		cap += g_anchors[i].pem_len + 512; /* the comment header below is under 400 */
	out = malloc(cap);
	if (out == NULL) {
		free(pub);
		return;
	}
	memcpy(out, pub, pub_len);
	len = pub_len;
	if (len > 0 && out[len - 1] != '\n')
		out[len++] = '\n';
	free(pub);
	for (i = 0; i < g_count; i++) {
		len += (size_t)snprintf(out + len, cap - len, "\n# Cix trust anchor %s\n# %s\n",
		                        g_anchors[i].info.fingerprint, g_anchors[i].info.subject);
		memcpy(out + len, g_anchors[i].pem, g_anchors[i].pem_len);
		len += g_anchors[i].pem_len;
		if (out[len - 1] != '\n')
			out[len++] = '\n';
	}
	if (persist_atomic_write(g_bundle_path, out, len) != 0)
		logstore_write("cixd", "error",
		                "trust anchors: could not write %s: %s -- fetches use libcurl's default, "
		                "without the registered anchors (ADR-0350)",
		                g_bundle_path, strerror(errno));
	else
		g_bundle_ok = 1;
	free(out);
}

/* The file, then whoever needs to know what is in force now. */
static void write_bundle(void)
{
	write_bundle_file();
	if (g_on_bundle != NULL)
		g_on_bundle(trustanchor_bundle());
}

static int save(void)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "anchors");
	jw_arr_open(&w);
	for (i = 0; i < g_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "pem");
		jw_str(&w, g_anchors[i].pem);
		jw_key(&w, "added_at");
		jw_int(&w, g_anchors[i].added_at);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_store_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

/* Fills the next slot from pem. The caller checks the count. */
static int load_one(const char *pem, size_t pem_len, long long added_at, char *err,
                    size_t err_size)
{
	struct trustanchor *a = &g_anchors[g_count];

	if (pem_len == 0 || pem_len >= sizeof(a->pem)) {
		snprintf(err, err_size, "the PEM is empty or longer than %d bytes",
		         TRUSTANCHOR_PEM_MAX - 1);
		return -1;
	}
	if (pkicrypto_anchor_inspect(pem, pem_len, &a->info, err, err_size) != 0)
		return -1;
	memcpy(a->pem, pem, pem_len);
	a->pem[pem_len] = '\0';
	a->pem_len = pem_len;
	a->added_at = added_at;
	return 0;
}

int trustanchor_init(const char *store_path, const char *bundle_path,
                     const char *public_bundle_path)
{
	struct json_value *root;
	const struct json_value *arr;
	char *buf = NULL;
	size_t len = 0, i;

	g_count = 0;
	g_bundle_ok = 0;
	snprintf(g_store_path, sizeof(g_store_path), "%s", store_path);
	snprintf(g_bundle_path, sizeof(g_bundle_path), "%s", bundle_path);
	snprintf(g_public_path, sizeof(g_public_path), "%s", public_bundle_path);

	if (persist_read_file(store_path, &buf, &len) != 0) {
		logstore_write("cixd", "error", "trust anchors: cannot read %s: %s", store_path,
		                strerror(errno));
		return -1;
	}
	/* An empty file is an empty store: a restore writes "" for a field
	 * whose file the backed-up host did not have. */
	if (buf != NULL && len == 0) {
		free(buf);
		buf = NULL;
	}
	if (buf != NULL) {
		root = json_parse(buf, len);
		free(buf);
		if (root == NULL) {
			logstore_write("cixd", "error", "trust anchors: %s is not valid JSON", store_path);
			return -1;
		}
		arr = json_object_get(root, "anchors");
		for (i = 0; arr != NULL && arr->type == JSON_ARRAY && i < arr->u.array.count; i++) {
			const struct json_value *e = arr->u.array.items[i];
			const char *pem = json_as_string(json_object_get(e, "pem"));
			char err[256];

			if (g_count >= TRUSTANCHOR_MAX)
				break;
			if (pem == NULL ||
			    load_one(pem, strlen(pem),
			             (long long)json_as_number(json_object_get(e, "added_at")), err,
			             sizeof(err)) != 0) {
				logstore_write("cixd", "error",
				                "trust anchors: entry %zu in %s no longer reads (%s); it is "
				                "skipped, and the next change to the store drops it",
				                i, store_path, pem == NULL ? "no pem" : err);
				continue;
			}
			g_count++;
		}
		json_free(root);
	}
	write_bundle();
	return 0;
}

enum trustanchor_result trustanchor_add(const char *pem, size_t pem_len, char *fingerprint_out,
                                        char *err, size_t err_size)
{
	struct trustanchor *a;

	if (g_count >= TRUSTANCHOR_MAX) {
		snprintf(err, err_size, "%d trust anchors are registered, the most this host keeps",
		         TRUSTANCHOR_MAX);
		return TRUSTANCHOR_FULL;
	}
	if (load_one(pem, pem_len, (long long)time(NULL), err, err_size) != 0)
		return TRUSTANCHOR_INVALID;
	a = &g_anchors[g_count];
	if (a->info.expired) {
		snprintf(err, err_size, "the certificate has expired");
		return TRUSTANCHOR_INVALID;
	}
	if (trustanchor_find(a->info.fingerprint) != NULL) {
		snprintf(err, err_size, "this CA is already registered");
		return TRUSTANCHOR_DUPLICATE;
	}
	g_count++;
	if (save() != 0) {
		g_count--;
		snprintf(err, err_size, "could not persist the store");
		return TRUSTANCHOR_PERSIST_FAILED;
	}
	snprintf(fingerprint_out, 65, "%s", a->info.fingerprint);
	write_bundle();
	return TRUSTANCHOR_OK;
}

enum trustanchor_result trustanchor_remove(const char *fingerprint)
{
	struct trustanchor saved;
	int i, at = -1;

	for (i = 0; i < g_count; i++)
		if (strcmp(g_anchors[i].info.fingerprint, fingerprint) == 0)
			at = i;
	if (at < 0)
		return TRUSTANCHOR_NOT_FOUND;
	saved = g_anchors[at];
	for (i = at; i + 1 < g_count; i++)
		g_anchors[i] = g_anchors[i + 1];
	g_count--;
	if (save() != 0) {
		for (i = g_count; i > at; i--)
			g_anchors[i] = g_anchors[i - 1];
		g_anchors[at] = saved;
		g_count++;
		return TRUSTANCHOR_PERSIST_FAILED;
	}
	write_bundle();
	return TRUSTANCHOR_OK;
}
