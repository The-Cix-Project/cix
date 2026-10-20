#include "device.h"

#include "disk.h"
#include "diskrole.h"
#include "diskpart.h"

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
#define NET_CLASS_DIR "/sys/class/net"
#define DRM_CLASS_DIR "/sys/class/drm"
/* The one shared, system-wide ROCm/HSA compute device -- see
 * enumerate_gpu()'s own comment on why this isn't a per-card DRM node. */
#define KFD_CLASS_DEV "/sys/class/kfd/kfd"
/* A generous fixed bound on distinct physical GPUs in one host --
 * mirrors CONTAINER_MAX_DEVICES's own "generous fixed bound" reasoning
 * (container.h), not a real hardware limit. */
#define GPU_MAX_CARDS 16

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

/*
 * ADR-0344: the canonical /sys/devices path a record's parent is found
 * by. Every walk below reaches its device through a class or bus
 * symlink (/sys/class/net/eth0, /sys/bus/pci/devices/...), and those
 * are flat; the kernel's real device tree is what the symlinks resolve
 * to, and it is the only place one device is visibly below another.
 * Empty when it does not resolve, which leaves the record a root.
 */
static void canonical_sysfs(const char *path, char *out, size_t out_size)
{
	char resolved[PATH_MAX];

	out[0] = '\0';
	if (realpath(path, resolved) != NULL)
		snprintf(out, out_size, "%s", resolved);
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

/*
 * ADR-0161 Phase A: /sys/bus/usb/devices/ is a flat directory --
 * "2-1" (the whole device) and "2-1:1.0"/"2-1:1.1"/... (its own real
 * interfaces) all appear as siblings there, not nested. usb_name_is_
 * device()'s own top-level walk already skips the interface entries
 * outright; this reads them back for device_basename specifically (a
 * second, small opendir() pass over the same already-open sysfs
 * directory, bounded to the handful of interface entries one real
 * device actually has -- not a genuinely separate discovery
 * mechanism, no recursive tree walk anywhere else in the codebase).
 */
static void enumerate_usb_interfaces(const char *device_basename, struct discovered_device *e)
{
	DIR *d;
	struct dirent *ent;
	size_t prefix_len = strlen(device_basename);

	d = opendir(USB_BUS_DIR);
	if (d == NULL)
		return;

	while (e->interface_count < USB_MAX_INTERFACES_REPORTED && (ent = readdir(d)) != NULL) {
		char attr[PATH_MAX];
		char ifpath[PATH_MAX];
		char numbuf[8] = "";
		struct usb_interface_info *info;

		if (strncmp(ent->d_name, device_basename, prefix_len) != 0 ||
		    ent->d_name[prefix_len] != ':')
			continue;

		if (snprintf(ifpath, sizeof(ifpath), "%s/%s", USB_BUS_DIR, ent->d_name) >=
		    (int)sizeof(ifpath))
			continue;

		info = &e->interfaces[e->interface_count];
		memset(info, 0, sizeof(*info));

		snprintf(attr, sizeof(attr), "%s/bInterfaceNumber", ifpath);
		if (read_sysfs_attr(attr, numbuf, sizeof(numbuf)) == 0)
			info->number = (int)strtol(numbuf, NULL, 16);
		snprintf(attr, sizeof(attr), "%s/bInterfaceClass", ifpath);
		read_sysfs_attr(attr, info->class_hex, sizeof(info->class_hex));
		snprintf(attr, sizeof(attr), "%s/bInterfaceSubClass", ifpath);
		read_sysfs_attr(attr, info->subclass_hex, sizeof(info->subclass_hex));
		snprintf(attr, sizeof(attr), "%s/bInterfaceProtocol", ifpath);
		read_sysfs_attr(attr, info->protocol_hex, sizeof(info->protocol_hex));

		e->interface_count++;
	}
	closedir(d);
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
	e->requires_driver = 1;
	canonical_sysfs(base, e->sysfs, sizeof(e->sysfs));

	{
		const char *device_basename = strrchr(base, '/');

		device_basename = (device_basename != NULL) ? device_basename + 1 : base;
		enumerate_usb_interfaces(device_basename, e);
	}

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
			canonical_sysfs(dir_path, e->sysfs, sizeof(e->sysfs));
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

	if (*count >= cap)
		return;

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

	/*
	 * ADR-0344: the function itself is always a record. It is the parent
	 * its disks, DRM nodes and interfaces hang off, which is how a reader
	 * sees that vda is the virtio disk at 0000:06:0a.0. It owns no node:
	 * the nodes below it are its children's, or, for one no more specific
	 * walk claims, a "pci:<bdf>:<node>" record of its own. This used to
	 * be emitted only when no node was found and never for a network or
	 * display controller, to avoid reporting one card twice -- the tree
	 * is what avoids that now, since a parent is not a duplicate.
	 */
	{
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
			         "PCI device %s:%s, class %s, driver %s", vendor_id, product_id,
			         class_hex, driver);
		else
			snprintf(e->description, sizeof(e->description),
			         "PCI device %s:%s, class %s, no driver bound", vendor_id, product_id,
			         class_hex);
		canonical_sysfs(base, e->sysfs, sizeof(e->sysfs));
	}

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
}

