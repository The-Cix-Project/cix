#include "disksnap.h"

#include <stdio.h>
#include <string.h>

/*
 * ADR-0377. The entries hold only what a child probed; topology and
 * mount status are read live by disk_enumerate(), because they cost no
 * block I/O and a safety decision (is this mounted, is this the OS
 * disk) must see them as they are now.
 */
struct snap_entry {
	int in_use;
	char name[32];
	/* What identifies the device the probe described. */
	unsigned long long size_bytes;
	unsigned long long start_sector;
	int identity_known;
	char fs_type[16];
	char fs_uuid[DISK_FS_UUID_MAX];
	char part_label[40];
	int capacity_known;
	unsigned long long used_bytes;
	unsigned long long free_bytes;
	long long probed_at;
};

static struct snap_entry g_snap[DISKSNAP_MAX];

struct counter {
	char name[32];
	unsigned long value;
};

static struct counter g_counters[DISKSNAP_MAX];
static int g_ncounters;
static int g_wanted;
/* A touch turned a known entry unknown: what every read built from the
 * snapshot answers has changed, and must be announced (ADR-0339). */
static int g_changed;

static struct snap_entry *entry_find(const char *name)
{
	int i;

	for (i = 0; i < DISKSNAP_MAX; i++)
		if (g_snap[i].in_use && strcmp(g_snap[i].name, name) == 0)
			return &g_snap[i];
	return NULL;
}

unsigned long disksnap_counter(const char *name)
{
	int i;

	for (i = 0; i < g_ncounters; i++)
		if (strcmp(g_counters[i].name, name) == 0)
			return g_counters[i].value;
	return 0;
}

void disksnap_touch(const char *name)
{
	struct snap_entry *e;
	int i;

	if (name == NULL || name[0] == '\0')
		return;
	for (i = 0; i < g_ncounters; i++)
		if (strcmp(g_counters[i].name, name) == 0)
			break;
	if (i == g_ncounters) {
		if (g_ncounters == DISKSNAP_MAX) {
			/* Full: a device we cannot count must not be trusted, so
			 * the refresh below still runs, and what it reads for this
			 * name is installed as before. Not reached with fewer than
			 * DISKSNAP_MAX block devices, which is every host. */
			g_wanted = 1;
			return;
		}
		snprintf(g_counters[i].name, sizeof(g_counters[i].name), "%s", name);
		g_counters[i].value = 0;
		g_ncounters++;
	}
	g_counters[i].value++;
	e = entry_find(name);
	if (e != NULL) {
		if (e->identity_known || e->capacity_known)
			g_changed = 1;
		e->identity_known = 0;
		e->capacity_known = 0;
	}
	g_wanted = 1;
}

int disksnap_take_changed(void)
{
	int c = g_changed;

	g_changed = 0;
	return c;
}

void disksnap_counters_capture(struct disksnap_counters *out)
{
	int i;

	out->n = g_ncounters;
	for (i = 0; i < g_ncounters; i++) {
		snprintf(out->names[i], sizeof(out->names[i]), "%s", g_counters[i].name);
		out->counters[i] = g_counters[i].value;
	}
}

static unsigned long captured(const struct disksnap_counters *c, const char *name)
{
	int i;

	for (i = 0; c != NULL && i < c->n; i++)
		if (strcmp(c->names[i], name) == 0)
			return c->counters[i];
	return 0;
}

int disksnap_result_installable(unsigned long counter_at_fork, unsigned long counter_now)
{
	return counter_at_fork == counter_now;
}

/* What a client sees of an entry, compared so an install that changed
 * nothing announces nothing. */
static int entry_same(const struct snap_entry *a, const struct snap_entry *b)
{
	return a->identity_known == b->identity_known && a->capacity_known == b->capacity_known &&
	       a->size_bytes == b->size_bytes && a->start_sector == b->start_sector &&
	       strcmp(a->fs_type, b->fs_type) == 0 && strcmp(a->fs_uuid, b->fs_uuid) == 0 &&
	       strcmp(a->part_label, b->part_label) == 0 && a->used_bytes == b->used_bytes &&
	       a->free_bytes == b->free_bytes;
}

