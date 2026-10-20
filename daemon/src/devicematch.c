#include "devicematch.h"
#include "devicestate.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/*
 * ADR-0345. Each kind has one meaning, and only a scoped value is an
 * identity: a USB serial counts only with its vendor and product, since
 * the TP-Link dongle on 192.168.15.95 reports 123456 like every unit of
 * its model (measured 2026-10-20), and a MAC only when the kernel says
 * it is the burned-in one.
 */

static const char *const KIND_WORDS[] = {
	[DEVICEMATCH_EXACT] = "exact",
	[DEVICEMATCH_VENDOR_MODEL] = "vendor_model",
	[DEVICEMATCH_USB_SERIAL] = "usb_serial",
	[DEVICEMATCH_WWID] = "wwid",
	[DEVICEMATCH_FS_UUID] = "fs_uuid",
	[DEVICEMATCH_MAC] = "mac",
};

#define KIND_COUNT ((int)(sizeof(KIND_WORDS) / sizeof(KIND_WORDS[0])))

int devicematch_kind_parse(const char *word, enum devicematch_kind *out)
{
	int k;

	if (word == NULL)
		return -1;
	for (k = 0; k < KIND_COUNT; k++) {
		if (strcmp(word, KIND_WORDS[k]) == 0) {
			*out = (enum devicematch_kind)k;
			return 0;
		}
	}
	return -1;
}

const char *devicematch_kind_word(enum devicematch_kind kind)
{
	return (int)kind >= 0 && (int)kind < KIND_COUNT ? KIND_WORDS[kind] : "?";
}

int devicematch_exclusive(enum devicematch_kind kind)
{
	return kind != DEVICEMATCH_VENDOR_MODEL;
}

/* Four hex digits, lowercased into out (5 bytes). sysfs writes idVendor
 * and idProduct, and PCI's own ids, that way. */
static int hex4(const char *s, char *out)
{
	int i;

	for (i = 0; i < 4; i++) {
		if (!isxdigit((unsigned char)s[i]))
			return -1;
		out[i] = (char)tolower((unsigned char)s[i]);
	}
	out[4] = '\0';
	return 0;
}

/* "vvvv:pppp" then the rest. Returns a pointer to what follows, or NULL. */
static const char *vid_pid(const char *s, char *vid, char *pid)
{
	if (hex4(s, vid) != 0 || s[4] != ':' || hex4(s + 5, pid) != 0)
		return NULL;
	return s + 9;
}

static int mac_normal(const char *s, char *out, size_t out_size)
{
	int i;

	if (strlen(s) != 17 || out_size < 18)
		return -1;
	for (i = 0; i < 17; i++) {
		if (i % 3 == 2) {
			if (s[i] != ':')
				return -1;
			out[i] = ':';
		} else {
			if (!isxdigit((unsigned char)s[i]))
				return -1;
			out[i] = (char)tolower((unsigned char)s[i]);
		}
	}
	out[17] = '\0';
	return 0;
}

/* exact names a location; every other bus's id is a kernel name or an
 * index, and the refusal says what names that bus's devices instead. */
static int check_exact(const char *selector, char *err, size_t err_size)
{
	if (strncmp(selector, "usb:", 4) == 0 || strncmp(selector, "pci:", 4) == 0)
		return 0;
	if (strncmp(selector, "disk:", 5) == 0)
		snprintf(err, err_size,
		         "exact names a location, and %s is a kernel name: name a disk by wwid or "
		         "fs_uuid",
		         selector);
	else if (strncmp(selector, "net:", 4) == 0 || strncmp(selector, "phy:", 4) == 0)
		snprintf(err, err_size,
		         "exact names a location, and %s is a kernel name: name an interface by mac",
		         selector);
	else if (strncmp(selector, "gpu:", 4) == 0)
		snprintf(err, err_size,
		         "exact names a location, and %s is an enumeration index; a GPU cannot be "
		         "named yet (ADR-0345)",
		         selector);
	else
		snprintf(err, err_size, "exact takes a usb: or pci: device id, not %s", selector);
	return -1;
}

