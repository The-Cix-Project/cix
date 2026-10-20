/*
 * ADR-0344: one record, one state, and every grant asks it.
 *
 * The fixture is 192.168.15.95's own shape as measured on 2026-10-20 --
 * the OS disk vdb with its structural and its ordinary partitions, vda
 * holding a storage role, the optical drive, the USB wifi dongle whose
 * radio ar-1 holds, eth0 as a port of the management bridge -- plus the
 * shapes .95 does not have and the rules must still get right: a USB
 * stick the host has mounted, a disk with free sibling partitions, a
 * driverless USB device, the host-wide ROCm node.
 *
 * Pure: records and claims in, verdicts out. In SELFTESTS, because a
 * build container's sysfs is not the box's and this logic needs none.
 */
#include "devicestate.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static struct discovered_device g_recs[32];
static int g_n;

static struct discovered_device *rec(const char *id, const char *bus, const char *parent)
{
	struct discovered_device *d = &g_recs[g_n++];

	memset(d, 0, sizeof(*d));
	snprintf(d->id, sizeof(d->id), "%s", id);
	snprintf(d->bus, sizeof(d->bus), "%s", bus);
	snprintf(d->parent, sizeof(d->parent), "%s", parent);
	return d;
}

static void node(struct discovered_device *d, enum device_node_type type, unsigned int major,
                 unsigned int minor, const char *path)
{
	d->type = type;
	d->major = major;
	d->minor = minor;
	snprintf(d->dev_path, sizeof(d->dev_path), "%s", path);
}

static void build_fixture(void)
{
	struct discovered_device *d;

	g_n = 0;

	/* The OS disk, behind its virtio PCI function. */
	rec("pci:0000:06:0c.0", "pci", "");
	d = rec("disk:vdb", "disk", "pci:0000:06:0c.0");
	node(d, DEVICE_NODE_BLOCK, 254, 16, "/dev/vdb");
	d->os_layout = 1;
	d = rec("disk:vdb1", "disk", "disk:vdb");
	node(d, DEVICE_NODE_BLOCK, 254, 17, "/dev/vdb1");
	d->is_partition = 1;
	d->os_layout = 1;
	d->mounted = 1;
	d = rec("disk:vdb5", "disk", "disk:vdb");
	node(d, DEVICE_NODE_BLOCK, 254, 21, "/dev/vdb5");
	d->is_partition = 1;
	d->mounted = 1;
	d = rec("disk:vdb6", "disk", "disk:vdb");
	node(d, DEVICE_NODE_BLOCK, 254, 22, "/dev/vdb6");
	d->is_partition = 1;

	/* vda: a storage role, mounted at /mnt/cix/vda. */
	rec("pci:0000:06:0a.0", "pci", "");
	d = rec("disk:vda", "disk", "pci:0000:06:0a.0");
	node(d, DEVICE_NODE_BLOCK, 254, 0, "/dev/vda");
	d->storage_role = 1;
	d->mounted = 1;

	d = rec("disk:sr0", "disk", "");
	node(d, DEVICE_NODE_BLOCK, 11, 0, "/dev/sr0");

	/* The USB wifi dongle and its radio. */
	rec("pci:0000:00:1d.7", "pci", "");
	d = rec("usb:2357:012e:123456", "usb", "pci:0000:00:1d.7");
	node(d, DEVICE_NODE_CHAR, 189, 1, "/dev/bus/usb/001/002");
	snprintf(d->driver, sizeof(d->driver), "usb");
	d->requires_driver = 1;
	rec("phy:phy0", "phy", "usb:2357:012e:123456");
	rec("net:wlan0", "net", "phy:phy0");
	rec("net:wlan1", "net", "phy:phy0");

	/* eth0 is a port of the bridge carrying the management address;
	 * eth1 a port of some other bridge; eth2 free. */
	d = rec("net:eth0", "net", "");
	d->bridge_port = 1;
	d->management = 1;
	d = rec("net:eth1", "net", "");
	d->bridge_port = 1;
	rec("net:eth2", "net", "");

	/* A USB device with no driver bound. */
	d = rec("usb:dead:beef:port1-1", "usb", "pci:0000:00:1d.7");
	node(d, DEVICE_NODE_CHAR, 189, 2, "/dev/bus/usb/001/003");
	d->requires_driver = 1;

	/* A USB stick whose partition the host has mounted. */
	d = rec("usb:0781:5583:stick", "usb", "pci:0000:00:1d.7");
	node(d, DEVICE_NODE_CHAR, 189, 3, "/dev/bus/usb/001/004");
	snprintf(d->driver, sizeof(d->driver), "usb-storage");
	d->requires_driver = 1;
	d = rec("disk:sdb", "disk", "usb:0781:5583:stick");
	node(d, DEVICE_NODE_BLOCK, 8, 16, "/dev/sdb");
	d = rec("disk:sdb1", "disk", "disk:sdb");
	node(d, DEVICE_NODE_BLOCK, 8, 17, "/dev/sdb1");
	d->is_partition = 1;
	d->mounted = 1;

	/* A free disk with two free partitions. */
	d = rec("disk:sdc", "disk", "");
	node(d, DEVICE_NODE_BLOCK, 8, 32, "/dev/sdc");
	d = rec("disk:sdc1", "disk", "disk:sdc");
	node(d, DEVICE_NODE_BLOCK, 8, 33, "/dev/sdc1");
	d->is_partition = 1;
	d = rec("disk:sdc2", "disk", "disk:sdc");
	node(d, DEVICE_NODE_BLOCK, 8, 34, "/dev/sdc2");
	d->is_partition = 1;

	/* A roled whole disk's partition: the role reaches down. */
	d = rec("disk:sdd", "disk", "");
	node(d, DEVICE_NODE_BLOCK, 8, 48, "/dev/sdd");
	d->storage_role = 1;
	d = rec("disk:sdd1", "disk", "disk:sdd");
	node(d, DEVICE_NODE_BLOCK, 8, 49, "/dev/sdd1");
	d->is_partition = 1;

	d = rec("gpu:kfd", "gpu", "");
	node(d, DEVICE_NODE_CHAR, 240, 0, "/dev/kfd");
	d->always_shared = 1;
}