int disksnap_install(const struct discovered_disk *res, int n,
                     const struct disksnap_counters *at_fork)
{
	static struct snap_entry next[DISKSNAP_MAX];
	int i, j, changed = 0;

	memset(next, 0, sizeof(next));
	for (i = 0; i < n && i < DISKSNAP_MAX; i++) {
		const struct discovered_disk *d = &res[i];
		struct snap_entry *x = &next[i];
		const struct snap_entry *old = entry_find(d->name);

		x->in_use = 1;
		snprintf(x->name, sizeof(x->name), "%s", d->name);
		x->size_bytes = d->size_bytes;
		x->start_sector = d->start_sector;
		if (!disksnap_result_installable(captured(at_fork, d->name),
		                                 disksnap_counter(d->name))) {
			/* Touched since the fork: what the child read may be from
			 * either side of the change. Stays unknown until a probe
			 * forked after it. */
			changed |= old == NULL || old->identity_known || old->capacity_known;
			continue;
		}
		x->identity_known = d->identity_known;
		snprintf(x->fs_type, sizeof(x->fs_type), "%s", d->fs_type);
		snprintf(x->fs_uuid, sizeof(x->fs_uuid), "%s", d->fs_uuid);
		snprintf(x->part_label, sizeof(x->part_label), "%s", d->part_label);
		x->capacity_known = d->capacity_known;
		x->used_bytes = d->used_bytes;
		x->free_bytes = d->free_bytes;
		x->probed_at = d->probed_at;
		if (old == NULL || !entry_same(old, x))
			changed = 1;
	}
	/* A device in the old snapshot and not in the new one has gone. */
	for (j = 0; j < DISKSNAP_MAX; j++) {
		int still = 0;

		if (!g_snap[j].in_use)
			continue;
		for (i = 0; i < n && i < DISKSNAP_MAX; i++)
			if (strcmp(next[i].name, g_snap[j].name) == 0)
				still = 1;
		if (!still)
			changed = 1;
	}
	memcpy(g_snap, next, sizeof(g_snap));
	return changed;
}

void disksnap_fill(struct discovered_disk *e)
{
	const struct snap_entry *x = entry_find(e->name);

	e->identity_known = 0;
	e->capacity_known = 0;
	e->probed_at = 0;
	e->fs_type[0] = '\0';
	e->fs_uuid[0] = '\0';
	e->part_label[0] = '\0';
	e->used_bytes = 0;
	e->free_bytes = 0;
	if (x == NULL || x->size_bytes != e->size_bytes || x->start_sector != e->start_sector)
		return;
	e->probed_at = x->probed_at;
	if (x->identity_known) {
		e->identity_known = 1;
		snprintf(e->fs_type, sizeof(e->fs_type), "%s", x->fs_type);
		snprintf(e->fs_uuid, sizeof(e->fs_uuid), "%s", x->fs_uuid);
		snprintf(e->part_label, sizeof(e->part_label), "%s", x->part_label);
	}
	/* Capacity describes the mount the probe saw. A device mounted
	 * since is unknown until the next probe, which its mount asked for. */
	if (x->capacity_known && e->mounted) {
		e->capacity_known = 1;
		e->used_bytes = x->used_bytes;
		e->free_bytes = x->free_bytes;
	}
}

int disksnap_identity_known(const char *name)
{
	const struct snap_entry *x = entry_find(name);

	return x != NULL && x->identity_known;
}

int disksnap_take_wanted(void)
{
	int w = g_wanted;

	g_wanted = 0;
	return w;
}

void disksnap_reset(void)
{
	memset(g_snap, 0, sizeof(g_snap));
	memset(g_counters, 0, sizeof(g_counters));
	g_ncounters = 0;
	g_wanted = 0;
	g_changed = 0;
}
