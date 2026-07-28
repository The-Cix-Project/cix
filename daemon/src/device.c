#include "device.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define USB_BUS_DIR "/sys/bus/usb/devices"
#define PCI_BUS_DIR "/sys/bus/pci/devices"
#define PCI_WALK_MAX_DEPTH 4

/*
 * Reads a small sysfs text attribute into out, trimming a trailing
 * newline. Leaves out empty (not an error the caller needs to check
 * for most fields -- e.g. a USB device's own "serial" or "manufacturer"
 * legitimately doesn't exist on plenty of real hardware) unless
 * documented otherwise at the call site.
 */
static int read_sysfs_attr(const char *path, char *out, size_t out_size)
{
	FILE *f;
	size_t n;

	out[0] = '\0';
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	if (fgets(out, (int)out_size, f) == NULL) {
		fclose(f);
		out[0] = '\0';
		return -1;
	}
	fclose(f);
	n = strlen(out);
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return 0;
}

/* Basename of the symlink at <device_dir>/driver, or empty if unbound. */
static void read_driver_name(const char *device_dir, char *out, size_t out_size)
{
	char link_path[PATH_MAX];
	char resolved[PATH_MAX];
	ssize_t n;
	const char *base;

	out[0] = '\0';
	if (snprintf(link_path, sizeof(link_path), "%s/driver", device_dir) >= (int)sizeof(link_path))
		return;
	n = readlink(link_path, resolved, sizeof(resolved) - 1);
	if (n < 0)
		return;
	resolved[n] = '\0';
	base = strrchr(resolved, '/');
	base = (base != NULL) ? base + 1 : resolved;
	snprintf(out, out_size, "%s", base);
}

/* PCI's vendor/device/class sysfs files read as "0x1234"; USB's
 * idVendor/idProduct read as "1234" with no prefix. Normalize PCI's
 * own fields to the same unprefixed form so ids/display are
 * consistent across both buses. */
static void strip_0x(const char *in, char *out, size_t out_size)
{
	if (in[0] == '0' && (in[1] == 'x' || in[1] == 'X'))
		in += 2;
	snprintf(out, out_size, "%s", in);
}

/* Basename of the symlink at <dir_path>/subsystem, e.g. "block" or
 * "nvme" -- the reliable way to tell a discovered node's own type
 * apart (confirmed live: a namespace's own subsystem symlink resolves
 * to /sys/class/block, its owning controller's to /sys/class/nvme). */
static int node_is_block(const char *dir_path)
{
	char link_path[PATH_MAX];
	char resolved[PATH_MAX];
	ssize_t n;
	const char *base;

	if (snprintf(link_path, sizeof(link_path), "%s/subsystem", dir_path) >= (int)sizeof(link_path))
		return 0;
	n = readlink(link_path, resolved, sizeof(resolved) - 1);
	if (n < 0)
		return 0;
	resolved[n] = '\0';
	base = strrchr(resolved, '/');
	base = (base != NULL) ? base + 1 : resolved;
	return strcmp(base, "block") == 0;
}

/* --- USB bus: a flat, driver-independent view of every USB device,
 * including any owned by a PCI USB host controller -- so the PCI walk
 * below deliberately never descends into a host controller's own
 * "usbN" subtree, to avoid enumerating the same device twice. --- */

/* Device directories look like "2-1" or "3-3.1" (no ':'); per-
 * interface subdirectories of a device look like "2-1:1.0". v1 assigns
 * whole devices, not individual interfaces. */
static int usb_name_is_device(const char *name)
{
	return strchr(name, ':') == NULL;
}

