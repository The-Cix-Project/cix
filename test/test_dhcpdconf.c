/*
 * ADR-0386: dhcpd_render(), dhcpd_leases_parse() and dhcpd_leases_merge().
 *
 * The lease file text below is real: dhcpt-1's and dhcpt-2's
 * /var/lib/dhcp/dhcpd.leases on 192.168.15.95, 2026-11-20, after a
 * dhcpcd client ("-h scratchclient") leased 10.99.0.105 from the
 * scratch pair. Cut to the records for that address, in file order, plus
 * the header and the failover state blocks; nothing in a record edited.
 */
#include "dhcpdconf.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_cases;

static uint32_t ip(const char *s)
{
	struct in_addr a;

	inet_pton(AF_INET, s, &a);
	return a.s_addr;
}

static void expect(int ok, const char *what)
{
	g_cases++;
	if (!ok) {
		g_failures++;
		fprintf(stderr, "FAIL: %s\n", what);
	}
}

static void expect_has(const char *text, const char *needle, const char *what)
{
	g_cases++;
	if (strstr(text, needle) == NULL) {
		g_failures++;
		fprintf(stderr, "FAIL: %s: missing \"%s\" in:\n%s\n", what, needle, text);
	}
}

static void expect_not(const char *text, const char *needle, const char *what)
{
	g_cases++;
	if (strstr(text, needle) != NULL) {
		g_failures++;
		fprintf(stderr, "FAIL: %s: unexpected \"%s\"\n", what, needle);
	}
}

static const char PRIMARY_FILE[] =
	"# The format of this file is documented in the dhcpd.leases(5) manual page.\n"
	"# This lease file was written by isc-dhcp-4.4.3-P1\n"
	"\n"
	"# authoring-byte-order entry is generated, DO NOT DELETE\n"
	"authoring-byte-order little-endian;\n"
	"\n"
	"\n"
	"failover peer \"dhcpt\" state {\n"
	"  my state recover at 5 2026/11/20 23:57:04;\n"
	"  partner state unknown-state at 5 2026/11/20 23:57:04;\n"
	"}\n"
	"lease 10.99.0.105 {\n"
	"  binding state free;\n"
	"}\n"
	"failover peer \"dhcpt\" state {\n"
	"  my state normal at 5 2026/11/20 23:57:04;\n"
	"  partner state normal at 5 2026/11/20 23:57:04;\n"
	"}\n"
	"lease 10.99.0.105 {\n"
	"  starts 5 2026/11/20 23:57:21;\n"
	"  ends 5 2026/11/20 23:59:21;\n"
	"  tstp 6 2026/11/21 00:03:21;\n"
	"  cltt 5 2026/11/20 23:57:21;\n"
	"  binding state active;\n"
	"  next binding state expired;\n"
	"  rewind binding state free;\n"
	"  hardware ethernet ee:42:cb:e3:d4:fb;\n"
	"  uid \"\\377\\313\\343\\324\\373\\000\\001\\000\\0012\\223\\232\\337\\356B\\313\\343\\324\\373\";\n"
	"  client-hostname \"scratchclient\";\n"
	"}\n"
	"lease 10.99.0.105 {\n"
	"  starts 5 2026/11/20 23:57:21;\n"
	"  ends 5 2026/11/20 23:59:21;\n"
	"  tstp 6 2026/11/21 00:03:21;\n"
	"  tsfp 6 2026/11/21 00:03:21;\n"
	"  atsfp 6 2026/11/21 00:03:21;\n"
	"  cltt 5 2026/11/20 23:57:21;\n"
	"  binding state active;\n"
	"  next binding state expired;\n"
	"  hardware ethernet ee:42:cb:e3:d4:fb;\n"
	"  uid \"\\377\\313\\343\\324\\373\\000\\001\\000\\0012\\223\\232\\337\\356B\\313\\343\\324\\373\";\n"
	"  client-hostname \"scratchclient\";\n"
	"}\n"
	"lease 10.99.0.106 {\n"
	"  binding state free;\n"
	"}\n";

