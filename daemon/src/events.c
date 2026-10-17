#include "events.h"
#include "json.h"

#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* ADR-0339: the generated announced reads and changes. */
#include "generated/events_table.h"

static uint64_t g_generation[EVENTS_READ_COUNT];
static char g_epoch[17];
static void (*g_listener)(int slot);

void events_set_listener(void (*fn)(int slot))
{
	g_listener = fn;
}

void events_init(void)
{
	uint64_t e = 0;

	/*
	 * Random rather than the start time: two starts within the same
	 * second -- a crash and its restart -- would otherwise share an
	 * epoch, and a client holding generations from the first would
	 * read the second's restarted counters as its own and re-read
	 * nothing. getrandom() without GRND_NONBLOCK can only wait before
	 * the kernel's pool is first initialised, which on this platform is
	 * long done by the time cixd starts; should it ever fail, the time
	 * and pid together still differ across a restart.
	 */
	if (getrandom(&e, sizeof(e), 0) != (ssize_t)sizeof(e))
		e = ((uint64_t)time(NULL) << 20) ^ (uint64_t)getpid();
	snprintf(g_epoch, sizeof(g_epoch), "%016llx", (unsigned long long)e);
	memset(g_generation, 0, sizeof(g_generation));
}

const char *events_epoch(void)
{
	return g_epoch;
}

void events_announce(int slot)
{
	if (slot < 0 || slot >= EVENTS_READ_COUNT)
		return;
	g_generation[slot]++;
	if (g_listener != NULL)
		g_listener(slot);
}

int events_announce_change(const char *op_id)
{
	int i, j;

	for (i = 0; i < EVENTS_CHANGE_COUNT; i++) {
		if (strcmp(g_events_changes[i].op_id, op_id) != 0)
			continue;
		for (j = 0; j < g_events_changes[i].n; j++)
			events_announce(g_events_changes[i].slots[j]);
		return g_events_changes[i].n;
	}
	return -1;
}

int events_slot_of(const char *op_id)
{
	int i;

	for (i = 0; i < EVENTS_READ_COUNT; i++)
		if (strcmp(g_events_reads[i].op_id, op_id) == 0)
			return i;
	return -1;
}

uint64_t events_generation(int slot)
{
	if (slot < 0 || slot >= EVENTS_READ_COUNT)
		return 0;
	return g_generation[slot];
}

const char *events_op_id(int slot)
{
	if (slot < 0 || slot >= EVENTS_READ_COUNT)
		return NULL;
	return g_events_reads[slot].op_id;
}

const char *events_permission(int slot)
{
	if (slot < 0 || slot >= EVENTS_READ_COUNT)
		return NULL;
	return g_events_reads[slot].permission;
}

const struct events_read *events_read_at(int slot)
{
	if (slot < 0 || slot >= EVENTS_READ_COUNT)
		return NULL;
	return &g_events_reads[slot];
}

/* ---- ADR-0339 point 7: the daemon checks its own announcements ---- */

static uint64_t g_seen_hash[EVENTS_READ_COUNT];
static uint64_t g_seen_gen[EVENTS_READ_COUNT];
static unsigned char g_seen[EVENTS_READ_COUNT];
static struct events_check_counts g_counts;

/* FNV-1a, 64-bit: small, fast, and a change detector rather than a
 * defence against anyone -- nothing here is adversarial. */
static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
	const unsigned char *p = data;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

/* Whether `key` is one of the comma-separated names in `list`. */
static int listed(const char *list, const char *key)
{
	size_t klen = strlen(key);
	const char *p = list;

	while (p != NULL && *p != '\0') {
		const char *comma = strchr(p, ',');
		size_t n = comma != NULL ? (size_t)(comma - p) : strlen(p);

		if (n == klen && strncmp(p, key, n) == 0)
			return 1;
		p = comma != NULL ? comma + 1 : NULL;
	}
	return 0;
}

/* The canonical walk: type, then content, keys in document order (the
 * daemon writes them in a fixed order), volatile keys left out. */
static uint64_t hash_value(uint64_t h, const struct json_value *v, const char *volatile_fields)
{
	size_t i;
	unsigned char tag = (unsigned char)v->type;

	h = fnv(h, &tag, 1);
	switch (v->type) {
	case JSON_OBJECT:
		for (i = 0; i < v->u.object.count; i++) {
			if (listed(volatile_fields, v->u.object.keys[i]))
				continue;
			h = fnv(h, v->u.object.keys[i], strlen(v->u.object.keys[i]) + 1);
			h = hash_value(h, v->u.object.values[i], volatile_fields);
		}
		break;
	case JSON_ARRAY:
		for (i = 0; i < v->u.array.count; i++)
			h = hash_value(h, v->u.array.items[i], volatile_fields);
		break;
	case JSON_STRING:
		h = fnv(h, v->u.string, strlen(v->u.string) + 1);
		break;
	case JSON_NUMBER:
		h = fnv(h, &v->u.number, sizeof(v->u.number));
		break;
	case JSON_BOOL:
		h = fnv(h, &v->u.boolean, sizeof(v->u.boolean));
		break;
	default:
		break;
	}
	return h;
}

uint64_t events_hash_body(const char *body, size_t len, const char *volatile_fields)
{
	uint64_t h = 1469598103934665603ULL;
	struct json_value *v = body != NULL ? json_parse(body, len) : NULL;

	if (v == NULL)
		return fnv(h, body != NULL ? body : "", body != NULL ? len : 0);
	h = hash_value(h, v, volatile_fields != NULL ? volatile_fields : "");
	json_free(v);
	return h;
}

int events_observe(int slot, uint64_t hash)
{
	int drift = 0;

	if (slot < 0 || slot >= EVENTS_READ_COUNT)
		return 0;
	if (g_events_reads[slot].kind == EVENTS_READ_SAMPLE) {
		/* A sample is announced by being observed to change; its first
		 * observation is the baseline a client already read. */
		if (g_seen[slot] && g_seen_hash[slot] != hash)
			events_announce(slot);
	} else if (g_seen[slot]) {
		g_counts.checked++;
		if (g_seen_hash[slot] != hash && g_seen_gen[slot] == g_generation[slot]) {
			g_counts.drift++;
			drift = 1;
		}
	}
	g_seen[slot] = 1;
	g_seen_hash[slot] = hash;
	g_seen_gen[slot] = g_generation[slot];
	return drift;
}

static unsigned char g_unreadable[EVENTS_READ_COUNT];

void events_mark_readable(int slot, int readable)
{
	if (slot >= 0 && slot < EVENTS_READ_COUNT)
		g_unreadable[slot] = !readable;
}

struct events_check_counts events_check_counts(void)
{
	struct events_check_counts c = g_counts;
	int i;

	c.unreadable = 0;
	for (i = 0; i < EVENTS_READ_COUNT; i++)
		c.unreadable += g_unreadable[i];
	return c;
}