static struct device_claim g_claims[16];
static int g_cn;

static void claim(enum device_claim_kind kind, const char *container, const char *id, int shared,
                  int binding)
{
	struct device_claim *c = &g_claims[g_cn++];

	memset(c, 0, sizeof(*c));
	c->kind = kind;
	snprintf(c->container, sizeof(c->container), "%s", container);
	snprintf(c->id, sizeof(c->id), "%s", id);
	c->shared = shared;
	c->binding = binding;
}

static void expect_state(const char *id, const char *state, const char *reason)
{
	struct device_verdict v;
	int idx = device_record_index(g_recs, g_n, id);

	if (idx < 0) {
		fprintf(stderr, "  FAIL: %s is not in the fixture\n", id);
		g_failures++;
		return;
	}
	device_state_of(g_recs, g_n, idx, g_claims, g_cn, &v);
	if (strcmp(device_state_word(v.state), state) != 0 || strcmp(v.reason, reason) != 0) {
		fprintf(stderr, "  FAIL: %s is %s/%s, expected %s/%s\n", id, device_state_word(v.state),
		        v.reason, state, reason);
		g_failures++;
	}
}

static void expect_grant(const char *id, const char *requester, int want_shared, int want_code,
                         const char *holder)
{
	struct device_verdict v;
	int idx = device_record_index(g_recs, g_n, id);
	int code;

	if (idx < 0) {
		fprintf(stderr, "  FAIL: %s is not in the fixture\n", id);
		g_failures++;
		return;
	}
	code = device_grant_verdict(g_recs, g_n, idx, g_claims, g_cn, want_shared, requester, &v);
	if (code != want_code) {
		fprintf(stderr, "  FAIL: granting %s to %s%s answered %d (%s/%s), expected %d\n", id,
		        requester, want_shared ? " (shared)" : "", code, device_state_word(v.state),
		        v.reason, want_code);
		g_failures++;
		return;
	}
	if (holder != NULL && (v.claim < 0 || strcmp(g_claims[v.claim].container, holder) != 0)) {
		fprintf(stderr, "  FAIL: granting %s to %s named %s, expected %s\n", id, requester,
		        v.claim >= 0 ? g_claims[v.claim].container : "(nobody)", holder);
		g_failures++;
	}
}

