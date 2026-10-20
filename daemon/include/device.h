#ifndef DEVICE_H
#define DEVICE_H

#include "container.h"
#include "json.h"

/*
 * Host devices (USB, PCI) discovered fresh from sysfs on every call --
 * never persisted (ADR-0012's atomic-JSON pattern is for daemon-owned
 * state; these are host hardware the daemon doesn't create or own,
 * re-enumerated the same way `ip link`/`lsusb` would be, every time).
 */
#define DEVICE_ENUM_MAX 256

/* ADR-0161 Phase A: purely descriptive metadata for a composite USB
 * device's own real interfaces (e.g. a combo HID+storage device) --
 * the passthrough unit stays the whole device, exactly as before;
 * this only makes GET /devices show what a device is actually made
 * of instead of staying opaque. Empty (interface_count == 0) for
 * every non-USB device, and for a USB device with only the one
 * implicit interface a single-function device already has. */
#define USB_MAX_INTERFACES_REPORTED 16

struct usb_interface_info {
	int number;
	char class_hex[8];
	char subclass_hex[8];
	char protocol_hex[8];
};

#define DEVICE_ID_MAX 96

/*
 * One record (ADR-0344): a thing the kernel models as a device that Cix
 * can grant or reason about -- a USB device, a PCI function, a node of a
 * PCI function no more specific walk owns, a disk or partition, a DRM
 * node, a wireless PHY, a network interface. Each /dev node belongs to
 * exactly one record.
 *
 * The record carries FACTS, never a verdict: whether it can be granted
 * is devicestate.c's decision, made from these fields and the grants,
 * so that the list, the clients and every grant path read one answer.
 */
struct discovered_device {
	char id[DEVICE_ID_MAX];
	char bus[8];		/* "usb", "pci", "disk", "gpu", "phy" or "net" */
	char parent[DEVICE_ID_MAX]; /* nearest ancestor that is a record; "" for none */
	char vendor_id[16];
	char product_id[16];
	char class_hex[8];	/* PCI only, empty for every other bus */
	char description[128];
	char driver[64];	/* empty if unbound */
	char dev_path[64];	/* empty if this record owns no /dev node */
	enum device_node_type type;
	unsigned int major;
	unsigned int minor;
	struct usb_interface_info interfaces[USB_MAX_INTERFACES_REPORTED];
	int interface_count;

	/* Facts devicestate.c decides from. */
	int requires_driver;	/* usb and DRM nodes: nothing works without one */
	int always_shared;	/* gpu:kfd, host-wide by design (ADR-0029) */
	int is_partition;	/* disk records only */
	int os_layout;		/* diskpart_os_layout_untouchable(): this record only */
	int storage_role;	/* holds a storage role; reaches below, and above as any fact does */
	int mounted;		/* THIS device is host-mounted; reaches below. A disk whose partition is
				 * mounted gets no fact of its own -- the partition below makes it
				 * the host's, and a sibling partition stays free. */
	int bridge_port;	/* net: enslaved to a bridge (the kernel's own `master` link) */
	char master[16];	/* net: that bridge's name */
	int management;		/* net: carries the management address, or is a port of the bridge that does */

	/* Internal to enumeration: the canonical sysfs path parents are found by. */
	char sysfs[256];
};

/*
 * Walks USB, PCI, block, DRM, wireless PHY and network interfaces fresh
 * from sysfs, writing up to cap records into out, and returns the count
 * (0 when none of those trees exist, which is a normal answer, not an
 * error). Every node is reported once, by the most specific walk that
 * reaches it, and every record's parent is filled in.
 *
 * os_containers_dir is passed straight through to disk_enumerate() (see
 * disk.h) to identify the OS disk -- device.c has no knowledge of
 * main.c's CONTAINERS_DIR, so the caller passes it in. Pass NULL from a
 * context with no real containers partition; no disk is then the OS
 * disk. The OS disk is REPORTED either way (ADR-0344): hiding a disk
 * the host uses is what made `sda` vanish while `vda` was offered.
 *
 * The management fact needs daemon configuration this module does not
 * own, so it is left 0 here and set by devicegrant.c.
 */
int device_enumerate(struct discovered_device *out, int cap, const char *os_containers_dir);

/*
 * device_enumerate() plus a linear search for id. NULL if not found.
 * Re-walks sysfs on every call, same as device_enumerate() itself --
 * simple and correct rather than cached, since this isn't a hot path
 * (bounded by CONTAINER_MAX_DEVICES lookups per container-create
 * request) and host hardware can change between calls (a USB device
 * unplugged and replugged elsewhere).
 */
const struct discovered_device *device_find(const char *id, const char *os_containers_dir);

/*
 * Resolves id to one or more records (ADR-0028). An exact id match
 * returns exactly that record, identical to device_find(). Only a bare
 * "gpu:<idx>" logical id with no exact match falls through to
 * expansion: every "gpu:<idx>:*" member that has a driver, plus
 * "gpu:kfd" when the host has one, so a single grant can request
 * everything one physical GPU needs without the caller naming each
 * node. Whether each member may be GRANTED is not decided here --
 * every one goes through devicegrant_check(). Writes up to cap pointers
 * into out, into the same per-call static cache device_find() uses --
 * valid until the next device_find()/device_find_group() call. Returns
 * the count written, or -1 if id resolves to nothing at all.
 */
int device_find_group(const char *id, const char *os_containers_dir,
                       const struct discovered_device *out[], int cap);

#endif /* DEVICE_H */
