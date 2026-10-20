#ifndef DEVICEMATCH_H
#define DEVICEMATCH_H

#include "device.h"

#include <stddef.h>

/*
 * ADR-0345: how a device mapping finds its device, as pure functions of
 * a list of records. A record's id says where a device is; a mapping's
 * kind and selector say which device it is. devicemap.c keeps the
 * mappings and calls these; test_devicematch drives them with fixture
 * records, because no test can plug in a second dongle.
 */

/* The largest selector any kind carries: a WWID (device.h's field). */
#define DEVICEMATCH_SELECTOR_MAX 256

enum devicematch_kind {
	DEVICEMATCH_EXACT,	  /* a usb: or pci: id -- a location */
	DEVICEMATCH_VENDOR_MODEL, /* vid:pid -- every device of one model */
	DEVICEMATCH_USB_SERIAL,	  /* vid:pid:serial -- one USB device */
	DEVICEMATCH_WWID,	  /* one disk */
	DEVICEMATCH_FS_UUID,	  /* one disk or partition, by its filesystem */
	DEVICEMATCH_MAC,	  /* one interface, by its permanent address */
};

/* 0 and *out set for a kind word the contract names, -1 otherwise. */
int devicematch_kind_parse(const char *word, enum devicematch_kind *out);
const char *devicematch_kind_word(enum devicematch_kind kind);

/* Every kind but vendor_model names one device: matching several is
 * ambiguous, never a choice. */
int devicematch_exclusive(enum devicematch_kind kind);

/*
 * Whether <selector> can name anything as <kind>, written in its one
 * stored form into out (hex and MACs lowercased, a WWID's trailing
 * padding removed). recs are the records present now, consulted for one
 * thing: a MAC that is present and not permanent is refused, because it
 * can change. 0, or -1 with a sentence in err that names the kind to
 * use instead where there is one.
 */
int devicematch_check_selector(enum devicematch_kind kind, const char *selector,
                               const struct discovered_device *recs, int n, char *out,
                               size_t out_size, char *err, size_t err_size);

/*
 * Indexes into recs of the records <kind>/<selector> matches, at most cap
 * of them written to out. Returns how many match, which can exceed cap.
 * Every kind compares a field of the record, so nothing here reads a
 * device.
 */
int devicematch_find(enum devicematch_kind kind, const char *selector,
                     const struct discovered_device *recs, int n, int out[], int cap);

#endif /* DEVICEMATCH_H */