static void test_host_use(void)
{
	build_fixture();
	g_cn = 0;

	expect_state("disk:vdb", "host", "os-disk");
	expect_state("disk:vdb1", "host", "os-partition");
	expect_state("disk:vdb5", "host", "mounted");
	/* #9: the OS layout does not reach an operator's own partition. */
	expect_state("disk:vdb6", "free", "");
	expect_state("disk:vda", "host", "storage-role");
	expect_state("disk:sdd1", "host", "storage-role");
	expect_state("disk:sr0", "free", "");
	expect_state("net:eth0", "host", "management-interface");
	expect_state("net:eth1", "host", "bridge-port");
	expect_state("net:eth2", "free", "");
	/* A USB stick whose partition the host mounted is the host's. */
	expect_state("usb:0781:5583:stick", "host", "mounted");
	expect_state("disk:sdb", "host", "mounted");
	/* A PCI function says what it is, not what is below it. */
	expect_state("pci:0000:06:0c.0", "unavailable", "no-node");
	expect_state("pci:0000:06:0a.0", "unavailable", "no-node");
	expect_state("usb:dead:beef:port1-1", "unavailable", "no-driver");
	expect_state("usb:2357:012e:123456", "free", "");
	expect_state("phy:phy0", "free", "");

	expect_grant("disk:vdb", "probe", 0, 400, NULL);
	expect_grant("disk:vdb1", "probe", 0, 400, NULL);
	expect_grant("disk:vda", "probe", 0, 400, NULL);
	expect_grant("net:eth0", "probe", 0, 400, NULL);
	expect_grant("pci:0000:06:0c.0", "probe", 0, 400, NULL);
	expect_grant("usb:dead:beef:port1-1", "probe", 0, 400, NULL);
	expect_grant("disk:vdb6", "probe", 0, 0, NULL);
	expect_grant("disk:sr0", "probe", 0, 0, NULL);
}

static void test_tree_holding(void)
{
	build_fixture();
	g_cn = 0;
	/* ar-1 holds the radio: a wireless grant is a grant of the PHY. */
	claim(DEVICE_CLAIM_GRANT, "ar-1", "phy:phy0", 0, 0);
	claim(DEVICE_CLAIM_GRANT, "store", "disk:sdc1", 0, 0);

	expect_state("phy:phy0", "held", "");
	expect_state("net:wlan0", "held", "");
	expect_state("net:wlan1", "held", "");
	expect_state("usb:2357:012e:123456", "held", "");
	expect_state("pci:0000:00:1d.7", "unavailable", "no-node");
	expect_state("disk:sdc", "held", "");
	expect_state("disk:sdc2", "free", "");

	/* Two interfaces of one PHY conflict; the dongle's own USB node
	 * conflicts with the radio below it. */
	expect_grant("net:wlan1", "other", 0, 409, "ar-1");
	expect_grant("usb:2357:012e:123456", "other", 0, 409, "ar-1");
	/* The holder itself is never refused by its own grant. */
	expect_grant("phy:phy0", "ar-1", 0, 0, NULL);
	/* An ancestor conflicts; a sibling does not. */
	expect_grant("disk:sdc", "other", 0, 409, "store");
	expect_grant("disk:sdc2", "other", 0, 0, NULL);
}

static void test_sharing(void)
{
	build_fixture();
	g_cn = 0;
	claim(DEVICE_CLAIM_GRANT, "a", "disk:sr0", 1, 0);
	claim(DEVICE_CLAIM_GRANT, "b", "disk:sdc2", 0, 0);
	claim(DEVICE_CLAIM_GRANT, "a", "gpu:kfd", 0, 0);

	expect_grant("disk:sr0", "c", 1, 0, NULL);
	expect_grant("disk:sr0", "c", 0, 409, "a");
	/* Both sides must ask: a shared newcomer against an exclusive holder. */
	expect_grant("disk:sdc2", "c", 1, 409, "b");
	/* The host-wide ROCm node never conflicts. */
	expect_grant("gpu:kfd", "c", 0, 0, NULL);
	expect_state("gpu:kfd", "held", "");
}

