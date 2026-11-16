/*
 * ADR-0379: which cix-root version each boot slot holds. See rootslot.h.
 *
 * The file is {"a": V, "b": V, "confirmed_slot": S, "attempt_slot": S,
 * "attempt_version": V, "bad": V}, every field optional. It is rewritten
 * whole on every change (persist_atomic_write()), and a change that
 * alters nothing writes nothing.
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
static char g_confirmed_slot[8];
static char g_attempt_slot[8];
static char g_attempt_version[IMAGE_VERSION_MAX];
static char g_bad[IMAGE_VERSION_MAX];
static void (*g_on_change)(void);

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
	put_field(&w, "confirmed_slot", g_confirmed_slot);
	put_field(&w, "attempt_slot", g_attempt_slot);
	put_field(&w, "attempt_version", g_attempt_version);
	put_field(&w, "bad", g_bad);
	jw_obj_close(&w);
	if (w.buf != NULL && persist_atomic_write(g_path, w.buf, w.len) != 0)
		fprintf(stderr, "rootslot: could not write %s\n", g_path);
	jw_free(&w);
	if (g_on_change != NULL)
		g_on_change();
}

/* Sets field to value, saving only when that changes it. */
static void set_and_save(char *field, size_t size, const char *value)
{
	if (strcmp(field, value) == 0)
		return;
	snprintf(field, size, "%s", value);
	save();
}

void rootslot_load(const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	snprintf(g_path, sizeof(g_path), "%s", path != NULL ? path : "");
	g_slot_a[0] = g_slot_b[0] = g_confirmed_slot[0] = '\0';
	g_attempt_slot[0] = g_attempt_version[0] = g_bad[0] = '\0';
	if (g_path[0] == '\0' || persist_read_file(g_path, &buf, &len) != 0 || buf == NULL)
		return;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return;
	copy_field(root, "a", g_slot_a, sizeof(g_slot_a));
	copy_field(root, "b", g_slot_b, sizeof(g_slot_b));
	copy_field(root, "confirmed_slot", g_confirmed_slot, sizeof(g_confirmed_slot));
	copy_field(root, "attempt_slot", g_attempt_slot, sizeof(g_attempt_slot));
	copy_field(root, "attempt_version", g_attempt_version, sizeof(g_attempt_version));
	copy_field(root, "bad", g_bad, sizeof(g_bad));
	json_free(root);
}

void rootslot_set_on_change(void (*on_change)(void))
{
	g_on_change = on_change;
}

const char *rootslot_version(const char *slot)
{
	const char *v = slot_field(slot);

	return v != NULL ? v : "";
}

void rootslot_clear(const char *slot)
{
	char *v = slot_field(slot);

	if (v != NULL)
		set_and_save(v, IMAGE_VERSION_MAX, "");
}

void rootslot_set(const char *slot, const char *version)
{
	char *v = slot_field(slot);

	if (v != NULL && version != NULL)
		set_and_save(v, IMAGE_VERSION_MAX, version);
}

void rootslot_attempt(const char *slot)
{
	const char *v = rootslot_version(slot);

	if (v[0] == '\0')
		return;
	if (strcmp(g_attempt_slot, slot) == 0 && strcmp(g_attempt_version, v) == 0)
		return;
	snprintf(g_attempt_slot, sizeof(g_attempt_slot), "%s", slot);
	snprintf(g_attempt_version, sizeof(g_attempt_version), "%s", v);
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
		g_attempt_slot[0] = g_attempt_version[0] = '\0';
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
		if (version != NULL && version_size > 0)
			snprintf(version, version_size, "%s", g_bad);
		save();
		return ROOTSLOT_START_REJECTED;
	}
	return ROOTSLOT_START_SAME;
}

void rootslot_confirmed(const char *slot)
{
	const char *running;
	int changed = 0;

	if (slot == NULL || slot_field(slot) == NULL)
		return;
	if (strcmp(g_confirmed_slot, slot) != 0) {
		snprintf(g_confirmed_slot, sizeof(g_confirmed_slot), "%s", slot);
		changed = 1;
	}
	if (g_attempt_slot[0] != '\0' && strcmp(g_attempt_slot, slot) == 0) {
		g_attempt_slot[0] = g_attempt_version[0] = '\0';
		changed = 1;
	}
	/* The root marked bad booted and confirmed after all (a refused
	 * switch's staged slot, booted at a later reboot): it works. */
	running = rootslot_version(slot);
	if (g_bad[0] != '\0' && running[0] != '\0' && strcmp(g_bad, running) == 0) {
		g_bad[0] = '\0';
		changed = 1;
	}
	if (changed)
		save();
}

void rootslot_mark_bad(const char *version)
{
	if (version != NULL && version[0] != '\0')
		set_and_save(g_bad, sizeof(g_bad), version);
}

const char *rootslot_bad(void)
{
	return g_bad;
}

void rootslot_clear_bad(void)
{
	set_and_save(g_bad, sizeof(g_bad), "");
}