int devicematch_check_selector(enum devicematch_kind kind, const char *selector,
                               const struct discovered_device *recs, int n, char *out,
                               size_t out_size, char *err, size_t err_size)
{
	char vid[5], pid[5];
	const char *rest;
	size_t len;
	int i;

	if (selector == NULL || selector[0] == '\0') {
		snprintf(err, err_size, "selector must not be empty");
		return -1;
	}
	len = strlen(selector);
	if (len >= out_size || len >= DEVICEMATCH_SELECTOR_MAX) {
		snprintf(err, err_size, "selector is longer than %d bytes", DEVICEMATCH_SELECTOR_MAX - 1);
		return -1;
	}

	switch (kind) {
	case DEVICEMATCH_EXACT:
		if (check_exact(selector, err, err_size) != 0)
			return -1;
		snprintf(out, out_size, "%s", selector);
		return 0;
	case DEVICEMATCH_VENDOR_MODEL:
		rest = vid_pid(selector, vid, pid);
		if (rest == NULL || rest[0] != '\0') {
			snprintf(err, err_size, "vendor_model takes vendor_id:product_id, e.g. 1058:2630");
			return -1;
		}
		snprintf(out, out_size, "%s:%s", vid, pid);
		return 0;
	case DEVICEMATCH_USB_SERIAL:
		rest = vid_pid(selector, vid, pid);
		if (rest == NULL || rest[0] != ':' || rest[1] == '\0') {
			snprintf(err, err_size,
			         "usb_serial takes vendor_id:product_id:serial, e.g. 2357:012e:123456 -- a "
			         "serial alone is not an identity");
			return -1;
		}
		snprintf(out, out_size, "%s:%s:%s", vid, pid, rest + 1);
		return 0;
	case DEVICEMATCH_WWID:
		/* The kernel pads the T10 form; the record carries it without
		 * the padding, and so does a selector. */
		while (len > 0 && isspace((unsigned char)selector[len - 1]))
			len--;
		if (len == 0) {
			snprintf(err, err_size, "selector must not be empty");
			return -1;
		}
		snprintf(out, out_size, "%.*s", (int)len, selector);
		return 0;
	case DEVICEMATCH_FS_UUID:
		for (i = 0; selector[i] != '\0'; i++) {
			if (!isxdigit((unsigned char)selector[i]) && selector[i] != '-') {
				snprintf(err, err_size,
				         "fs_uuid takes a filesystem UUID: hex digits and '-' only");
				return -1;
			}
		}
		snprintf(out, out_size, "%s", selector);
		return 0;
	case DEVICEMATCH_MAC:
		if (mac_normal(selector, out, out_size) != 0) {
			snprintf(err, err_size, "mac takes an address like 98:25:4a:5b:de:5b");
			return -1;
		}
		for (i = 0; i < n; i++) {
			if (strcmp(recs[i].bus, "net") == 0 && strcasecmp(recs[i].mac, out) == 0 &&
			    !recs[i].mac_permanent) {
				snprintf(err, err_size,
				         "%s is %s's current address, not its permanent one, so it can "
				         "change and cannot name the interface",
				         out, recs[i].id);
				return -1;
			}
		}
		return 0;
	}
	snprintf(err, err_size, "unknown kind");
	return -1;
}

static int matches(enum devicematch_kind kind, const char *selector,
                   const struct discovered_device *d, devicematch_fs_uuid_fn probe)
{
	char vid[5], pid[5];
	const char *rest;

	switch (kind) {
	case DEVICEMATCH_EXACT:
		return strcmp(d->id, selector) == 0;
	case DEVICEMATCH_VENDOR_MODEL:
		/* A grant unit, not a verdict: a PCI function shares its vendor
		 * ids with its node children and has nothing of its own to
		 * grant. Whether a match may be granted is the grant gate's
		 * answer (ADR-0344). */
		rest = vid_pid(selector, vid, pid);
		return rest != NULL && device_is_grant_unit(d) && strcmp(d->vendor_id, vid) == 0 &&
		       strcmp(d->product_id, pid) == 0;
	case DEVICEMATCH_USB_SERIAL:
		rest = vid_pid(selector, vid, pid);
		return rest != NULL && rest[0] == ':' && strcmp(d->bus, "usb") == 0 &&
		       strcmp(d->vendor_id, vid) == 0 && strcmp(d->product_id, pid) == 0 &&
		       strcmp(d->serial, rest + 1) == 0;
	case DEVICEMATCH_WWID:
		return strcmp(d->bus, "disk") == 0 && d->wwid[0] != '\0' && strcmp(d->wwid, selector) == 0;
	case DEVICEMATCH_FS_UUID:
		if (strcmp(d->bus, "disk") != 0 || d->dev_path[0] == '\0' || probe == NULL)
			return 0;
		{
			char uuid[64];

			probe(d->dev_path, uuid, sizeof(uuid));
			return uuid[0] != '\0' && strcasecmp(uuid, selector) == 0;
		}
	case DEVICEMATCH_MAC:
		return strcmp(d->bus, "net") == 0 && d->mac_permanent && strcasecmp(d->mac, selector) == 0;
	}
	return 0;
}

int devicematch_find(enum devicematch_kind kind, const char *selector,
                     const struct discovered_device *recs, int n, devicematch_fs_uuid_fn probe,
                     int out[], int cap)
{
	int i, found = 0;

	for (i = 0; i < n; i++) {
		if (!matches(kind, selector, &recs[i], probe))
			continue;
		if (found < cap)
			out[found] = i;
		found++;
	}
	return found;
}
