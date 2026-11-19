#ifndef DHCPRELAY_H
#define DHCPRELAY_H

/*
 * ADR-0383: a DHCP range may be relayed to its servers. The two pure
 * decisions behind it, over plain facts the daemon gathers:
 *
 *   dhcprelay_check()  -- whether a range's servers and relays can serve
 *                         it, refused at the point of asking (ADR-0197's
 *                         rule), naming the container at fault;
 *   dhcprelay_render() -- the relay's /etc/dnsmasq-relay.conf.
 *
 * Measured in dnsmasq 2.90's source (probe-dnsrelay@1-1, @3-1): a relay
 * line is dhcp-relay=<local address>,<server address>[,<interface>]; the
 * local address becomes giaddr (dhcp.c:1118), the server picks the range
 * by giaddr (rfc2131.c:289-291) and replies to it (dhcp.c:375-379), and
 * the interface names where the server's replies are accepted.
 *
 * Header-only and freestanding of the daemon, so test_dhcprelay checks
 * it with no daemon (the fwacquire_verdict.h pattern).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#define DHCPRELAY_NAME_MAX 64
#define DHCPRELAY_IFNAME_MAX 16
#define DHCPRELAY_MAX_ATT 8
#define DHCPRELAY_MAX_ROUTES 16
#define DHCPRELAY_MAX_LIST 8

/* One network a container is attached to: its address there, that
 * network's prefix, and the interface's name inside the container. */
struct dhcprelay_att {
	char network[DHCPRELAY_NAME_MAX];
	uint32_t ip_be;
	int prefix_len;
	char ifname[DHCPRELAY_IFNAME_MAX];
};

struct dhcprelay_route {
	uint32_t dest_be;
	int prefix_len;
};

/* A server or a relay: its attachments in attachment order, and its
 * static routes (a server's, for the route back to giaddr). */
struct dhcprelay_host {
	char name[DHCPRELAY_NAME_MAX];
	struct dhcprelay_att att[DHCPRELAY_MAX_ATT];
	int att_count;
	struct dhcprelay_route routes[DHCPRELAY_MAX_ROUTES];
	int route_count;
};

/* One range, as far as relaying is concerned. */
struct dhcprelay_range {
	char network[DHCPRELAY_NAME_MAX];
	int enabled;
	char servers[DHCPRELAY_MAX_LIST][DHCPRELAY_NAME_MAX];
	int server_count;
	char relays[DHCPRELAY_MAX_LIST][DHCPRELAY_NAME_MAX];
	int relay_count;
};

enum dhcprelay_verdict {
	DHCPRELAY_OK = 0,
	DHCPRELAY_RELAY_NOT_ON_NETWORK, /* a relay hears nothing on that network */
	DHCPRELAY_UNREACHED,            /* an unattached server and no relay */
	DHCPRELAY_UNNEEDED,             /* relays, but every server is attached */
	DHCPRELAY_NO_SHARED_NETWORK,    /* a relay cannot reach an unattached server */
	DHCPRELAY_NO_ROUTE_BACK         /* the server's reply to giaddr has no route */
};

static inline const struct dhcprelay_att *dhcprelay_on(const struct dhcprelay_host *h,
                                                       const char *network)
{
	int i;

	for (i = 0; h != NULL && i < h->att_count; i++)
		if (strcmp(h->att[i].network, network) == 0)
			return &h->att[i];
	return NULL;
}

static inline int dhcprelay_covers(uint32_t base_be, int prefix_len, uint32_t ip_be)
{
	uint32_t mask;

	if (prefix_len <= 0)
		return 1;
	if (prefix_len > 32)
		return 0;
	mask = htonl(0xffffffffu << (32 - prefix_len));
	return (base_be & mask) == (ip_be & mask);
}

/*
 * Where a relay sends to an unattached server: the server's address on
 * the first of its networks (in its attachment order) the relay is also
 * on. *relay_side is the relay's own attachment there, whose interface
 * accepts the server's replies. NULL when they share no network.
 */
static inline const struct dhcprelay_att *dhcprelay_shared(const struct dhcprelay_host *relay,
                                                           const struct dhcprelay_host *server,
                                                           const struct dhcprelay_att **relay_side)
{
	int i;

	for (i = 0; i < server->att_count; i++) {
		const struct dhcprelay_att *r = dhcprelay_on(relay, server->att[i].network);

		if (r != NULL) {
			if (relay_side != NULL)
				*relay_side = r;
			return &server->att[i];
		}
	}
	return NULL;
}

/* Whether a server can send its reply to ip_be: a network it is on
 * covers it, or one of its static routes does (a default route does). */
static inline int dhcprelay_route_back(const struct dhcprelay_host *server, uint32_t ip_be)
{
	int i;

	for (i = 0; i < server->att_count; i++)
		if (dhcprelay_covers(server->att[i].ip_be, server->att[i].prefix_len, ip_be))
			return 1;
	for (i = 0; i < server->route_count; i++)
		if (dhcprelay_covers(server->routes[i].dest_be, server->routes[i].prefix_len, ip_be))
			return 1;
	return 0;
}

/*
 * Whether a range's servers and relays can serve it. servers[] and
 * relays[] are the hosts the range names, in its order. On a refusal,
 * *who is the name of the container at fault.
 */
