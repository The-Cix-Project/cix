#ifndef DISKSNAP_H
#define DISKSNAP_H

#include "disk.h"

/*
 * ADR-0377: the disk snapshot. What a helper child read from every
 * block device -- filesystem type and UUID, partition label, and a
 * mounted filesystem's capacity -- kept so that no caller on the event
 * loop reads a block device or waits on a busy filesystem.
 *
 * Measured on 192.168.15.95 before it (GET /v1/system/stalls): GET
 * /v1/storage held the loop 5240 ms, GET /v1/volumes 1173 ms and GET
 * /v1/storage/quota 795 ms, all during builds, all through
 * disk_enumerate()'s superblock, GPT and statvfs reads.
 *
 * This module holds data and the rule for installing it. It forks
 * nothing; main.c runs the child and calls disksnap_install() when the
 * child exits.
 */

#define DISKSNAP_MAX DISK_ENUM_MAX

/* Every device's change counter at the moment a child was forked. */
struct disksnap_counters {
	int n;
	char names[DISKSNAP_MAX][32];
	unsigned long counters[DISKSNAP_MAX];
};

/*
 * Something changed this device, or is about to: a format, a partition
 * write, a mount, a kernel uevent. Bumps its counter, makes what the
 * snapshot holds for it unknown at once, and asks for a refresh. Any
 * module may call it; the refresh is started by main.c's loop.
 */
void disksnap_touch(const char *name);

/* The device's counter now; 0 for one never touched. */
unsigned long disksnap_counter(const char *name);

void disksnap_counters_capture(struct disksnap_counters *out);

/*
 * The one rule (ADR-0377 item 5): a result read in a child may be
 * installed only when nothing touched its device since the fork.
 */
int disksnap_result_installable(unsigned long counter_at_fork, unsigned long counter_now);

/*
 * Installs a child's (or the start-up build's) results. A result whose
 * device was touched since `at_fork` is left unknown; a device absent
 * from the results is dropped. Returns 1 when anything a client can see
 * changed, 0 otherwise.
 */
int disksnap_install(const struct discovered_disk *res, int n,
                     const struct disksnap_counters *at_fork);

/*
 * disk_enumerate()'s identity source: copies what the snapshot holds
 * for e->name into e when the entry still describes the same device
 * (same size and start sector), and marks e unknown otherwise.
 */
void disksnap_fill(struct discovered_disk *e);

/* Is this device's identity known right now. */
int disksnap_identity_known(const char *name);

/* Was a refresh asked for since the last call; clears the request. */
int disksnap_take_wanted(void);

/*
 * Did a touch turn a known device unknown since the last call; clears
 * it. Every read built from the snapshot then answers differently, and
 * a kernel uevent has no request to announce it, so main.c announces
 * them when this says so (ADR-0339).
 */
int disksnap_take_changed(void);

/* Test only: forget everything. */
void disksnap_reset(void);

#endif