/* --- net bus: real, physically-backed network interfaces only --
 * every netdev the kernel creates itself (bridges, veths, dummy,
 * loopback, tun/tap, ...) resolves under /sys/devices/virtual/net/,
 * confirmed live -- the reliable way to tell cix's own managed
 * bridges/veths (netplane/src/rtnetlink.c) and any other purely
 * software interface apart from something a container could
 * meaningfully take real ownership of. Assignment itself (moving one
 * into a container's netns) is a wholly separate mechanism from the
 * BPF_CGROUP_DEVICE grant PCI/USB devices use above -- see
 * container_net_host_attach_interfaces() -- a netdev has no /dev
 * node at all. --- */

static int enumerate_net_one(const char *ifname, struct discovered_device *e)
{
	char link_path[PATH_MAX];
	char resolved[PATH_MAX];
	char addr_attr[PATH_MAX];
	char mac[32] = "";
	ssize_t n;

	snprintf(link_path, sizeof(link_path), "%s/%s", NET_CLASS_DIR, ifname);
	n = readlink(link_path, resolved, sizeof(resolved) - 1);
	if (n < 0)
		return -1;
	resolved[n] = '\0';
	if (strstr(resolved, "/virtual/net/") != NULL)
		return -1; /* cix's own bridges/veths, or any other software netdev */

	memset(e, 0, sizeof(*e));
	snprintf(e->bus, sizeof(e->bus), "net");
	snprintf(e->id, sizeof(e->id), "net:%s", ifname);

	snprintf(addr_attr, sizeof(addr_attr), "%s/address", link_path);
	read_sysfs_attr(addr_attr, mac, sizeof(mac));
	if (mac[0] != '\0')
		snprintf(e->description, sizeof(e->description), "network interface %s, mac %s", ifname,
		         mac);
	else
		snprintf(e->description, sizeof(e->description), "network interface %s", ifname);

	read_driver_name(link_path, e->driver, sizeof(e->driver));
	canonical_sysfs(link_path, e->sysfs, sizeof(e->sysfs));

	/*
	 * No /dev node, no BPF_CGROUP_DEVICE grant, no major:minor -- a
	 * netdev is moved wholesale into a container's own netns instead
	 * (container_net_host_attach_interfaces()). Once moved it stops
	 * appearing here, because this walk sees only the daemon's own
	 * netns; since ADR-0344 that is no longer how anyone learns it is
	 * held -- the grant recorded at the time is (devicegrant.c).
	 *
	 * Enslavement to a bridge is read from the kernel's own `master`
	 * symlink, which exists for exactly as long as the interface is
	 * enslaved to anything, rather than from a second table of who
	 * attached what.
	 */
	{
		char master_path[PATH_MAX];
		char master_target[PATH_MAX];
		ssize_t m;

		snprintf(master_path, sizeof(master_path), "%s/master", link_path);
		m = readlink(master_path, master_target, sizeof(master_target) - 1);
		if (m >= 0) {
			const char *base;

			master_target[m] = '\0';
			base = strrchr(master_target, '/');
			snprintf(e->master, sizeof(e->master), "%s", base != NULL ? base + 1 : master_target);
			e->bridge_port = 1;
		}
	}

	/*
	 * A radio's interface hangs off its PHY, which is the unit that moves
	 * (ADR-0265): every interface on the wiphy goes with it. The kernel
	 * names the PHY through `phy80211`, and the PHY is a sibling of the
	 * interface in the device tree rather than its ancestor, so this is
	 * the one parent not found by path.
	 */
	{
		char phy_path[PATH_MAX];
		char phy_target[PATH_MAX];
		ssize_t p;

		snprintf(phy_path, sizeof(phy_path), "%s/phy80211", link_path);
		p = readlink(phy_path, phy_target, sizeof(phy_target) - 1);
		if (p >= 0) {
			const char *base;

			phy_target[p] = '\0';
			base = strrchr(phy_target, '/');
			snprintf(e->parent, sizeof(e->parent), "phy:%s", base != NULL ? base + 1 : phy_target);
		}
	}
	return 0;
}