static int enumerate_usb_one(const char *base, struct discovered_device *e)
{
	char attr[PATH_MAX];
	char cls[8] = "";
	char devbuf[32] = "";
	char busnum[16] = "";
	char devnum[16] = "";
	char vendor[16] = "";
	char product[16] = "";
	char serial[80] = "";
	char devpath[32] = "";
	char manufacturer[64] = "";
	char product_name[64] = "";
	unsigned int major, minor;

	snprintf(attr, sizeof(attr), "%s/bDeviceClass", base);
	read_sysfs_attr(attr, cls, sizeof(cls));
	if (strcmp(cls, "09") == 0)
		return -1; /* hub -- not something a container can meaningfully own */

	snprintf(attr, sizeof(attr), "%s/dev", base);
	if (read_sysfs_attr(attr, devbuf, sizeof(devbuf)) != 0)
		return -1; /* no usbfs node -- nothing a container could open */
	if (sscanf(devbuf, "%u:%u", &major, &minor) != 2)
		return -1;

	snprintf(attr, sizeof(attr), "%s/busnum", base);
	read_sysfs_attr(attr, busnum, sizeof(busnum));
	snprintf(attr, sizeof(attr), "%s/devnum", base);
	read_sysfs_attr(attr, devnum, sizeof(devnum));
	snprintf(attr, sizeof(attr), "%s/idVendor", base);
	read_sysfs_attr(attr, vendor, sizeof(vendor));
	snprintf(attr, sizeof(attr), "%s/idProduct", base);
	read_sysfs_attr(attr, product, sizeof(product));
	snprintf(attr, sizeof(attr), "%s/serial", base);
	read_sysfs_attr(attr, serial, sizeof(serial));
	snprintf(attr, sizeof(attr), "%s/devpath", base);
	read_sysfs_attr(attr, devpath, sizeof(devpath));
	snprintf(attr, sizeof(attr), "%s/manufacturer", base);
	read_sysfs_attr(attr, manufacturer, sizeof(manufacturer));
	snprintf(attr, sizeof(attr), "%s/product", base);
	read_sysfs_attr(attr, product_name, sizeof(product_name));

	memset(e, 0, sizeof(*e));
	snprintf(e->bus, sizeof(e->bus), "usb");
	snprintf(e->vendor_id, sizeof(e->vendor_id), "%s", vendor);
	snprintf(e->product_id, sizeof(e->product_id), "%s", product);

	/* serial is the only field genuinely stable across a re-plug for
	 * most real devices; busnum/devnum are reassigned on every
	 * enumeration and must never be used in a stable id. The port-
	 * topology path (devpath) is a weaker but still-real fallback for
	 * the (common) case of a device with no serial number at all. */
	if (serial[0] != '\0')
		snprintf(e->id, sizeof(e->id), "usb:%s:%s:%s", vendor, product, serial);
	else
		snprintf(e->id, sizeof(e->id), "usb:%s:%s:port%s-%s", vendor, product, busnum,
		         devpath);

	if (manufacturer[0] != '\0' || product_name[0] != '\0')
		snprintf(e->description, sizeof(e->description), "%s %s", manufacturer,
		         product_name);
	else
		snprintf(e->description, sizeof(e->description), "USB device %s:%s", vendor,
		         product);

	read_driver_name(base, e->driver, sizeof(e->driver));

	/* usbfs device nodes are always char devices (USB_DEVICE_MAJOR is
	 * a char major) -- no need to probe this the way the PCI walk
	 * below has to. */
	e->type = DEVICE_NODE_CHAR;
	e->major = major;
	e->minor = minor;
	snprintf(e->dev_path, sizeof(e->dev_path), "/dev/bus/usb/%03u/%03u",
	         (unsigned)strtoul(busnum, NULL, 10), (unsigned)strtoul(devnum, NULL, 10));
	e->assignable = (e->driver[0] != '\0');
	return 0;
}

static void enumerate_usb(struct discovered_device *out, int cap, int *count)
{
	DIR *d;
	struct dirent *ent;

	d = opendir(USB_BUS_DIR);
	if (d == NULL)
		return;

	while (*count < cap && (ent = readdir(d)) != NULL) {
		char base[PATH_MAX];

		if (ent->d_name[0] == '.' || !usb_name_is_device(ent->d_name))
			continue;
		if (snprintf(base, sizeof(base), "%s/%s", USB_BUS_DIR, ent->d_name) >=
		    (int)sizeof(base))
			continue;
		if (enumerate_usb_one(base, &out[*count]) == 0)
			(*count)++;
	}
	closedir(d);
}

