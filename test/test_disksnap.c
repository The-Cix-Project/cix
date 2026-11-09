/*
 * test_disksnap -- the disk snapshot's rules (ADR-0377), without a
 * daemon and without a block device.
 *
 * What is proved here is the decision the snapshot exists to make
 * correctly: a probe result is installed only when nothing touched its
 * device since the child was forked, a touched device reads as unknown
 * at once, and what disk_enumerate() is given for a device is the
 * snapshot's only while the snapshot still describes that device.
 *
 * In SELFTESTS: it reads nothing and runs nothing.
 */
#include "disksnap.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		g_failures++;
	}
}

/* A probe result as disk_enumerate_probe() would give it. */
static struct discovered_disk probed(const char *name, unsigned long long size,
                                    const char *fs_type, const char *uuid, int mounted,
                                    unsigned long long used)
{
	struct discovered_disk d;

	memset(&d, 0, sizeof(d));
	snprintf(d.name, sizeof(d.name), "%s", name);
	d.size_bytes = size;
	d.identity_known = 1;
	snprintf(d.fs_type, sizeof(d.fs_type), "%s", fs_type);
	snprintf(d.fs_uuid, sizeof(d.fs_uuid), "%s", uuid);
	d.mounted = mounted;
	d.capacity_known = mounted;
	d.used_bytes = used;
	d.free_bytes = mounted ? 1000 : 0;
	d.probed_at = 1700000000;
	return d;
}

/* What disk_enumerate() reads live for a device, before the snapshot
 * fills the rest. */
static struct discovered_disk live(const char *name, unsigned long long size, int mounted)
{
	struct discovered_disk d;

	memset(&d, 0, sizeof(d));
	snprintf(d.name, sizeof(d.name), "%s", name);
	d.size_bytes = size;
	d.mounted = mounted;
	/* Whatever was left in the struct must not survive the fill. */
	snprintf(d.fs_type, sizeof(d.fs_type), "stale");
	return d;
}

int main(void)
{
	struct discovered_disk res[3], e;
	struct disksnap_counters at_fork;

	printf("test_disksnap\n");
	disksnap_reset();

	check(disksnap_result_installable(3, 3), "an unchanged counter installs");
	check(!disksnap_result_installable(3, 4), "a counter that moved does not");

	/* A first install: everything is new, so something changed. */
	res[0] = probed("sda", 1000, "btrfs", "aaaa-1", 1, 500);
	res[1] = probed("sda1", 400, "ext4", "bbbb-2", 0, 0);
	disksnap_counters_capture(&at_fork);
	check(disksnap_install(res, 2, &at_fork) == 1, "a first install reports a change");
	check(disksnap_install(res, 2, &at_fork) == 0, "the same results again change nothing");
	check(disksnap_identity_known("sda1"), "an installed device is known");

	e = live("sda", 1000, 1);
	disksnap_fill(&e);
	check(e.identity_known && strcmp(e.fs_type, "btrfs") == 0 &&
	              strcmp(e.fs_uuid, "aaaa-1") == 0,
	      "a fill copies the probed identity");
	check(e.capacity_known && e.used_bytes == 500, "and a mounted device's capacity");
	check(e.probed_at == 1700000000, "and when it was probed");

	/* Mounted since the probe: capacity is unknown, identity is not. */
	e = live("sda1", 400, 1);
	disksnap_fill(&e);
	check(e.identity_known && !e.capacity_known,
	      "a device mounted since its probe has unknown capacity, known identity");

	/* A different device under the same name. */
	e = live("sda1", 999, 0);
	disksnap_fill(&e);
	check(!e.identity_known && e.fs_type[0] == '\0',
	      "a device whose size no longer matches is unknown, and nothing stale is left");

	/* A device the snapshot never saw. */
	e = live("sdz", 10, 0);
	disksnap_fill(&e);
	check(!e.identity_known, "a device not in the snapshot is unknown");

	/* A touch makes the device unknown at once and asks for a refresh. */
	(void)disksnap_take_wanted();
	(void)disksnap_take_changed();
	disksnap_touch("sda1");
	check(disksnap_take_wanted(), "a touch asks for a refresh");
	check(!disksnap_take_wanted(), "and asks once");
	check(disksnap_take_changed(),
	      "a touch that made a known device unknown is a change to announce");
	disksnap_touch("sda1");
	check(!disksnap_take_changed(), "touching a device already unknown changes nothing");
	check(!disksnap_identity_known("sda1"), "a touched device is unknown at once");
	e = live("sda1", 400, 0);
	disksnap_fill(&e);
	check(!e.identity_known, "and disk_enumerate() is told so");

	/* The case the rule exists for: a probe forked before the touch read
	 * the device from either side of the change. Capture, then touch
	 * (the format starts), then install what the child read. */
	disksnap_counters_capture(&at_fork);
	disksnap_touch("sda1");
	res[1] = probed("sda1", 400, "ext4", "old-uuid", 0, 0);
	(void)disksnap_install(res, 2, &at_fork);
	check(!disksnap_identity_known("sda1"),
	      "a result read while its device was being changed is not installed");
	check(disksnap_identity_known("sda"),
	      "while the other devices in the same result are");

	/* The next probe, forked after the change, is installed. */
	disksnap_counters_capture(&at_fork);
	res[1] = probed("sda1", 400, "btrfs", "new-uuid", 0, 0);
	check(disksnap_install(res, 2, &at_fork) == 1, "the probe after the change installs");
	e = live("sda1", 400, 0);
	disksnap_fill(&e);
	check(e.identity_known && strcmp(e.fs_uuid, "new-uuid") == 0,
	      "and reports the new filesystem");

	/* A device that has gone. */
	disksnap_counters_capture(&at_fork);
	check(disksnap_install(res, 1, &at_fork) == 1, "a device that left is a change");
	check(!disksnap_identity_known("sda1"), "and is no longer known");

	if (g_failures != 0) {
		printf("FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
