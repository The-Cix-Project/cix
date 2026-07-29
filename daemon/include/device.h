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
	char bus[4];		/* "usb", "pci", or "net" */
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
 * Walks /sys/bus/usb/devices and /sys/bus/pci/devices fresh, writing
 * up to cap entries into out. Returns the count written (0 if neither
 * sysfs tree is present -- e.g. this dev environment's own kernel
 * lacking USB support entirely is a normal, non-fatal case, not an
 * error).
 */
int device_enumerate(struct discovered_device *out, int cap);

/*
 * device_enumerate() plus a linear search for id. NULL if not found.
 * Re-walks sysfs on every call, same as device_enumerate() itself --
 * simple and correct rather than cached, since this isn't a hot path
 * (bounded by CONTAINER_MAX_DEVICES lookups per container-create
 * request) and host hardware can change between calls (a USB device
 * unplugged and replugged elsewhere).
 */
const struct discovered_device *device_find(const char *id);

void device_write_json_one(const struct discovered_device *d, struct json_writer *w);

/* Calls device_enumerate() itself, then writes the whole list. */
void device_write_json_list(struct json_writer *w);

#endif /* DEVICE_H */