/* --- phy bus: a wireless PHY, the unit a radio moves into a container
 * as (ADR-0265). It owns no /dev node; it is granted by naming one of
 * its interfaces in interfaces[], and its interfaces are its children. --- */

#define IEEE80211_CLASS_DIR "/sys/class/ieee80211"

static void enumerate_phy(struct discovered_device *out, int cap, int *count)
{
	DIR *d;
	struct dirent *ent;

	d = opendir(IEEE80211_CLASS_DIR);
	if (d == NULL)
		return;

	while (*count < cap && (ent = readdir(d)) != NULL) {
		char path[PATH_MAX];
		char device_dir[PATH_MAX];
		struct discovered_device *e;

		if (ent->d_name[0] == '.')
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", IEEE80211_CLASS_DIR, ent->d_name) >=
		    (int)sizeof(path))
			continue;

		e = &out[(*count)++];
		memset(e, 0, sizeof(*e));
		snprintf(e->bus, sizeof(e->bus), "phy");
		snprintf(e->id, sizeof(e->id), "phy:%s", ent->d_name);
		snprintf(e->description, sizeof(e->description), "wireless PHY %s", ent->d_name);
		snprintf(device_dir, sizeof(device_dir), "%s/device", path);
		read_driver_name(device_dir, e->driver, sizeof(e->driver));
		canonical_sysfs(path, e->sysfs, sizeof(e->sysfs));
	}
	closedir(d);
}

