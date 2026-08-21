/*
 * ADR-0179 (issue #29): subordinate-ID range allocator -- see subid.h
 * for the contract and the ADR for the full design (floor/width
 * choices, both-direction collision checking, key semantics).
 *
 * Persistence follows this codebase's flat-table convention (ADR-0012):
 * one JSON array, atomically rewritten on every committed allocation,
 * loaded whole at startup. An allocation is only ever visible to
 * callers after its persist succeeded -- a crash between decide and
 * persist simply re-derives the same answer next time (the table is
 * append-only and the next-base scan is deterministic over it).
 */
#include "subid.h"
#include "json.h"
#include "ldap.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUBID_MAX 512
#define SUBID_KEY_MAX 128

struct subid_entry {
	char key[SUBID_KEY_MAX];
	long long base;
};

static struct subid_entry g_table[SUBID_MAX];
static int g_count;
static char g_state_path[PATH_MAX];

static struct subid_entry *find(const char *key)
{
	int i;

	for (i = 0; i < g_count; i++) {
		if (strcmp(g_table[i].key, key) == 0)
			return &g_table[i];
	}
	return NULL;
}

static int persist_table(void)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < g_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "key");
		jw_str(&w, g_table[i].key);
		jw_key(&w, "base");
		jw_int(&w, g_table[i].base);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

int subid_init(const char *state_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	size_t i;

	if (snprintf(g_state_path, sizeof(g_state_path), "%s", state_path) >=
	    (int)sizeof(g_state_path))
		return -1;
	g_count = 0;
	memset(g_table, 0, sizeof(g_table));

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted subid table\n", g_state_path);
		return -1;
	}
	for (i = 0; i < root->u.array.count && g_count < SUBID_MAX; i++) {
		const struct json_value *item = root->u.array.items[i];
		const char *key = json_as_string(json_object_get(item, "key"));
		const struct json_value *jbase = json_object_get(item, "base");

		if (key == NULL || jbase == NULL || jbase->type != JSON_NUMBER) {
			fprintf(stderr, "%s: invalid entry at index %zu\n", g_state_path, i);
			continue;
		}
		snprintf(g_table[g_count].key, sizeof(g_table[g_count].key), "%s", key);
		g_table[g_count].base = (long long)json_as_number(jbase);
		g_count++;
	}
	json_free(root);
	return 0;
}

void subid_repoint(const char *new_state_path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", new_state_path);
}

int subid_lookup_or_assign(const char *key, long long *out_base)
{
	struct subid_entry *e = find(key);
	long long next;
	int i;

	if (e != NULL) {
		*out_base = e->base;
		return 0;
	}
	if (g_count >= SUBID_MAX)
		return -1;

	/* Deterministic next-base scan: one past the highest committed
	 * range, never below the floor. */
	next = SUBID_FLOOR;
	for (i = 0; i < g_count; i++) {
		if (g_table[i].base + SUBID_RANGE_LEN > next)
			next = g_table[i].base + SUBID_RANGE_LEN;
	}

	/* ADR-0179's active collision check: never commit a range any
	 * managed user's own real uidnumber falls inside -- the floor
	 * should already make this unreachable, but "should" is not
	 * "verified". Step past a colliding candidate rather than fail:
	 * the very next range is checked the same way. */
	while (ldap_user_uidnumber_in_range(next, next + SUBID_RANGE_LEN - 1)) {
		next += SUBID_RANGE_LEN;
		if (next > LLONG_MAX - SUBID_RANGE_LEN)
			return -1;
	}

	snprintf(g_table[g_count].key, sizeof(g_table[g_count].key), "%s", key);
	g_table[g_count].base = next;
	g_count++;
	if (persist_table() != 0) {
		g_count--;
		return -1;
	}
	*out_base = next;
	return 0;
}

int subid_overlaps_uidnumber(long long uidnumber)
{
	int i;

	for (i = 0; i < g_count; i++) {
		if (uidnumber >= g_table[i].base && uidnumber < g_table[i].base + SUBID_RANGE_LEN)
			return 1;
	}
	return 0;
}