/* --- PCI bus: the vendor/device/class/driver are read once per PCI
 * device; the actual /dev node(s) it owns are found by a bounded-depth
 * walk of its own sysfs subtree, since the relationship is driver-
 * specific and not uniform (a simple device might expose nothing, an
 * NVMe controller exposes both its own controller node and one node
 * per namespace, nested two levels deep). --- */

struct pci_dev_ctx {
	const char *address;
	const char *vendor_id;
	const char *product_id;
	const char *class_hex;
	const char *driver;
};

/* nvme0n1p1 is a partition of nvme0n1 -- v1 assigns whole disks/
 * namespaces only, per the same "whole device, not a sub-piece" scope
 * boundary usb_name_is_device() already applies to USB interfaces. */
/*
 * A PCI bridge/root port's own sysfs directory contains a real
 * subdirectory for each device attached downstream of it, named after
 * that device's own BDF address (e.g. 0000:00:1d.0/0000:02:00.0/...),
 * mirroring the physical bus topology -- confirmed live on this dev
 * host (a bridge and the NVMe controller behind it both "own" the
 * identical nvme0/nvme0n1 nodes via this nesting). Descending into one
 * would just re-discover a device enumerate_pci() already visits
 * independently via its own top-level /sys/bus/pci/devices/<addr>
 * entry, producing duplicate entries for the same physical hardware.
 */
static int looks_like_pci_address(const char *name)
{
	int i;

	if (strlen(name) != 12)
		return 0;
	for (i = 0; i < 4; i++) {
		if (!isxdigit((unsigned char)name[i]))
			return 0;
	}
	if (name[4] != ':')
		return 0;
	for (i = 5; i < 7; i++) {
		if (!isxdigit((unsigned char)name[i]))
			return 0;
	}
	if (name[7] != ':')
		return 0;
	for (i = 8; i < 10; i++) {
		if (!isxdigit((unsigned char)name[i]))
			return 0;
	}
	if (name[10] != '.')
		return 0;
	return isxdigit((unsigned char)name[11]) != 0;
}

static int is_partition_name(const char *parent_name, const char *child_name)
{
	size_t plen = strlen(parent_name);
	size_t i;

	if (strncmp(child_name, parent_name, plen) != 0)
		return 0;
	if (child_name[plen] != 'p' || child_name[plen + 1] == '\0')
		return 0;
	for (i = plen + 1; child_name[i] != '\0'; i++) {
		if (!isdigit((unsigned char)child_name[i]))
			return 0;
	}
	return 1;
}