static void enumerate_net(struct discovered_device *out, int cap, int *count)
{
	DIR *d;
	struct dirent *ent;

	d = opendir(NET_CLASS_DIR);
	if (d == NULL)
		return;

	while (*count < cap && (ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		if (enumerate_net_one(ent->d_name, &out[*count]) == 0)
			(*count)++;
	}
	closedir(d);
}

/* --- gpu bus: DRM display/render nodes for a real GPU (AMD amdgpu is
 * the confirmed target hardware -- see ADR-0028), grouped under one
 * gpu:<idx> logical id per physical card so a container can request
 * everything a GPU needs (card + render nodes) in a single grant via
 * device_find_group() rather than naming each node by hand. Unlike
 * usb/pci/net above, this bus's own ids are two-tier: "gpu:<idx>:
 * <node>" (this function's own output, one entry per real node) plus
 * the bare "gpu:<idx>" logical id device_find_group() alone knows how
 * to expand -- never emitted here as its own discovered_device. --- */

/* Resolves entry_path's own "device" symlink (e.g.
 * <DRM_CLASS_DIR>/card0/device) to its parent PCI device's own BDF
 * address (the symlink target's basename) -- the stable key used to
 * group multiple DRM nodes (card0, renderD128, ...) belonging to the
 * same physical GPU under one gpu:<idx>. Returns 0 and fills out, or
 * -1 if entry_path has no "device" symlink at all. */
static int drm_parent_pci_address(const char *entry_path, char *out, size_t out_size)
{
	char link_path[PATH_MAX];
	char resolved[PATH_MAX];
	ssize_t n;
	const char *base;

	snprintf(link_path, sizeof(link_path), "%s/device", entry_path);
	n = readlink(link_path, resolved, sizeof(resolved) - 1);
	if (n < 0)
		return -1;
	resolved[n] = '\0';
	base = strrchr(resolved, '/');
	base = (base != NULL) ? base + 1 : resolved;
	snprintf(out, out_size, "%s", base);
	return 0;
}

static void enumerate_gpu(struct discovered_device *out, int cap, int *count)
{
	DIR *d;
	struct dirent *ent;
	char seen_addr[GPU_MAX_CARDS][32];
	int seen_count = 0;

	d = opendir(DRM_CLASS_DIR);
	if (d == NULL)
		return;

	while (*count < cap && (ent = readdir(d)) != NULL) {
		char entry_path[PATH_MAX];
		char attr[PATH_MAX];
		char devbuf[32];
		char pci_addr[32];
		char vendor_raw[16] = "", device_raw[16] = "", class_raw[16] = "";
		char vendor_id[16], product_id[16], class_hex[8];
		char driver[64];
		unsigned int major, minor;
		int idx, i;
		struct discovered_device *e;

		if (ent->d_name[0] == '.')
			continue;
		if (snprintf(entry_path, sizeof(entry_path), "%s/%s", DRM_CLASS_DIR, ent->d_name) >=
		    (int)sizeof(entry_path))
			continue;

		/* Connector subdirs (e.g. "card0-DP-1") have no "dev" file --
		 * only a real device node (cardN, renderDN) does. */
		snprintf(attr, sizeof(attr), "%s/dev", entry_path);
		if (read_sysfs_attr(attr, devbuf, sizeof(devbuf)) != 0)
			continue;
		if (sscanf(devbuf, "%u:%u", &major, &minor) != 2)
			continue;

		if (drm_parent_pci_address(entry_path, pci_addr, sizeof(pci_addr)) != 0)
			continue;

		/* Stable index: first-seen order among unique parent PCI
		 * addresses, so every node belonging to the same physical GPU
		 * (card0 + renderD128, ...) lands under the same gpu:<idx>. */
		idx = -1;
		for (i = 0; i < seen_count; i++) {
			if (strcmp(seen_addr[i], pci_addr) == 0) {
				idx = i;
				break;
			}
		}
		if (idx < 0) {
			if (seen_count >= GPU_MAX_CARDS)
				continue;
			snprintf(seen_addr[seen_count], sizeof(seen_addr[seen_count]), "%s", pci_addr);
			idx = seen_count++;
		}

		snprintf(attr, sizeof(attr), "/sys/bus/pci/devices/%s/vendor", pci_addr);
		read_sysfs_attr(attr, vendor_raw, sizeof(vendor_raw));
		snprintf(attr, sizeof(attr), "/sys/bus/pci/devices/%s/device", pci_addr);
		read_sysfs_attr(attr, device_raw, sizeof(device_raw));
		snprintf(attr, sizeof(attr), "/sys/bus/pci/devices/%s/class", pci_addr);
		read_sysfs_attr(attr, class_raw, sizeof(class_raw));
		strip_0x(vendor_raw, vendor_id, sizeof(vendor_id));
		strip_0x(device_raw, product_id, sizeof(product_id));
		strip_0x(class_raw, class_hex, sizeof(class_hex));

		snprintf(attr, sizeof(attr), "/sys/bus/pci/devices/%s", pci_addr);
		read_driver_name(attr, driver, sizeof(driver));

		e = &out[(*count)++];
		memset(e, 0, sizeof(*e));
		snprintf(e->bus, sizeof(e->bus), "gpu");
		snprintf(e->id, sizeof(e->id), "gpu:%d:%s", idx, ent->d_name);
		snprintf(e->vendor_id, sizeof(e->vendor_id), "%s", vendor_id);
		snprintf(e->product_id, sizeof(e->product_id), "%s", product_id);
		snprintf(e->class_hex, sizeof(e->class_hex), "%s", class_hex);
		snprintf(e->driver, sizeof(e->driver), "%s", driver);
		snprintf(e->description, sizeof(e->description), "%s (gpu %d, pci %s, driver %s)",
		         ent->d_name, idx, pci_addr, driver[0] != '\0' ? driver : "?");
		e->type = DEVICE_NODE_CHAR;
		e->major = major;
		e->minor = minor;
		snprintf(e->dev_path, sizeof(e->dev_path), "/dev/dri/%s", ent->d_name);
		e->requires_driver = 1;
		canonical_sysfs(entry_path, e->sysfs, sizeof(e->sysfs));
	}
	closedir(d);

	/*
	 * /dev/kfd (ROCm/HSA compute) is one shared, system-wide node -- not
	 * per-card, since the kfd driver's topology multiplexes across every
	 * GPU the host has. A container needs it alongside its render node
	 * for ROCm compute to work, and granting it exposes KFD's queue
	 * submission host-wide, which is ROCm's own architecture (ADR-0029).
	 *
	 * ONE record, "gpu:kfd" (ADR-0344). It used to be emitted once per
	 * GPU group as "gpu:<idx>:kfd" -- one node under N ids, which is what
	 * the one-node-one-record gate exists to refuse. device_find_group()
	 * adds it to every "gpu:<idx>" expansion instead, and always_shared
	 * says what ADR-0029 already did: two containers each with their own
	 * GPU both need it, so it never conflicts.
	 */
	if (seen_count > 0 && *count < cap) {
		char attr[PATH_MAX];
		char devbuf[32];
		unsigned int major, minor;

		snprintf(attr, sizeof(attr), "%s/dev", KFD_CLASS_DEV);
		if (read_sysfs_attr(attr, devbuf, sizeof(devbuf)) == 0 &&
		    sscanf(devbuf, "%u:%u", &major, &minor) == 2) {
			struct discovered_device *e = &out[(*count)++];

			memset(e, 0, sizeof(*e));
			snprintf(e->bus, sizeof(e->bus), "gpu");
			snprintf(e->id, sizeof(e->id), "gpu:kfd");
			snprintf(e->description, sizeof(e->description),
			         "ROCm/HSA compute device (host-wide, shared, %s)", KFD_CLASS_DEV);
			e->type = DEVICE_NODE_CHAR;
			e->major = major;
			e->minor = minor;
			snprintf(e->dev_path, sizeof(e->dev_path), "/dev/kfd");
			e->always_shared = 1;
			canonical_sysfs(KFD_CLASS_DEV, e->sysfs, sizeof(e->sysfs));
		}
	}
}

/*
 * ADR-0142 raw disk passthrough, reshaped by ADR-0344. EVERY disk and
 * partition disk_enumerate() reports is a record, "disk:<name>", with
 * the facts that decide whether the host is using it: the OS layout,
 * a storage role, a mount. This walk used to skip the OS disk and any
 * disk with a role, and the PCI walk below reached some of those anyway
 * under another id -- so on 192.168.15.95 `vda` (roled, mounted) was
 * offered as assignable while `sda` (roled, mounted) did not exist at
 * all, and the OS disk `vdb` was assignable through its PCI id. A disk
 * the host uses is a fact an operator needs; devicestate.c says it is
 * the host's, and every grant asks devicestate.c.
 *
 * No kernel driver concept exists for a disk the way a USB device has
 * one, so a disk never requires one.
 */
static void enumerate_disk(struct discovered_device *out, int cap, int *count,
                            const char *os_containers_dir)
{
	static struct discovered_disk disks[DISK_ENUM_MAX];
	int n = disk_enumerate(disks, DISK_ENUM_MAX, os_containers_dir);
	int i;

	for (i = 0; i < n && *count < cap; i++) {
		char attr[PATH_MAX];
		char devbuf[32];
		unsigned int major, minor;
		struct discovered_device *e;

		snprintf(attr, sizeof(attr), "/sys/class/block/%s/dev", disks[i].name);
		if (read_sysfs_attr(attr, devbuf, sizeof(devbuf)) != 0)
			continue;
		if (sscanf(devbuf, "%u:%u", &major, &minor) != 2)
			continue;

		e = &out[(*count)++];
		memset(e, 0, sizeof(*e));
		snprintf(e->bus, sizeof(e->bus), "disk");
		snprintf(e->id, sizeof(e->id), "disk:%s", disks[i].name);
		if (disks[i].is_partition)
			snprintf(e->description, sizeof(e->description), "partition %s of %s (%lld bytes)",
			         disks[i].name, disks[i].parent_disk, (long long)disks[i].size_bytes);
		else
			snprintf(e->description, sizeof(e->description), "%s (%lld bytes%s)",
			         disks[i].model[0] != '\0' ? disks[i].model : disks[i].name,
			         (long long)disks[i].size_bytes, disks[i].removable ? ", removable" : "");
		e->type = DEVICE_NODE_BLOCK;
		e->major = major;
		e->minor = minor;
		snprintf(e->dev_path, sizeof(e->dev_path), "%s", disks[i].dev_path);
		e->is_partition = disks[i].is_partition;
		e->os_layout = diskpart_os_layout_untouchable(disks[i].name, disks[i].is_os_disk,
		                                              disks[i].is_partition);
		e->storage_role = diskrole_lookup(disks[i].name) != NULL;
		e->mounted = disks[i].mounted;
		snprintf(attr, sizeof(attr), "/sys/class/block/%s", disks[i].name);
		canonical_sysfs(attr, e->sysfs, sizeof(e->sysfs));
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

/*
 * ADR-0344: each /dev node belongs to the most specific walk that
 * reaches it. The PCI walk finds every node below a function -- a disk,
 * a DRM card -- and the class walks find the same nodes as what they
 * are, so a "pci:<bdf>:<node>" record whose node a class walk also
 * reported is dropped. What survives under a "pci:" id is a node nothing
 * more specific owns, such as an NVMe controller's own char node.
 *
 * On 192.168.15.95 this is the difference between `card0` reported once
 * and `card0` reported twice with opposite answers (2026-10-20).
 */
static int drop_pci_duplicates(struct discovered_device *out, int count)
{
	int i, j, kept = 0;

	for (i = 0; i < count; i++) {
		int duplicate = 0;

		if (strcmp(out[i].bus, "pci") == 0 && out[i].dev_path[0] != '\0') {
			for (j = 0; j < count && !duplicate; j++) {
				duplicate = j != i && strcmp(out[j].bus, "pci") != 0 &&
				            out[j].dev_path[0] != '\0' && out[j].type == out[i].type &&
				            out[j].major == out[i].major && out[j].minor == out[i].minor;
			}
		}
		if (duplicate)
			continue;
		if (kept != i)
			out[kept] = out[i];
		kept++;
	}
	return kept;
}

/*
 * A record's parent is the nearest record above it in the kernel's own
 * device tree: the one whose canonical path is the longest strict
 * prefix of its own. That is how a disk sits under its PCI function, a
 * partition under its disk, a USB device under its host controller, a
 * PHY under its USB device -- one rule, whatever directory the kernel
 * chose. On 192.168.15.95 card0's `device` link names a platform
 * framebuffer that is not a record, which is exactly why the rule walks
 * up to the nearest RECORD rather than reading that link. A record that
 * already has a parent -- an interface naming its PHY -- keeps it.
 */
static void assign_parents(struct discovered_device *out, int count)
{
	int i, j;

	for (i = 0; i < count; i++) {
		size_t best = 0;
		int best_j = -1;

		if (out[i].parent[0] != '\0' || out[i].sysfs[0] == '\0')
			continue;
		for (j = 0; j < count; j++) {
			size_t len = strlen(out[j].sysfs);

			if (j == i || len == 0 || len <= best)
				continue;
			if (strncmp(out[i].sysfs, out[j].sysfs, len) == 0 && out[i].sysfs[len] == '/') {
				best = len;
				best_j = j;
			}
		}
		if (best_j >= 0)
			snprintf(out[i].parent, sizeof(out[i].parent), "%s", out[best_j].id);
	}
}

int device_enumerate(struct discovered_device *out, int cap, const char *os_containers_dir)
{
	int count = 0;

	enumerate_usb(out, cap, &count);
	enumerate_pci(out, cap, &count);
	enumerate_phy(out, cap, &count);
	enumerate_net(out, cap, &count);
	enumerate_gpu(out, cap, &count);
	enumerate_disk(out, cap, &count, os_containers_dir);
	count = drop_pci_duplicates(out, count);
	assign_parents(out, count);
	return count;
}

const struct discovered_device *device_find(const char *id, const char *os_containers_dir)
{
	static struct discovered_device cache[DEVICE_ENUM_MAX];
	int n = device_enumerate(cache, DEVICE_ENUM_MAX, os_containers_dir);
	int i;

	for (i = 0; i < n; i++) {
		if (strcmp(cache[i].id, id) == 0)
			return &cache[i];
	}
	return NULL;
}

int device_find_group(const char *id, const char *os_containers_dir,
                       const struct discovered_device *out[], int cap)
{
	static struct discovered_device cache[DEVICE_ENUM_MAX];
	int n = device_enumerate(cache, DEVICE_ENUM_MAX, os_containers_dir);
	char prefix[100];
	size_t prefix_len;
	int i, found = 0;

	for (i = 0; i < n; i++) {
		if (strcmp(cache[i].id, id) == 0) {
			if (cap < 1)
				return -1;
			out[0] = &cache[i];
			return 1;
		}
	}

	/* No exact match -- a bare "gpu:<idx>" expands into every member
	 * node with a driver, plus the host-wide gpu:kfd. Only "gpu:" ids
	 * expand: a "pci:<bdf>" function is a strict prefix of its own
	 * "pci:<bdf>:<node>" children, and naming a function must not grant
	 * its nodes behind the tree's back. */
	if (strncmp(id, "gpu:", 4) != 0)
		return -1;
	snprintf(prefix, sizeof(prefix), "%s:", id);
	prefix_len = strlen(prefix);
	for (i = 0; i < n && found < cap; i++) {
		if (strncmp(cache[i].id, prefix, prefix_len) == 0 && cache[i].driver[0] != '\0')
			out[found++] = &cache[i];
	}
	if (found == 0)
		return -1;
	for (i = 0; i < n && found < cap; i++) {
		if (strcmp(cache[i].id, "gpu:kfd") == 0)
			out[found++] = &cache[i];
	}
	return found;
}
