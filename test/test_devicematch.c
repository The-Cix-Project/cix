/*
 * ADR-0345: a record id says where a device is; a mapping says which
 * device it is.
 *
 * The fixture is 192.168.15.95's hardware as measured on 2026-10-20 by
 * 0.2.57-558 -- the TP-Link dongle reporting serial 123456, eth0 and
 * wlan0 with permanent addresses, sda's padded WWID, a QEMU display
 * adapter on PCI -- plus what .95 does not have and the rules must still
 * get right: a second dongle of the same model with the same factory
 * serial in another port, another vendor's device that also says 123456,
 * an interface whose address is not permanent, and two disks carrying
 * filesystems.
 *
 * Pure: records in, matches out, with a fake superblock reader. In
 * SELFTESTS, because no test can plug in a second dongle.
 */
#include "devicematch.h"
#include "devicestate.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

/* sda's WWID on .95 without the 13 spaces the kernel pads it with. */
#define SDA_WWID "t10.ATA     QEMU HARDDISK                           QM00013"

static struct discovered_device g_recs[16];
static int g_n;

static struct discovered_device *rec(const char *id, const char *bus)
{
	struct discovered_device *d = &g_recs[g_n++];

	memset(d, 0, sizeof(*d));
	snprintf(d->id, sizeof(d->id), "%s", id);
	snprintf(d->bus, sizeof(d->bus), "%s", bus);
	return d;
}

static void usb(const char *id, const char *vid, const char *pid, const char *serial)
{
	struct discovered_device *d = rec(id, "usb");

	snprintf(d->vendor_id, sizeof(d->vendor_id), "%s", vid);
	snprintf(d->product_id, sizeof(d->product_id), "%s", pid);
	snprintf(d->serial, sizeof(d->serial), "%s", serial);
	snprintf(d->dev_path, sizeof(d->dev_path), "/dev/bus/usb/x");
}

static void net(const char *id, const char *mac, int permanent)
{
	struct discovered_device *d = rec(id, "net");

	snprintf(d->mac, sizeof(d->mac), "%s", mac);
	d->mac_permanent = permanent;
}

static void disk(const char *id, const char *dev_path, const char *wwid)
{
	struct discovered_device *d = rec(id, "disk");

	snprintf(d->dev_path, sizeof(d->dev_path), "%s", dev_path);
	snprintf(d->wwid, sizeof(d->wwid), "%s", wwid);
}

static int g_probes;

/* sda5 and vdb5 carry filesystems; every other node reads as none. */
static void fake_fs_uuid(const char *dev_path, char *out, size_t out_size)
{
	g_probes++;
	if (strcmp(dev_path, "/dev/sda5") == 0)
		snprintf(out, out_size, "6F0B4E2A-91C3-4D7E-8A55-1B2C3D4E5F60");
	else if (strcmp(dev_path, "/dev/vdb5") == 0)
		snprintf(out, out_size, "0a1b2c3d-0000-4000-8000-00000000abcd");
	else
		out[0] = '\0';
}

static void build_fixture(void)
{
	g_n = 0;
	usb("usb:2357:012e@0000:07:1b.0-usb2-1", "2357", "012e", "123456");
	usb("usb:2357:012e@0000:07:1b.0-usb2-2", "2357", "012e", "123456");
	usb("usb:0bda:8179@0000:07:1b.0-usb2-3", "0bda", "8179", "123456");
	net("net:eth0", "bc:24:11:42:0f:4f", 1);
	net("net:wlan0", "98:25:4a:5b:de:5b", 1);
	net("net:wlan1", "02:11:22:33:44:55", 0);
	disk("disk:sda", "/dev/sda", SDA_WWID);
	disk("disk:sda5", "/dev/sda5", "");
	disk("disk:vdb5", "/dev/vdb5", "");
	disk("disk:sr0", "/dev/sr0", "");
	{
		struct discovered_device *d = rec("pci:0000:00:01.0:card0", "pci");

		snprintf(d->vendor_id, sizeof(d->vendor_id), "1234");
		snprintf(d->product_id, sizeof(d->product_id), "1111");
		snprintf(d->dev_path, sizeof(d->dev_path), "/dev/card0");
	}
}

/* Checks the selector, then finds it; expects <count> matches and,
 * when count is 1, that it is <id>. */
static void expect(enum devicematch_kind kind, const char *selector, int count, const char *id)
{
	char normal[DEVICEMATCH_SELECTOR_MAX];
	char err[256];
	int idx[8];
	int found;

	if (devicematch_check_selector(kind, selector, g_recs, g_n, normal, sizeof(normal), err,
	                               sizeof(err)) != 0) {
		fprintf(stderr, "  FAIL: %s %s refused: %s\n", devicematch_kind_word(kind), selector,
		        err);
		g_failures++;
		return;
	}
	found = devicematch_find(kind, normal, g_recs, g_n, fake_fs_uuid, idx, 8);
	if (found != count || (count == 1 && strcmp(g_recs[idx[0]].id, id) != 0)) {
		fprintf(stderr, "  FAIL: %s %s matched %d (%s), expected %d (%s)\n",
		        devicematch_kind_word(kind), selector, found,
		        found > 0 ? g_recs[idx[0]].id : "-", count, id != NULL ? id : "-");
		g_failures++;
	}
}

