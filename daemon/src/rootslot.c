/*
 * ADR-0379: which cix-root version each boot slot holds. See rootslot.h.
 *
 * The file is {"a": V, "b": V, "a_cixd": C, "b_cixd": C,
 * "confirmed_slot": S, "attempt_slot": S, "attempt_version": V,
 * "attempt_cixd": C, "bad": V, "bad_cixd": C}, every field optional; the
 * _cixd fields are ADR-0380's, absent from a record written before it.
 * It is rewritten whole on every change (persist_atomic_write()), and a
 * change that alters nothing writes nothing.
 */
#include "rootslot.h"

#include "image.h"
#include "json.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_path[512];
static char g_slot_a[IMAGE_VERSION_MAX];
static char g_slot_b[IMAGE_VERSION_MAX];
static char g_cixd_a[ROOTSLOT_CIXD_MAX];
static char g_cixd_b[ROOTSLOT_CIXD_MAX];
static char g_confirmed_slot[8];
static char g_attempt_slot[8];
static char g_attempt_version[IMAGE_VERSION_MAX];
static char g_attempt_cixd[ROOTSLOT_CIXD_MAX];
static char g_bad[IMAGE_VERSION_MAX];
static char g_bad_cixd[ROOTSLOT_CIXD_MAX];
static char g_running_cixd[ROOTSLOT_CIXD_MAX];
static void (*g_on_change)(void);

/* ADR-0380: the cixd a bad mark records, which is never the running one. */
static void set_bad_cixd(const char *cixd)
{
	if (cixd == NULL || (g_running_cixd[0] != '\0' && strcmp(cixd, g_running_cixd) == 0))
		cixd = "";
	snprintf(g_bad_cixd, sizeof(g_bad_cixd), "%s", cixd);
}

static char *slot_field(const char *slot)
{
	if (slot == NULL)
		return NULL;
	if (strcmp(slot, "a") == 0)
		return g_slot_a;
	if (strcmp(slot, "b") == 0)
		return g_slot_b;
	return NULL;
}

static char *cixd_field(const char *slot)
{
	if (slot == NULL)
		return NULL;
	if (strcmp(slot, "a") == 0)
		return g_cixd_a;
	if (strcmp(slot, "b") == 0)
		return g_cixd_b;
	return NULL;
}

static void copy_field(const struct json_value *root, const char *key, char *out, size_t size)
{
	const char *s = json_as_string(json_object_get(root, key));

	snprintf(out, size, "%s", s != NULL ? s : "");
}

static void put_field(struct json_writer *w, const char *key, const char *value)
{
	if (value[0] == '\0')
		return;
	jw_key(w, key);
	jw_str(w, value);
}

static void save(void)
{
	struct json_writer w;

	if (g_path[0] == '\0')
		return;
	jw_init(&w);
	jw_obj_open(&w);
	put_field(&w, "a", g_slot_a);
	put_field(&w, "b", g_slot_b);
	put_field(&w, "a_cixd", g_cixd_a);
	put_field(&w, "b_cixd", g_cixd_b);
	put_field(&w, "confirmed_slot", g_confirmed_slot);
	put_field(&w, "attempt_slot", g_attempt_slot);
	put_field(&w, "attempt_version", g_attempt_version);
	put_field(&w, "attempt_cixd", g_attempt_cixd);
	put_field(&w, "bad", g_bad);
	put_field(&w, "bad_cixd", g_bad_cixd);
	jw_obj_close(&w);
	if (w.buf != NULL && persist_atomic_write(g_path, w.buf, w.len) != 0)
		fprintf(stderr, "rootslot: could not write %s\n", g_path);
	jw_free(&w);
	if (g_on_change != NULL)
		g_on_change();
}

