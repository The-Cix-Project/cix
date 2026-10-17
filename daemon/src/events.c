#include "events.h"

#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* ADR-0339: the generated announced reads and changes. */
#include "generated/events_table.h"

static uint64_t g_generation[EVENTS_READ_COUNT];
static char g_epoch[17];

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
