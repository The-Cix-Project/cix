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

struct discovered_device {
	char id[96];
	char bus[8];		/* "usb", "pci", "net", "gpu", or "disk" */
	char vendor_id[16];
	char product_id[16];
	char class_hex[8];	/* PCI only, empty for USB */
	char description[128];
	char driver[64];	/* empty if unbound */
	char dev_path[64];	/* empty if no /dev node was found */
	enum device_node_type type;
	unsigned int major;
	unsigned int minor;
	int assignable;		/* driver bound AND a real /dev node found */
};

/*
 * Walks /sys/bus/usb/devices, /sys/bus/pci/devices, /sys/class/net,
 * /sys/class/drm, and (ADR-0142) every non-OS, role-less whole disk
 * disk_enumerate() reports, fresh, writing up to cap entries into out.
 * Returns the count written (0 if none of those sysfs trees are
 * present -- e.g. this dev environment's own kernel lacking USB support
 * entirely is a normal, non-fatal case, not an error).
 *
 * os_containers_dir is passed straight through to disk_enumerate()
 * (see disk.h) to identify and exclude the OS disk -- device.c has no
 * knowledge of main.c's own CONTAINERS_DIR global, so the caller
 * passes it in explicitly, the same convention disk.c itself already
 * established. Pass NULL from a context with no real containers
 * partition (most test/dev daemons); every disk is then a passthrough
 * candidate except ones with an assigned role.
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
 * Resolves id to one or more discovered devices (ADR-0028). An exact
 * id match -- covering every usb:/pci:/net: id, and a "gpu:<idx>:
 * <node>" id naming one specific GPU member node directly -- returns
 * exactly that one entry, identical to device_find(). Only a bare
 * "gpu:<idx>" logical id with no exact match falls through to
 * expansion: every currently assignable "gpu:<idx>:*" member is
 * returned, so a single grant can atomically request everything one
 * physical GPU needs (e.g. both its card and render nodes) without the
 * caller naming each one by hand. Writes up to cap pointers into out,
 * into the same kind of per-call static cache device_find() itself
 * uses -- valid until the next device_find()/device_find_group() call.
 * Returns the count written, or -1 if id resolves to nothing
 * assignable at all.
 */
int device_find_group(const char *id, const char *os_containers_dir,
                       const struct discovered_device *out[], int cap);

void device_write_json_one(const struct discovered_device *d, struct json_writer *w);

/* Calls device_enumerate() itself, then writes the whole list. */
void device_write_json_list(struct json_writer *w, const char *os_containers_dir);

#endif /* DEVICE_H */