static inline enum dhcprelay_verdict
dhcprelay_check(const char *network, const struct dhcprelay_host *servers, int server_count,
                const struct dhcprelay_host *relays, int relay_count, const char **who)
{
	int s, r, unattached = 0;

	*who = "";
	for (r = 0; r < relay_count; r++)
		if (dhcprelay_on(&relays[r], network) == NULL) {
			*who = relays[r].name;
			return DHCPRELAY_RELAY_NOT_ON_NETWORK;
		}
	for (s = 0; s < server_count; s++) {
		if (dhcprelay_on(&servers[s], network) != NULL)
			continue;
		unattached++;
		if (relay_count == 0) {
			*who = servers[s].name;
			return DHCPRELAY_UNREACHED;
		}
		for (r = 0; r < relay_count; r++) {
			const struct dhcprelay_att *giaddr = dhcprelay_on(&relays[r], network);

			if (dhcprelay_shared(&relays[r], &servers[s], NULL) == NULL) {
				*who = servers[s].name;
				return DHCPRELAY_NO_SHARED_NETWORK;
			}
			if (!dhcprelay_route_back(&servers[s], giaddr->ip_be)) {
				*who = servers[s].name;
				return DHCPRELAY_NO_ROUTE_BACK;
			}
		}
	}
	if (relay_count > 0 && unattached == 0) {
		*who = relays[0].name;
		return DHCPRELAY_UNNEEDED;
	}
	return DHCPRELAY_OK;
}

/*
 * The relay's /etc/dnsmasq-relay.conf. find_host() answers a server's
 * facts by name, or NULL when it has none (not running, not defined):
 * such a server gets no line until it does. Returns bytes written
 * (excluding NUL), or -1 when it did not fit.
 *
 * port=0 turns dnsmasq's DNS off; with no dhcp-range it serves no DHCP
 * either, so it only relays (dnsmasq.c:312, measured). One interface=
 * line per relayed network, and none for the reply side: in
 * dhcp_packet() a server's reply is matched by relay_reply4() before any
 * interface check, while a client's request passes iface_check() before
 * relay_upstream4() (probe-dnsrelay@4-1). So a core relays only the
 * networks a range names, never broadcasts from another (a LAN with its
 * own DHCP server), and still takes replies on the servers' network.
 * With no relay line at all, dnsmasq opens no DHCP socket.
 */
static inline int dhcprelay_render(const struct dhcprelay_host *relay,
                                   const struct dhcprelay_range *ranges, int range_count,
                                   const struct dhcprelay_host *(*find_host)(const char *name,
                                                                             void *ctx),
                                   void *ctx, char *out, size_t out_size)
{
	char ifnames[DHCPRELAY_MAX_ATT][DHCPRELAY_IFNAME_MAX];
	int if_count = 0;
	size_t off;
	int i, s, k, n;

	n = snprintf(out, out_size,
	             "# Rendered by cixd -- every edit here is overwritten (ADR-0383).\n"
	             "# A relay only: no DNS (port=0) and no dhcp-range. Each line\n"
	             "# relays one network's requests to one server that is not on\n"
	             "# it, and accepts that server's replies on one interface.\n"
	             "port=0\n");
	if (n < 0 || (size_t)n >= out_size)
		return -1;
	off = (size_t)n;
	for (i = 0; i < range_count; i++) {
		const struct dhcprelay_range *rg = &ranges[i];
		const struct dhcprelay_att *giaddr;
		int named = 0;

		if (!rg->enabled)
			continue;
		for (k = 0; k < rg->relay_count; k++)
			if (strcmp(rg->relays[k], relay->name) == 0)
				named = 1;
		giaddr = dhcprelay_on(relay, rg->network);
		if (!named || giaddr == NULL)
			continue;
		for (s = 0; s < rg->server_count; s++) {
			const struct dhcprelay_host *server = find_host(rg->servers[s], ctx);
			const struct dhcprelay_att *to, *reply_side = NULL;
			char a[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
			struct in_addr ia;
			int m;

			if (server == NULL || dhcprelay_on(server, rg->network) != NULL)
				continue;
			to = dhcprelay_shared(relay, server, &reply_side);
			if (to == NULL)
				continue;
			ia.s_addr = giaddr->ip_be;
			inet_ntop(AF_INET, &ia, a, sizeof(a));
			ia.s_addr = to->ip_be;
			inet_ntop(AF_INET, &ia, b, sizeof(b));
			n = snprintf(out + off, out_size - off, "dhcp-relay=%s,%s,%s\n", a, b,
			             reply_side->ifname);
			if (n < 0 || (size_t)n >= out_size - off)
				return -1;
			off += (size_t)n;
			for (m = 0; m < if_count; m++)
				if (strcmp(ifnames[m], giaddr->ifname) == 0)
					break;
			if (m == if_count && if_count < DHCPRELAY_MAX_ATT)
				snprintf(ifnames[if_count++], DHCPRELAY_IFNAME_MAX, "%s", giaddr->ifname);
		}
	}
	for (i = 0; i < if_count; i++) {
		n = snprintf(out + off, out_size - off, "interface=%s\n", ifnames[i]);
		if (n < 0 || (size_t)n >= out_size - off)
			return -1;
		off += (size_t)n;
	}
	return (int)off;
}

#endif /* DHCPRELAY_H */