void rootslot_load(const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	snprintf(g_path, sizeof(g_path), "%s", path != NULL ? path : "");
	g_slot_a[0] = g_slot_b[0] = g_cixd_a[0] = g_cixd_b[0] = g_confirmed_slot[0] = '\0';
	g_attempt_slot[0] = g_attempt_version[0] = g_attempt_cixd[0] = '\0';
	g_bad[0] = g_bad_cixd[0] = '\0';
	if (g_path[0] == '\0' || persist_read_file(g_path, &buf, &len) != 0 || buf == NULL)
		return;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return;
	copy_field(root, "a", g_slot_a, sizeof(g_slot_a));
	copy_field(root, "b", g_slot_b, sizeof(g_slot_b));
	copy_field(root, "a_cixd", g_cixd_a, sizeof(g_cixd_a));
	copy_field(root, "b_cixd", g_cixd_b, sizeof(g_cixd_b));
	copy_field(root, "confirmed_slot", g_confirmed_slot, sizeof(g_confirmed_slot));
	copy_field(root, "attempt_slot", g_attempt_slot, sizeof(g_attempt_slot));
	copy_field(root, "attempt_version", g_attempt_version, sizeof(g_attempt_version));
	copy_field(root, "attempt_cixd", g_attempt_cixd, sizeof(g_attempt_cixd));
	copy_field(root, "bad", g_bad, sizeof(g_bad));
	copy_field(root, "bad_cixd", g_bad_cixd, sizeof(g_bad_cixd));
	json_free(root);
}

void rootslot_set_on_change(void (*on_change)(void))
{
	g_on_change = on_change;
}

void rootslot_set_running_cixd(const char *cixd)
{
	snprintf(g_running_cixd, sizeof(g_running_cixd), "%s", cixd != NULL ? cixd : "");
	/* A mark read back from the file can name it too: a refused switch
	 * recorded under an older process that did not know this rule. */
	if (g_bad_cixd[0] != '\0' && g_running_cixd[0] != '\0' &&
	    strcmp(g_bad_cixd, g_running_cixd) == 0) {
		g_bad_cixd[0] = '\0';
		save();
	}
}

const char *rootslot_version(const char *slot)
{
	const char *v = slot_field(slot);

	return v != NULL ? v : "";
}

const char *rootslot_cixd(const char *slot)
{
	const char *c = cixd_field(slot);

	return c != NULL ? c : "";
}

void rootslot_clear(const char *slot)
{
	char *v = slot_field(slot), *c = cixd_field(slot);

	if (v == NULL || (v[0] == '\0' && c[0] == '\0'))
		return;
	v[0] = c[0] = '\0';
	save();
}

void rootslot_set(const char *slot, const char *version, const char *cixd)
{
	char *v = slot_field(slot), *c = cixd_field(slot);

	if (v == NULL || version == NULL)
		return;
	if (cixd == NULL)
		cixd = "";
	if (strcmp(v, version) == 0 && strcmp(c, cixd) == 0)
		return;
	snprintf(v, IMAGE_VERSION_MAX, "%s", version);
	snprintf(c, ROOTSLOT_CIXD_MAX, "%s", cixd);
	save();
}

void rootslot_attempt(const char *slot)
{
	const char *v = rootslot_version(slot), *c = rootslot_cixd(slot);

	if (v[0] == '\0')
		return;
	if (strcmp(g_attempt_slot, slot) == 0 && strcmp(g_attempt_version, v) == 0 &&
	    strcmp(g_attempt_cixd, c) == 0)
		return;
	snprintf(g_attempt_slot, sizeof(g_attempt_slot), "%s", slot);
	snprintf(g_attempt_version, sizeof(g_attempt_version), "%s", v);
	snprintf(g_attempt_cixd, sizeof(g_attempt_cixd), "%s", c);
	save();
}