static void test_reservation(void)
{
	build_fixture();
	g_cn = 0;
	/* Off by default: a stopped definition is reported, refuses nothing. */
	claim(DEVICE_CLAIM_RESERVATION, "planned", "disk:sdc1", 0, 0);
	/* reserve_devices: the router owns its radio while stopped. */
	claim(DEVICE_CLAIM_RESERVATION, "router", "phy:phy0", 0, 1);

	expect_state("disk:sdc1", "free", "");
	expect_grant("disk:sdc1", "other", 0, 0, NULL);

	expect_state("phy:phy0", "reserved", "");
	expect_state("usb:2357:012e:123456", "reserved", "");
	expect_grant("net:wlan0", "other", 0, 409, "router");
	expect_grant("usb:2357:012e:123456", "other", 0, 409, "router");
	expect_grant("phy:phy0", "router", 0, 0, NULL);

	/* A reservation never displaces a holder: held wins the state, and
	 * the reserving container's own start is refused naming the holder. */
	claim(DEVICE_CLAIM_GRANT, "squatter", "phy:phy0", 0, 0);
	expect_state("phy:phy0", "held", "");
	expect_grant("phy:phy0", "router", 0, 409, "squatter");
}

static void test_one_node_one_record(void)
{
	int a = -1, b = -1;

	build_fixture();
	if (device_records_share_a_node(g_recs, g_n, &a, &b)) {
		fprintf(stderr, "  FAIL: the fixture has %s and %s on one node\n", g_recs[a].id,
		        g_recs[b].id);
		g_failures++;
	}
	/* The .95 duplicate: one DRM node reported by two walks. */
	node(rec("pci:0000:00:01.0:card0", "pci", "pci:0000:00:01.0"), DEVICE_NODE_CHAR, 226, 0,
	     "/dev/card0");
	node(rec("gpu:0:card0", "gpu", ""), DEVICE_NODE_CHAR, 226, 0, "/dev/dri/card0");
	if (!device_records_share_a_node(g_recs, g_n, &a, &b)) {
		fprintf(stderr, "  FAIL: two records on 226:0 were not caught\n");
		g_failures++;
	}
}

static void test_json(void)
{
	struct json_writer w;
	char body[16384];

	build_fixture();
	g_cn = 0;
	claim(DEVICE_CLAIM_GRANT, "ar-1", "phy:phy0", 0, 0);
	claim(DEVICE_CLAIM_RESERVATION, "router", "phy:phy0", 0, 1);

	jw_init(&w);
	device_write_json_states(&w, g_recs, g_n, g_claims, g_cn);
	snprintf(body, sizeof(body), "%.*s", (int)w.len, w.buf);
	jw_free(&w);

	if (strstr(body, "\"id\":\"disk:vdb\",\"bus\":\"disk\",\"parent\":\"pci:0000:06:0c.0\"") == NULL ||
	    strstr(body, "\"state\":\"host\",\"reason\":\"os-disk\"") == NULL) {
		fprintf(stderr, "  FAIL: vdb's record or state is not written as the contract says\n");
		g_failures++;
	}
	if (strstr(body, "\"held_by\":[{\"container\":\"ar-1\",\"granted\":\"phy:phy0\","
	                 "\"shared\":false}]") == NULL) {
		fprintf(stderr, "  FAIL: held_by does not name ar-1 and the PHY it was granted\n");
		g_failures++;
	}
	if (strstr(body, "\"reserved_by\":[{\"container\":\"router\",\"claimed\":\"phy:phy0\","
	                 "\"binding\":true}]") == NULL) {
		fprintf(stderr, "  FAIL: reserved_by does not name the binding reservation\n");
		g_failures++;
	}
}

int main(void)
{
	printf("test_devicestate:\n");
	test_host_use();
	test_tree_holding();
	test_sharing();
	test_reservation();
	test_one_node_one_record();
	test_json();
	if (g_failures != 0) {
		printf("test_devicestate: %d FAILED\n", g_failures);
		return 1;
	}
	printf("test_devicestate: PASS\n");
	return 0;
}
