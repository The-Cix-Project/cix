/*
 * ADR-0383: dhcprelay_check() and dhcprelay_render(), the two decisions
 * behind a relayed DHCP range. Modelled on 192.168.15.95's own layout:
 * dns-1 on services and access, routing everything else via the cores'
 * services VIP; cr-1 on management, services, access and remote-access.
 */
#include "dhcprelay.h"

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

static void att(struct dhcprelay_host *h, const char *net, const char *addr, int prefix,
                const char *ifname)
{
	struct dhcprelay_att *a = &h->att[h->att_count++];

	snprintf(a->network, sizeof(a->network), "%s", net);
	a->ip_be = ip(addr);
	a->prefix_len = prefix;
	snprintf(a->ifname, sizeof(a->ifname), "%s", ifname);
}

static void route(struct dhcprelay_host *h, const char *dest, int prefix)
{
	h->routes[h->route_count].dest_be = ip(dest);
	h->routes[h->route_count].prefix_len = prefix;
	h->route_count++;
}

static struct dhcprelay_host g_dns1, g_dns2, g_cr1, g_cr2, g_isolated, g_noroute;

static void setup(void)
{
	memset(&g_dns1, 0, sizeof(g_dns1));
	snprintf(g_dns1.name, sizeof(g_dns1.name), "dns-1");
	att(&g_dns1, "services", "192.168.150.101", 24, "services");
	att(&g_dns1, "access", "192.168.151.101", 24, "access");
	route(&g_dns1, "0.0.0.0", 0);

	g_dns2 = g_dns1;
	snprintf(g_dns2.name, sizeof(g_dns2.name), "dns-2");
	g_dns2.att[0].ip_be = ip("192.168.150.102");
	g_dns2.att[1].ip_be = ip("192.168.151.102");

	memset(&g_cr1, 0, sizeof(g_cr1));
	snprintf(g_cr1.name, sizeof(g_cr1.name), "cr-1");
	att(&g_cr1, "management", "192.168.15.101", 24, "management");
	att(&g_cr1, "services", "192.168.150.253", 24, "services");
	att(&g_cr1, "access", "192.168.151.253", 24, "access");
	att(&g_cr1, "remote-access", "192.168.152.253", 24, "remote-access");

	g_cr2 = g_cr1;
	snprintf(g_cr2.name, sizeof(g_cr2.name), "cr-2");
	g_cr2.att[3].ip_be = ip("192.168.152.252");

	/* A server only on a network no core is on. */
	memset(&g_isolated, 0, sizeof(g_isolated));
	snprintf(g_isolated.name, sizeof(g_isolated.name), "iso-1");
	att(&g_isolated, "lab", "10.9.0.5", 24, "eth0");
	route(&g_isolated, "0.0.0.0", 0);

	/* A server on services with no default route: no way back to
	 * 192.168.152.0/24. */
	memset(&g_noroute, 0, sizeof(g_noroute));
	snprintf(g_noroute.name, sizeof(g_noroute.name), "nr-1");
	att(&g_noroute, "services", "192.168.150.120", 24, "services");
}

static void check(const struct dhcprelay_host *servers, int ns, const struct dhcprelay_host *relays,
                  int nr, enum dhcprelay_verdict want, const char *want_who, const char *what)
{
	const char *who = NULL;
	enum dhcprelay_verdict got = dhcprelay_check("remote-access", servers, ns, relays, nr, &who);

	g_cases++;
	if (got != want || (want_who != NULL && strcmp(who, want_who) != 0)) {
		fprintf(stderr, "FAIL: %s: gave %d (%s), want %d (%s)\n", what, (int)got, who,
		        (int)want, want_who != NULL ? want_who : "-");
		g_failures++;
	}
}

static const struct dhcprelay_host *find(const char *name, void *ctx)
{
	(void)ctx;
	if (strcmp(name, "dns-1") == 0)
		return &g_dns1;
	if (strcmp(name, "dns-2") == 0)
		return &g_dns2;
	if (strcmp(name, "cr-1") == 0)
		return &g_cr1;
	return NULL;
}

static void expect_text(const char *got, const char *want, const char *what)
{
	g_cases++;
	if (strstr(got, want) == NULL) {
		fprintf(stderr, "FAIL: %s: missing \"%s\" in:\n%s\n", what, want, got);
		g_failures++;
	}
}

static void expect_absent(const char *got, const char *absent, const char *what)
{
	g_cases++;
	if (strstr(got, absent) != NULL) {
		fprintf(stderr, "FAIL: %s: unexpected \"%s\" in:\n%s\n", what, absent, got);
		g_failures++;
	}
}

