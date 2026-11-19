#ifndef CIX_FWSCAN_H
#define CIX_FWSCAN_H

#include "device.h"
#include "json.h"
#include "kmod.h"

/*
 * ADR-0381 release 1b: which firmware each device's driver declares,
 * and which of it the host lacks.
 *
 * A device's modules are the driver bound to its own sysfs directory and
 * to its direct children (a USB device's driver binds to its interfaces,
 * the device itself carries the generic "usb"), with each one's
 * modules.dep closure. The declaration often sits in a module the bound
 * one depends on: rtw88's firmware is declared in rtw88_8822b, while the
 * dongle binds rtw88_8822bu. Each module's names come from its own
 * .modinfo (kmod_declared_firmware()), and a name is present when the
 * firmware link or /lib/firmware holds it.
 *
 * fwscan_scan() reads sysfs and module files, so it runs only in a helper
 * child; main.c installs the result with fwscan_install() and reads it
 * through fwscan_write_device(). The result is one flat struct, so it
 * travels down a memfd as bytes.
 */

#define FWSCAN_DEVICES_MAX DEVICE_ENUM_MAX
#define FWSCAN_DEVICE_MODULES_MAX 16
#define FWSCAN_MODULES_MAX 256
#define FWSCAN_NAMES_MAX 8192
#define FWSCAN_NAME_MAX 160

#define FWSCAN_PKG_MAX 64 /* a package name or version, PKG_NAME_MAX / PKG_VERSION_MAX */

struct fwscan_name {
	char name[FWSCAN_NAME_MAX];
	unsigned char present;   /* the firmware image or the root holds it */
	unsigned char in_image;  /* the firmware image holds it: what acquisition asks (release 3) */
	unsigned char ambiguous; /* two outputs claim it */
	char provider[FWSCAN_PKG_MAX]; /* the output claiming it, "" for none */
	char recipe[FWSCAN_PKG_MAX];   /* that output's recipe */
	char version[FWSCAN_PKG_MAX];  /* the recipe's current version */
};

struct fwscan_module {
	char name[KMOD_NAME_MAX];
	int first, count; /* into names */
	int unreadable;   /* its file could not be read */
};

struct fwscan_device {
	char id[DEVICE_ID_MAX];
	int modules[FWSCAN_DEVICE_MODULES_MAX]; /* into modules */
	int module_count;
};

struct fwscan_result {
	int device_count, module_count, name_count;
	int truncated; /* a table filled up: what is listed is right, and incomplete */
	struct fwscan_device devices[FWSCAN_DEVICES_MAX];
	struct fwscan_module modules[FWSCAN_MODULES_MAX];
	struct fwscan_name names[FWSCAN_NAMES_MAX];
};

/*
 * Fills r for the devices device_enumerate() finds, checking each name
 * under firmware_dir (the link) and root_firmware_dir (/lib/firmware).
 * Helper child only.
 */
void fwscan_scan(struct fwscan_result *r, const char *firmware_dir, const char *root_firmware_dir,
                 const char *os_containers_dir);

/*
 * ADR-0381 release 3: fills each name the firmware image lacks with the
 * output whose files selectors claim lib/firmware/<name>, from walk
 * (pkg_firmware_providers_walk()). Two outputs claiming one name make it
 * ambiguous. Walks nothing when the image lacks nothing. Helper child.
 */
typedef int (*fwscan_provider_walk)(void (*fn)(const char *output, const char *recipe,
                                               const char *version, const char *pattern,
                                               int glob, void *ctx),
                                    void *ctx);
void fwscan_match_providers(struct fwscan_result *r, fwscan_provider_walk walk);

/* Installs a scan's result. 1 when what a device record shows changed. */
int fwscan_install(const struct fwscan_result *r);

/*
 * ADR-0381 release 3: each (device, module, name) the installed scan
 * found absent from the firmware image, for the daemon's acquisition.
 */
void fwscan_for_each_absent(void (*fn)(const char *device, const char *module,
                                       const struct fwscan_name *n, void *ctx),
                            void *ctx);

/*
 * Writes the record's "firmware" key for device id: null when no scan
 * has run or the device has no driver module, else
 * {"modules": [...], "declared": N, "missing": [...], "complete": bool}.
 */
void fwscan_write_device(struct json_writer *w, const char *id);

#endif