static void walk_pci_dev_nodes(const char *dir_path, const char *node_name,
                                const char *parent_name, int depth,
                                const struct pci_dev_ctx *ctx, struct discovered_device *out,
                                int cap, int *count)
{
	char attr[PATH_MAX];
	char devbuf[32];
	DIR *d;
	struct dirent *ent;

	if (depth > PCI_WALK_MAX_DEPTH || *count >= cap)
		return;
	if (parent_name != NULL && is_partition_name(parent_name, node_name))
		return;

	if (snprintf(attr, sizeof(attr), "%s/dev", dir_path) < (int)sizeof(attr) &&
	    read_sysfs_attr(attr, devbuf, sizeof(devbuf)) == 0) {
		unsigned int major, minor;

		if (sscanf(devbuf, "%u:%u", &major, &minor) == 2 && *count < cap) {
			struct discovered_device *e = &out[(*count)++];

			memset(e, 0, sizeof(*e));
			snprintf(e->bus, sizeof(e->bus), "pci");
			snprintf(e->id, sizeof(e->id), "pci:%s:%s", ctx->address, node_name);
			snprintf(e->vendor_id, sizeof(e->vendor_id), "%s", ctx->vendor_id);
			snprintf(e->product_id, sizeof(e->product_id), "%s", ctx->product_id);
			snprintf(e->class_hex, sizeof(e->class_hex), "%s", ctx->class_hex);
			snprintf(e->driver, sizeof(e->driver), "%s", ctx->driver);
			snprintf(e->description, sizeof(e->description), "%s (pci %s, driver %s)",
			         node_name, ctx->address,
			         ctx->driver[0] != '\0' ? ctx->driver : "?");
			e->type = node_is_block(dir_path) ? DEVICE_NODE_BLOCK : DEVICE_NODE_CHAR;
			e->major = major;
			e->minor = minor;
			snprintf(e->dev_path, sizeof(e->dev_path), "/dev/%s", node_name);
			e->assignable = 1;
		}
	}

	d = opendir(dir_path);
	if (d == NULL)
		return;
	while (*count < cap && (ent = readdir(d)) != NULL) {
		char child_path[PATH_MAX];
		struct stat st;

		if (ent->d_name[0] == '.')
			continue;
		if (looks_like_pci_address(ent->d_name))
			continue;
		if (snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, ent->d_name) >=
		    (int)sizeof(child_path))
			continue;
		/* Symlinks (driver, subsystem, firmware_node, device, ...)
		 * are never part of this walk -- only a real subdirectory can
		 * hold another kobject's own "dev" file, and following
		 * symlinks like "subsystem"/"device" would walk back out of
		 * this device's own subtree entirely. */
		if (lstat(child_path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		walk_pci_dev_nodes(child_path, ent->d_name, node_name, depth + 1, ctx, out, cap,
		                    count);
	}
	closedir(d);
}

static void enumerate_pci_one(const char *base, const char *address, struct discovered_device *out,
                               int cap, int *count)
{
	char attr[PATH_MAX];
	char vendor_raw[16] = "", device_raw[16] = "", class_raw[16] = "";
	char vendor_id[16], product_id[16], class_hex[8];
	char driver[64];
	struct pci_dev_ctx ctx;
	DIR *d;
	struct dirent *ent;
	int before = *count;

	snprintf(attr, sizeof(attr), "%s/vendor", base);
	read_sysfs_attr(attr, vendor_raw, sizeof(vendor_raw));
	snprintf(attr, sizeof(attr), "%s/device", base);
	read_sysfs_attr(attr, device_raw, sizeof(device_raw));
	snprintf(attr, sizeof(attr), "%s/class", base);
	read_sysfs_attr(attr, class_raw, sizeof(class_raw));
	strip_0x(vendor_raw, vendor_id, sizeof(vendor_id));
	strip_0x(device_raw, product_id, sizeof(product_id));
	strip_0x(class_raw, class_hex, sizeof(class_hex));
	read_driver_name(base, driver, sizeof(driver));

	ctx.address = address;
	ctx.vendor_id = vendor_id;
	ctx.product_id = product_id;
	ctx.class_hex = class_hex;
	ctx.driver = driver;

	d = opendir(base);
	if (d != NULL) {
		while (*count < cap && (ent = readdir(d)) != NULL) {
			char child_path[PATH_MAX];
			struct stat st;

			if (ent->d_name[0] == '.')
				continue;
			/* A PCI USB host controller's own owned devices appear
			 * nested here too (usbN/...) -- already covered, flatly
			 * and driver-independently, by enumerate_usb(); walking
			 * them again here would just duplicate every entry. */
			if (strncmp(ent->d_name, "usb", 3) == 0 &&
			    isdigit((unsigned char)ent->d_name[3]))
				continue;
			/* A bridge/root port's direct children include the real
			 * downstream device(s) attached to it, named by their own
			 * BDF address (see looks_like_pci_address()'s own
			 * comment) -- enumerate_pci()'s own top-level loop already
			 * visits that device independently. */
			if (looks_like_pci_address(ent->d_name))
				continue;
			if (snprintf(child_path, sizeof(child_path), "%s/%s", base, ent->d_name) >=
			    (int)sizeof(child_path))
				continue;
			if (lstat(child_path, &st) != 0 || !S_ISDIR(st.st_mode))
				continue;
			walk_pci_dev_nodes(child_path, ent->d_name, NULL, 1, &ctx, out, cap, count);
		}
		closedir(d);
	}

	if (*count == before && *count < cap) {
		/* Nothing owned found anywhere in this device's own subtree
		 * (a bridge, an unbound device, a network card -- see
		 * ROADMAP.md's own scoping note on why NICs are handled by
		 * the existing network-attachment feature, not this one).
		 * Still surfaced, unassignable, so an operator can see the
		 * device exists and why it isn't offered. */
		struct discovered_device *e = &out[(*count)++];

		memset(e, 0, sizeof(*e));
		snprintf(e->bus, sizeof(e->bus), "pci");
		snprintf(e->id, sizeof(e->id), "pci:%s", address);
		snprintf(e->vendor_id, sizeof(e->vendor_id), "%s", vendor_id);
		snprintf(e->product_id, sizeof(e->product_id), "%s", product_id);
		snprintf(e->class_hex, sizeof(e->class_hex), "%s", class_hex);
		snprintf(e->driver, sizeof(e->driver), "%s", driver);
		if (driver[0] != '\0')
			snprintf(e->description, sizeof(e->description),
			         "PCI device %s:%s, class %s, driver %s (no owned /dev node found)",
			         vendor_id, product_id, class_hex, driver);
		else
			snprintf(e->description, sizeof(e->description),
			         "PCI device %s:%s, class %s, no driver bound", vendor_id,
			         product_id, class_hex);
		e->assignable = 0;
	}
}

static void enumerate_pci(struct discovered_device *out, int cap, int *count)
{
	DIR *d;
	struct dirent *ent;

	d = opendir(PCI_BUS_DIR);
	if (d == NULL)
		return;

	while (*count < cap && (ent = readdir(d)) != NULL) {
		char base[PATH_MAX];

		if (ent->d_name[0] == '.')
			continue;
		if (snprintf(base, sizeof(base), "%s/%s", PCI_BUS_DIR, ent->d_name) >=
		    (int)sizeof(base))
			continue;
		enumerate_pci_one(base, ent->d_name, out, cap, count);
	}
	closedir(d);
}

int device_enumerate(struct discovered_device *out, int cap)
{
	int count = 0;

	enumerate_usb(out, cap, &count);
	enumerate_pci(out, cap, &count);
	return count;
}

const struct discovered_device *device_find(const char *id)
{
	static struct discovered_device cache[DEVICE_ENUM_MAX];
	int n = device_enumerate(cache, DEVICE_ENUM_MAX);
	int i;

	for (i = 0; i < n; i++) {
		if (strcmp(cache[i].id, id) == 0)
			return &cache[i];
	}
	return NULL;
}

void device_write_json_one(const struct discovered_device *d, struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "id");
	jw_str(w, d->id);
	jw_key(w, "bus");
	jw_str(w, d->bus);
	jw_key(w, "vendor_id");
	jw_str(w, d->vendor_id);
	jw_key(w, "product_id");
	jw_str(w, d->product_id);
	jw_key(w, "class");
	jw_str(w, d->class_hex);
	jw_key(w, "description");
	jw_str(w, d->description);
	jw_key(w, "driver");
	jw_str(w, d->driver);
	jw_key(w, "dev_path");
	jw_str(w, d->dev_path);
	jw_key(w, "type");
	jw_str(w, d->type == DEVICE_NODE_BLOCK ? "block" : "char");
	jw_key(w, "major");
	jw_int(w, d->major);
	jw_key(w, "minor");
	jw_int(w, d->minor);
	jw_key(w, "assignable");
	jw_bool(w, d->assignable);
	jw_obj_close(w);
}

void device_write_json_list(struct json_writer *w)
{
	struct discovered_device devices[DEVICE_ENUM_MAX];
	int n = device_enumerate(devices, DEVICE_ENUM_MAX);
	int i;

	jw_arr_open(w);
	for (i = 0; i < n; i++)
		device_write_json_one(&devices[i], w);
	jw_arr_close(w);
}