/* Expects the selector refused, with a sentence containing <phrase>. */
static void refused(enum devicematch_kind kind, const char *selector, const char *phrase)
{
	char normal[DEVICEMATCH_SELECTOR_MAX];
	char err[256] = "";

	if (devicematch_check_selector(kind, selector, g_recs, g_n, normal, sizeof(normal), err,
	                               sizeof(err)) == 0) {
		fprintf(stderr, "  FAIL: %s %s was accepted\n", devicematch_kind_word(kind), selector);
		g_failures++;
	} else if (strstr(err, phrase) == NULL) {
		fprintf(stderr, "  FAIL: %s %s refused as \"%s\", without \"%s\"\n",
		        devicematch_kind_word(kind), selector, err, phrase);
		g_failures++;
	}
}

/* The factory serial: two of the model in two ports, and another
 * vendor saying the same thing. */
static void test_usb_serial(void)
{
	build_fixture();
	expect(DEVICEMATCH_USB_SERIAL, "2357:012e:123456", 2, NULL);
	if (!devicematch_exclusive(DEVICEMATCH_USB_SERIAL)) {
		fprintf(stderr, "  FAIL: usb_serial is not exclusive, so two matches would be granted\n");
		g_failures++;
	}
	/* Scoped by vendor and product: the other vendor's 123456 is its own. */
	expect(DEVICEMATCH_USB_SERIAL, "0BDA:8179:123456", 1, "usb:0bda:8179@0000:07:1b.0-usb2-3");
	refused(DEVICEMATCH_USB_SERIAL, "123456", "a serial alone is not an identity");
	/* The fix for the ambiguity: the port. */
	expect(DEVICEMATCH_EXACT, "usb:2357:012e@0000:07:1b.0-usb2-2", 1,
	       "usb:2357:012e@0000:07:1b.0-usb2-2");
	/* vendor_model is the group kind: both dongles, and that is no error. */
	expect(DEVICEMATCH_VENDOR_MODEL, "2357:012E", 2, NULL);
	if (devicematch_exclusive(DEVICEMATCH_VENDOR_MODEL)) {
		fprintf(stderr, "  FAIL: vendor_model is exclusive, so a group would be ambiguous\n");
		g_failures++;
	}
}

static void test_mac(void)
{
	build_fixture();
	expect(DEVICEMATCH_MAC, "98:25:4A:5B:DE:5B", 1, "net:wlan0");
	refused(DEVICEMATCH_MAC, "02:11:22:33:44:55", "not its permanent one");
	refused(DEVICEMATCH_MAC, "98254a5bde5b", "an address like");
	/* An address nobody has now may be named ahead of the hardware, and
	 * still never matches one the kernel did not burn in. */
	expect(DEVICEMATCH_MAC, "aa:bb:cc:dd:ee:ff", 0, NULL);
	g_recs[5].mac_permanent = 0;
	snprintf(g_recs[5].mac, sizeof(g_recs[5].mac), "aa:bb:cc:dd:ee:ff");
	{
		int idx[2];

		if (devicematch_find(DEVICEMATCH_MAC, "aa:bb:cc:dd:ee:ff", g_recs, g_n, fake_fs_uuid,
		                     idx, 2) != 0) {
			fprintf(stderr, "  FAIL: a mac mapping matched an address that is not permanent\n");
			g_failures++;
		}
	}
}

static void test_disks(void)
{
	build_fixture();
	/* The padding the kernel writes is not part of the identity. */
	expect(DEVICEMATCH_WWID, SDA_WWID "             ",
	       1, "disk:sda");
	expect(DEVICEMATCH_FS_UUID, "6f0b4e2a-91c3-4d7e-8a55-1b2c3d4e5f60", 1, "disk:sda5");
	refused(DEVICEMATCH_FS_UUID, "not a uuid", "hex digits");
	/* A superblock read happens only for an fs_uuid selector. */
	g_probes = 0;
	expect(DEVICEMATCH_WWID, SDA_WWID, 1,
	       "disk:sda");
	if (g_probes != 0) {
		fprintf(stderr, "  FAIL: a wwid lookup read %d superblocks\n", g_probes);
		g_failures++;
	}
}

static void test_exact_is_a_location(void)
{
	build_fixture();
	refused(DEVICEMATCH_EXACT, "disk:sda", "wwid or fs_uuid");
	refused(DEVICEMATCH_EXACT, "net:wlan0", "by mac");
	refused(DEVICEMATCH_EXACT, "gpu:0", "cannot be named yet");
	expect(DEVICEMATCH_EXACT, "pci:0000:00:01.0:card0", 1, "pci:0000:00:01.0:card0");
}

/* One id, one record: the gate that says so if device.c ever builds an
 * id that is not a location again. */
static void test_one_id_one_record(void)
{
	int a, b;

	build_fixture();
	if (device_records_share_an_id(g_recs, g_n, &a, &b)) {
		fprintf(stderr, "  FAIL: the fixture's location ids collided: %s\n", g_recs[a].id);
		g_failures++;
	}
	/* What the old id scheme built for two dongles: usb:vid:pid:serial. */
	snprintf(g_recs[0].id, sizeof(g_recs[0].id), "usb:2357:012e:123456");
	snprintf(g_recs[1].id, sizeof(g_recs[1].id), "usb:2357:012e:123456");
	if (!device_records_share_an_id(g_recs, g_n, &a, &b) || a != 0 || b != 1) {
		fprintf(stderr, "  FAIL: two records with one id were not caught\n");
		g_failures++;
	}
}

int main(void)
{
	printf("test_devicematch:\n");
	test_usb_serial();
	test_mac();
	test_disks();
	test_exact_is_a_location();
	test_one_id_one_record();
	if (g_failures != 0) {
		printf("test_devicematch: %d FAILED\n", g_failures);
		return 1;
	}
	printf("test_devicematch: PASS\n");
	return 0;
}