static const char SECONDARY_FILE[] =
	"authoring-byte-order little-endian;\n"
	"failover peer \"dhcpt\" state {\n"
	"  my state normal at 5 2026/11/20 23:57:04;\n"
	"  partner state normal at 5 2026/11/20 23:57:04;\n"
	"}\n"
	"server-duid \"\\000\\001\\000\\001\";\n"
	"host omapi-made {\n"
	"  dynamic;\n"
	"  hardware ethernet 02:00:00:00:00:01;\n"
	"}\n"
	"lease 10.99.0.105 {\n"
	"  starts 5 2026/11/20 23:57:21;\n"
	"  ends 5 2026/11/20 23:59:21;\n"
	"  tsfp 6 2026/11/21 00:03:21;\n"
	"  atsfp 6 2026/11/21 00:03:21;\n"
	"  binding state active;\n"
	"  next binding state expired;\n"
	"  hardware ethernet ee:42:cb:e3:d4:fb;\n"
	"  uid \"\\377\\313\\343\\324\\373\\000\\001\\000\\0012\\223\\232\\337\\356B\\313\\343\\324\\373\";\n"
	"}\n"
	"lease 10.99.0.107 {\n"
	"  starts 5 2026/11/20 23:58:00;\n"
	"  ends never;\n"
	"  binding state active;\n"
	"  hardware ethernet 02:00:00:00:00:07;\n"
	"  client-hostname \"bad\\001name\";\n"
	"}\n";

static const struct dhcpd_lease *find(const struct dhcpd_leases *l, const char *addr)
{
	int i;

	for (i = 0; i < l->count; i++)
		if (l->v[i].ip_be == ip(addr))
			return &l->v[i];
	return NULL;
}

static void build_conf(struct dhcpd_conf *c, int primary)
{
	struct dhcpd_subnet *s;

	memset(c, 0, sizeof(*c));
	c->failover = 1;
	c->primary = primary;
	c->my_addr_be = ip(primary ? "192.168.150.111" : "192.168.150.112");
	c->peer_addr_be = ip(primary ? "192.168.150.112" : "192.168.150.111");

	s = &c->subnets[c->subnet_count++];
	s->net_be = ip("192.168.150.0");
	s->prefix_len = 24;

	s = &c->subnets[c->subnet_count++];
	s->net_be = ip("192.168.151.0");
	s->prefix_len = 24;
	s->has_pool = 1;
	s->failover = 1;
	s->start_be = ip("192.168.151.150");
	s->end_be = ip("192.168.151.200");
	s->router_be = ip("192.168.151.254");
	s->dns_be[0] = ip("192.168.150.101");
	s->dns_be[1] = ip("192.168.150.102");
	s->dns_count = 2;
	s->lease_seconds = 43200;

	snprintf(c->hosts[0].mac, sizeof(c->hosts[0].mac), "02:00:00:00:00:99");
	c->hosts[0].ip_be = ip("192.168.151.230");
	snprintf(c->hosts[0].name, sizeof(c->hosts[0].name), "printer");
	snprintf(c->hosts[1].mac, sizeof(c->hosts[1].mac), "02:00:00:00:00:98");
	c->hosts[1].ip_be = ip("192.168.151.231");
	c->host_count = 2;
}