int main(void)
{
	struct dhcprelay_host servers[2], relays[2];
	struct dhcprelay_range ranges[2];
	char out[2048];
	int n;

	setup();
	servers[0] = g_dns1;
	servers[1] = g_dns2;
	relays[0] = g_cr1;
	relays[1] = g_cr2;

	check(servers, 2, relays, 2, DHCPRELAY_OK, "",
	      ".95's case: dns-1/dns-2 off remote-access, both cores relay");
	check(servers, 2, relays, 0, DHCPRELAY_UNREACHED, "dns-1",
	      "an unattached server with no relay hears nothing");
	{
		struct dhcprelay_host r[1];

		r[0] = g_dns1; /* not on remote-access */
		check(servers, 2, r, 1, DHCPRELAY_RELAY_NOT_ON_NETWORK, "dns-1",
		      "a relay not on the network hears nothing there");
	}
	{
		struct dhcprelay_host s[1];

		s[0] = g_isolated;
		check(s, 1, relays, 2, DHCPRELAY_NO_SHARED_NETWORK, "iso-1",
		      "a server the relay shares no network with cannot be reached");
		s[0] = g_noroute;
		check(s, 1, relays, 2, DHCPRELAY_NO_ROUTE_BACK, "nr-1",
		      "a server with no route to giaddr cannot reply");
		s[0] = g_noroute;
		route(&s[0], "192.168.152.0", 24);
		check(s, 1, relays, 2, DHCPRELAY_OK, "",
		      "a specific route back to the relayed subnet is enough");
	}
	{
		/* Every server on the network: a relay only duplicates. */
		struct dhcprelay_host s[1];

		s[0] = g_dns1;
		att(&s[0], "remote-access", "192.168.152.101", 24, "remote-access");
		check(s, 1, relays, 2, DHCPRELAY_UNNEEDED, "cr-1",
		      "relays on a range whose servers are all attached are refused");
		check(s, 1, relays, 0, DHCPRELAY_OK, "", "an attached server needs no relay (ADR-0197)");
	}

	/* The render, for cr-1. */
	memset(ranges, 0, sizeof(ranges));
	snprintf(ranges[0].network, sizeof(ranges[0].network), "remote-access");
	ranges[0].enabled = 1;
	snprintf(ranges[0].servers[0], DHCPRELAY_NAME_MAX, "dns-1");
	snprintf(ranges[0].servers[1], DHCPRELAY_NAME_MAX, "dns-2");
	ranges[0].server_count = 2;
	snprintf(ranges[0].relays[0], DHCPRELAY_NAME_MAX, "cr-1");
	snprintf(ranges[0].relays[1], DHCPRELAY_NAME_MAX, "cr-2");
	ranges[0].relay_count = 2;
	/* access: served directly by attached servers, no relays. */
	snprintf(ranges[1].network, sizeof(ranges[1].network), "access");
	ranges[1].enabled = 1;
	snprintf(ranges[1].servers[0], DHCPRELAY_NAME_MAX, "dns-1");
	ranges[1].server_count = 1;

	n = dhcprelay_render(&g_cr1, ranges, 2, find, NULL, out, sizeof(out));
	g_cases++;
	if (n < 0) {
		fprintf(stderr, "FAIL: render did not fit\n");
		g_failures++;
	}
	expect_text(out, "port=0\n", "DNS is off");
	expect_text(out, "dhcp-relay=192.168.152.253,192.168.150.101,services\n",
	            "cr-1 relays remote-access to dns-1 on services, replies on services");
	expect_text(out, "dhcp-relay=192.168.152.253,192.168.150.102,services\n",
	            "and to dns-2");
	expect_text(out, "interface=remote-access\n", "listens on the relayed network");
	expect_absent(out, "interface=services", "the reply side is not a listen interface");
	expect_absent(out, "interface=management", "never the LAN");
	expect_absent(out, "192.168.151", "a range with no relay renders nothing here");

	/* A range not naming this relay, or disabled, renders no line. */
	ranges[0].relay_count = 1; /* cr-1 only */
	n = dhcprelay_render(&g_cr2, ranges, 2, find, NULL, out, sizeof(out));
	expect_absent(out, "dhcp-relay=", "cr-2 is not named: no line");
	expect_absent(out, "interface=", "and no listen interface");
	ranges[0].relay_count = 2;
	ranges[0].enabled = 0;
	n = dhcprelay_render(&g_cr1, ranges, 2, find, NULL, out, sizeof(out));
	expect_absent(out, "dhcp-relay=", "a disabled range relays nothing");
	ranges[0].enabled = 1;

	/* A server with no facts (not running) gets no line until it has. */
	snprintf(ranges[0].servers[1], DHCPRELAY_NAME_MAX, "gone-1");
	n = dhcprelay_render(&g_cr1, ranges, 2, find, NULL, out, sizeof(out));
	expect_text(out, "192.168.150.101", "the known server keeps its line");
	expect_absent(out, "gone-1", "an unknown server gets none");

	/* Too small an output buffer is -1, never a truncated file. */
	n = dhcprelay_render(&g_cr1, ranges, 2, find, NULL, out, 40);
	g_cases++;
	if (n != -1) {
		fprintf(stderr, "FAIL: a render that does not fit must be -1, gave %d\n", n);
		g_failures++;
	}

	if (g_failures != 0)
		return 1;
	printf("test_dhcprelay: %d cases pass\n", g_cases);
	return 0;
}