enum rootslot_start rootslot_resolve_start(const char *running_slot, char *version,
                                           unsigned long version_size)
{
	if (version != NULL && version_size > 0)
		version[0] = '\0';
	if (running_slot == NULL || slot_field(running_slot) == NULL)
		return ROOTSLOT_START_SAME;
	if (g_attempt_slot[0] != '\0') {
		if (strcmp(g_attempt_slot, running_slot) == 0)
			return ROOTSLOT_START_SAME; /* rootslot_confirmed() resolves it later */
		/* The switch or boot into the attempted slot failed, and the
		 * host runs the other one. */
		snprintf(g_bad, sizeof(g_bad), "%s", g_attempt_version);
		set_bad_cixd(g_attempt_cixd);
		g_attempt_slot[0] = g_attempt_version[0] = g_attempt_cixd[0] = '\0';
		if (version != NULL && version_size > 0)
			snprintf(version, version_size, "%s", g_bad);
		save();
		return ROOTSLOT_START_FAILED;
	}
	if (g_confirmed_slot[0] != '\0' && strcmp(g_confirmed_slot, running_slot) != 0) {
		/* No attempt, yet the host left the slot it last confirmed: an
		 * operator chose this slot, or a boot rolled back. What it left
		 * must not be deployed again by itself. */
		const char *left = rootslot_version(g_confirmed_slot);

		if (left[0] == '\0')
			return ROOTSLOT_START_SAME;
		snprintf(g_bad, sizeof(g_bad), "%s", left);
		set_bad_cixd(rootslot_cixd(g_confirmed_slot));
		if (version != NULL && version_size > 0)
			snprintf(version, version_size, "%s", g_bad);
		save();
		return ROOTSLOT_START_REJECTED;
	}
	return ROOTSLOT_START_SAME;
}

void rootslot_confirmed(const char *slot)
{
	const char *running, *running_cixd;
	int changed = 0;

	if (slot == NULL || slot_field(slot) == NULL)
		return;
	if (strcmp(g_confirmed_slot, slot) != 0) {
		snprintf(g_confirmed_slot, sizeof(g_confirmed_slot), "%s", slot);
		changed = 1;
	}
	if (g_attempt_slot[0] != '\0' && strcmp(g_attempt_slot, slot) == 0) {
		g_attempt_slot[0] = g_attempt_version[0] = g_attempt_cixd[0] = '\0';
		changed = 1;
	}
	/* The root marked bad booted and confirmed after all (a refused
	 * switch's staged slot, booted at a later reboot): it works. */
	running = rootslot_version(slot);
	running_cixd = rootslot_cixd(slot);
	if (g_bad[0] != '\0' && running[0] != '\0' && strcmp(g_bad, running) == 0) {
		g_bad[0] = g_bad_cixd[0] = '\0';
		changed = 1;
	} else if (g_bad_cixd[0] != '\0' && strcmp(g_bad_cixd, running_cixd) == 0) {
		/* ADR-0380: the confirmed root holds the bad version's cixd, so
		 * that cixd works and was not what failed. Only the version
		 * stays bad: both slots hold one cixd after a root-only roll
		 * that moved a library. */
		g_bad_cixd[0] = '\0';
		changed = 1;
	}
	if (changed)
		save();
}

void rootslot_mark_bad(const char *version, const char *cixd)
{
	if (version == NULL || version[0] == '\0')
		return;
	if (cixd == NULL || (g_running_cixd[0] != '\0' && strcmp(cixd, g_running_cixd) == 0))
		cixd = "";
	if (strcmp(g_bad, version) == 0 && strcmp(g_bad_cixd, cixd) == 0)
		return;
	snprintf(g_bad, sizeof(g_bad), "%s", version);
	set_bad_cixd(cixd);
	save();
}

const char *rootslot_bad(void)
{
	return g_bad;
}

const char *rootslot_bad_cixd(void)
{
	return g_bad_cixd;
}

void rootslot_clear_bad(void)
{
	if (g_bad[0] == '\0' && g_bad_cixd[0] == '\0')
		return;
	g_bad[0] = g_bad_cixd[0] = '\0';
	save();
}