static void test_render(void)
{
	struct dhcpd_conf c;
	char out[8192];
	char tiny[64];

	build_conf(&c, 1);
	expect(dhcpd_render(&c, out, sizeof(out)) > 0, "primary renders");
	expect_has(out, "failover peer \"cix\" {\n  primary;\n  mclt 3600;\n  split 128;\n",
	           "primary carries mclt and split");
	expect_has(out, "  address 192.168.150.111;\n  port 647;\n  peer address 192.168.150.112;\n",
	           "primary addresses");
	expect_has(out, "  auto-partner-down 600;\n", "auto-partner-down");
	expect_has(out, "subnet 192.168.150.0 netmask 255.255.255.0 { }\n",
	           "own network declared with no pool");
	expect_has(out,
	           "subnet 192.168.151.0 netmask 255.255.255.0 {\n"
	           "  option routers 192.168.151.254;\n"
	           "  option domain-name-servers 192.168.150.101, 192.168.150.102;\n"
	           "  default-lease-time 43200;\n  max-lease-time 43200;\n"
	           "  pool {\n    failover peer \"cix\";\n    range 192.168.151.150 192.168.151.200;\n"
	           "  }\n}\n",
	           "range subnet");
	expect_has(out,
	           "host r-02-00-00-00-00-99 {\n  hardware ethernet 02:00:00:00:00:99;\n"
	           "  fixed-address 192.168.151.230;\n  option host-name \"printer\";\n}\n",
	           "named reservation");
	expect_has(out,
	           "host r-02-00-00-00-00-98 {\n  hardware ethernet 02:00:00:00:00:98;\n"
	           "  fixed-address 192.168.151.231;\n}\n",
	           "a reservation with no name still has a unique declaration");

	build_conf(&c, 0);
	expect(dhcpd_render(&c, out, sizeof(out)) > 0, "secondary renders");
	expect_has(out, "failover peer \"cix\" {\n  secondary;\n  address 192.168.150.112;\n",
	           "secondary");
	expect_not(out, "mclt", "the secondary carries no mclt");
	expect_not(out, "split", "the secondary carries no split");

	build_conf(&c, 1);
	c.failover = 0;
	c.subnets[1].router_be = 0;
	c.subnets[1].dns_count = 0;
	expect(dhcpd_render(&c, out, sizeof(out)) > 0, "single server renders");
	expect_not(out, "failover", "a single server has no peer");
	expect_not(out, "option routers", "no router means none sent");
	expect_not(out, "domain-name-servers", "no DNS servers means none sent");

	/* A paired server serving another range alone: that pool is its
	 * own, so it carries no failover peer the partner does not know. */
	build_conf(&c, 1);
	c.subnets[2] = c.subnets[1];
	c.subnets[2].net_be = ip("192.168.152.0");
	c.subnets[2].start_be = ip("192.168.152.100");
	c.subnets[2].end_be = ip("192.168.152.200");
	c.subnets[2].failover = 0;
	c.subnet_count = 3;
	expect(dhcpd_render(&c, out, sizeof(out)) > 0, "mixed renders");
	expect_has(out,
	           "  pool {\n    range 192.168.152.100 192.168.152.200;\n  }\n",
	           "a server's own pool has no failover peer");
	expect_has(out,
	           "  pool {\n    failover peer \"cix\";\n    range 192.168.151.150 192.168.151.200;\n",
	           "the pair's pool keeps its peer");

	build_conf(&c, 1);
	expect(dhcpd_render(&c, tiny, sizeof(tiny)) == -1, "a render that does not fit is -1");
}

static void test_leases(void)
{
	struct dhcpd_leases a, b;
	const struct dhcpd_lease *l;

	expect(dhcpd_leases_parse(PRIMARY_FILE, strlen(PRIMARY_FILE), &a) == 0, "primary parses");
	expect(a.count == 2, "one record per address, last wins");
	l = find(&a, "10.99.0.105");
	expect(l != NULL && l->binding == DHCPD_BINDING_ACTIVE, "105 active");
	expect(l != NULL && strcmp(l->mac, "ee:42:cb:e3:d4:fb") == 0, "105 mac");
	expect(l != NULL && strcmp(l->hostname, "scratchclient") == 0, "105 hostname");
	expect(l != NULL && l->starts == 1795219041LL && l->ends == 1795219161LL, "105 times, UTC");
	expect(strcmp(a.my_state, "normal") == 0 && strcmp(a.partner_state, "normal") == 0,
	       "the last failover state block wins");
	expect(dhcpd_lease_is_current(l, 1795219100LL), "current before ends");
	expect(!dhcpd_lease_is_current(l, 1795219200LL), "not current after ends");
	expect(!dhcpd_lease_is_current(find(&a, "10.99.0.106"), 0), "a free lease is no client's");

	expect(dhcpd_leases_parse(SECONDARY_FILE, strlen(SECONDARY_FILE), &b) == 0,
	       "secondary parses");
	expect(b.count == 2, "server-duid and a host block are skipped");
	l = find(&b, "10.99.0.105");
	expect(l != NULL && l->hostname[0] == '\0', "the peer's copy carries no name (measured)");
	l = find(&b, "10.99.0.107");
	expect(l != NULL && l->ends == 0, "ends never");
	expect(l != NULL && l->hostname[0] == '\0', "a name with a control byte is dropped");
	expect(dhcpd_lease_is_current(l, 4000000000LL), "a lease that never ends is current");

	expect(dhcpd_leases_merge(&b, &a) == 0, "merge");
	l = find(&b, "10.99.0.105");
	expect(b.count == 3, "merged: 105, 106, 107");
	expect(l != NULL && strcmp(l->hostname, "scratchclient") == 0,
	       "the name comes from whichever peer has it");
	dhcpd_leases_free(&a);
	dhcpd_leases_free(&b);
}

int main(void)
{
	test_render();
	test_leases();
	if (g_failures != 0) {
		fprintf(stderr, "DHCPDCONF: FAIL (%d of %d)\n", g_failures, g_cases);
		return 1;
	}
	printf("DHCPDCONF: PASS (%d cases)\n", g_cases);
	return 0;
}
