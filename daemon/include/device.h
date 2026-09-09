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
	struct usb_interface_info interfaces[USB_MAX_INTERFACES_REPORTED];
	int interface_count;
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

/*
 * Who currently holds a device (#356).
 *
 * `assignable` answers "does the host consider this grantable" and has
 * never answered "is anything already using it" -- it is a driver-bound
 * check for USB, unconditional for PCI, and a not-enslaved check for a
 * netdev. Nothing reported the second question at all, and nothing
 * refuses a second grant, so the same USB/PCI/GPU device could be
 * granted to any number of containers with no way to notice.
 *
 * Supplied as a callback rather than looked up here, because device.c
 * knows nothing about the registry and must not learn: the same
 * dependency injection registry_write_json_list() already uses to ask
 * containerdef.c a question without either module depending on the
 * other. Pass NULL and every device reports an empty holder list, which
 * is what a context with no live containers (a test, a tool) should
 * say.
 *
 * Fills out with up to max container names and returns the count, or 0
 * for none. Names are truncated to DEVICE_HOLDER_NAME_MAX rather than
 * dropped -- a truncated name still tells an operator something is
 * holding it.
 */
#define DEVICE_HOLDER_NAME_MAX 64
#define DEVICE_HOLDERS_MAX 16

typedef int (*device_holders_fn)(const char *device_id,
                                  char out[][DEVICE_HOLDER_NAME_MAX], int max);

void device_write_json_one(const struct discovered_device *d, struct json_writer *w,
                            device_holders_fn holders);

/* Calls device_enumerate() itself, then writes the whole list. */
void device_write_json_list(struct json_writer *w, const char *os_containers_dir,
                             device_holders_fn holders);

#endif /* DEVICE_H */
