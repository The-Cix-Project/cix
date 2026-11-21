#ifndef DHCPDCONF_H
#define DHCPDCONF_H

/*
 * ADR-0386: DHCP is served by an ISC dhcpd failover pair. The two pure
 * halves of talking to dhcpd, over plain facts the daemon gathers:
 *
 *   dhcpd_render()        -- one server's /etc/dhcpd.conf;
 *   dhcpd_leases_parse()  -- what a dhcpd.leases file says, and
 *   dhcpd_leases_merge()  -- one answer from both peers' files.
 *
 * Header-only and freestanding of the daemon, so test_dhcpdconf checks
 * it with no daemon (the dhcprelay.h pattern).
 *
 * Measured on 192.168.15.95, 2026-11-20, with a scratch pair
 * (isc-dhcp@4.4.3-P1-3) and a dhcpcd client:
 *   - a lease file is an append log: one address carries several
 *     records, and the last one in the file is the server's answer;
 *   - the peer's copy of a lease carried no client-hostname, so a name
 *     is taken from whichever file has it for the same binding;
 *   - times are UTC, written "<weekday> YYYY/MM/DD HH:MM:SS";
 *   - a fixed-address host never appears in the file, so reservations
 *     are not leases and their names come from the daemon's own state.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>

#define DHCPD_MAC_MAX 18
#define DHCPD_HOSTNAME_MAX 64
#define DHCPD_STATE_MAX 32
#define DHCPD_MAX_SUBNETS 32
#define DHCPD_MAX_HOSTS 128
#define DHCPD_MAX_DNS 4

/*
 * The failover settings (ADR-0386 decision 3). A server is in at most
 * one pair, so the peer declaration has one name. mclt and split are
 * the primary's alone (dhcpd.conf(5): mclt "must be specified on the
 * primary, and may not be specified on the secondary").
 */
#define DHCPD_PEER_NAME "cix"
#define DHCPD_FAILOVER_PORT 647
#define DHCPD_MCLT_SECONDS 3600
#define DHCPD_AUTO_PARTNER_DOWN_SECONDS 600

/* One subnet declaration. dhcpd listens only on an interface whose
 * subnet is declared, so a server's own networks are declared with no
 * pool, and every range it serves is declared with one. */
struct dhcpd_subnet {
	uint32_t net_be;
	int prefix_len;
	int has_pool;
	int failover; /* this pool is the pair's (needs the conf's peer) */
	uint32_t start_be;
	uint32_t end_be;
	uint32_t router_be; /* 0: none sent */
	uint32_t dns_be[DHCPD_MAX_DNS];
	int dns_count;
	int lease_seconds;
};

struct dhcpd_host {
	char mac[DHCPD_MAC_MAX];
	uint32_t ip_be;
	char name[DHCPD_HOSTNAME_MAX]; /* "" = no name */
};

struct dhcpd_conf {
	int failover;
	int primary;
	uint32_t my_addr_be;
	uint32_t peer_addr_be;
	struct dhcpd_subnet subnets[DHCPD_MAX_SUBNETS];
	int subnet_count;
	struct dhcpd_host hosts[DHCPD_MAX_HOSTS];
	int host_count;
};

static inline int dhcpd_put(char *out, size_t out_size, size_t *off, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*off >= out_size)
		return -1;
	va_start(ap, fmt);
	n = vsnprintf(out + *off, out_size - *off, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= out_size - *off)
		return -1;
	*off += (size_t)n;
	return 0;
}

static inline void dhcpd_ip_str(uint32_t be, char *out, size_t out_size)
{
	struct in_addr a;

	a.s_addr = be;
	if (inet_ntop(AF_INET, &a, out, (socklen_t)out_size) == NULL)
		snprintf(out, out_size, "0.0.0.0");
}

static inline uint32_t dhcpd_mask_be(int prefix_len)
{
	if (prefix_len <= 0)
		return 0;
	if (prefix_len >= 32)
		return 0xffffffffu;
	return htonl(0xffffffffu << (32 - prefix_len));
}

/*
 * Renders the whole file, always in full. Returns bytes written, or -1
 * when it did not fit: the caller logs that and keeps the file it had,
 * never writes a truncated one.
 */
static inline int dhcpd_render(const struct dhcpd_conf *c, char *out, size_t out_size)
{
	size_t off = 0;
	char a[16], b[16];
	int i, k;

	if (out_size == 0)
		return -1;
	out[0] = '\0';
	if (dhcpd_put(out, out_size, &off,
	              "# Rendered by cixd -- every edit here is overwritten (ADR-0386).\n"
	              "# dhcpd reads this file only at start, so a change restarts this\n"
	              "# container, one member of a pair at a time.\n"
	              "authoritative;\n"
	              "ddns-update-style none;\n") != 0)
		return -1;

	if (c->failover) {
		dhcpd_ip_str(c->my_addr_be, a, sizeof(a));
		dhcpd_ip_str(c->peer_addr_be, b, sizeof(b));
		if (dhcpd_put(out, out_size, &off, "failover peer \"%s\" {\n  %s;\n", DHCPD_PEER_NAME,
		              c->primary ? "primary" : "secondary") != 0)
			return -1;
		if (c->primary &&
		    dhcpd_put(out, out_size, &off,
		              "  mclt %d;\n  split 128;\n  load balance max seconds 3;\n",
		              DHCPD_MCLT_SECONDS) != 0)
			return -1;
		if (dhcpd_put(out, out_size, &off,
		              "  address %s;\n  port %d;\n  peer address %s;\n  peer port %d;\n"
		              "  max-response-delay 60;\n  max-unacked-updates 10;\n"
		              "  auto-partner-down %d;\n}\n",
		              a, DHCPD_FAILOVER_PORT, b, DHCPD_FAILOVER_PORT,
		              DHCPD_AUTO_PARTNER_DOWN_SECONDS) != 0)
			return -1;
	}

	for (i = 0; i < c->subnet_count; i++) {
		const struct dhcpd_subnet *s = &c->subnets[i];
		uint32_t mask = dhcpd_mask_be(s->prefix_len);

		dhcpd_ip_str(s->net_be & mask, a, sizeof(a));
		dhcpd_ip_str(mask, b, sizeof(b));
		if (!s->has_pool) {
			if (dhcpd_put(out, out_size, &off, "subnet %s netmask %s { }\n", a, b) != 0)
				return -1;
			continue;
		}
		if (dhcpd_put(out, out_size, &off, "subnet %s netmask %s {\n", a, b) != 0)
			return -1;
		/* No router line sends no router: dhcpd, unlike dnsmasq, has
		 * no default that names itself. */
		if (s->router_be != 0) {
			dhcpd_ip_str(s->router_be, a, sizeof(a));
			if (dhcpd_put(out, out_size, &off, "  option routers %s;\n", a) != 0)
				return -1;
		}
		if (s->dns_count > 0) {
			if (dhcpd_put(out, out_size, &off, "  option domain-name-servers ") != 0)
				return -1;
			for (k = 0; k < s->dns_count; k++) {
				dhcpd_ip_str(s->dns_be[k], a, sizeof(a));
				if (dhcpd_put(out, out_size, &off, "%s%s", k ? ", " : "", a) != 0)
					return -1;
			}
			if (dhcpd_put(out, out_size, &off, ";\n") != 0)
				return -1;
		}
		dhcpd_ip_str(s->start_be, a, sizeof(a));
		dhcpd_ip_str(s->end_be, b, sizeof(b));
		if (dhcpd_put(out, out_size, &off,
		              "  default-lease-time %d;\n  max-lease-time %d;\n  pool {\n%s"
		              "    range %s %s;\n  }\n}\n",
		              s->lease_seconds, s->lease_seconds,
		              c->failover && s->failover ? "    failover peer \"" DHCPD_PEER_NAME "\";\n" : "",
		              a,
		              b) != 0)
			return -1;
	}

	/* A host's declaration name is its MAC, which is unique among
	 * reservations, so a reservation with no hostname still has one. */
	for (i = 0; i < c->host_count; i++) {
		const struct dhcpd_host *h = &c->hosts[i];
		char id[DHCPD_MAC_MAX];

		snprintf(id, sizeof(id), "%s", h->mac);
		for (k = 0; id[k] != '\0'; k++)
			if (id[k] == ':')
				id[k] = '-';
		dhcpd_ip_str(h->ip_be, a, sizeof(a));
		if (dhcpd_put(out, out_size, &off,
		              "host r-%s {\n  hardware ethernet %s;\n  fixed-address %s;\n", id,
		              h->mac, a) != 0)
			return -1;
		if (h->name[0] != '\0' &&
		    dhcpd_put(out, out_size, &off, "  option host-name \"%s\";\n", h->name) != 0)
			return -1;
		if (dhcpd_put(out, out_size, &off, "}\n") != 0)
			return -1;
	}
	return (int)off;
}

/* ---- dhcpd.leases ---- */

enum dhcpd_binding {
	DHCPD_BINDING_OTHER = 0,
	DHCPD_BINDING_ACTIVE,
	DHCPD_BINDING_FREE,
	DHCPD_BINDING_BACKUP,
	DHCPD_BINDING_EXPIRED,
	DHCPD_BINDING_RELEASED,
	DHCPD_BINDING_ABANDONED
};

struct dhcpd_lease {
	uint32_t ip_be;
	char mac[DHCPD_MAC_MAX];
	char hostname[DHCPD_HOSTNAME_MAX];
	long long starts; /* -1 when absent */
	long long ends;   /* -1 when absent, 0 for "never" */
	enum dhcpd_binding binding;
};

struct dhcpd_leases {
	struct dhcpd_lease *v;
	int count;
	int cap;
	/* The file's last "failover peer ... state" block; "" when none. */
	char my_state[DHCPD_STATE_MAX];
	char partner_state[DHCPD_STATE_MAX];
};

static inline void dhcpd_leases_free(struct dhcpd_leases *l)
{
	free(l->v);
	memset(l, 0, sizeof(*l));
}

/* "5 2026/11/20 23:57:21" (UTC) or "never". */
static inline long long dhcpd_parse_time(const char *s)
{
	int wd, y, mo, d, h, mi, se;
	struct tm tm;

	while (*s == ' ' || *s == '\t')
		s++;
	if (strncmp(s, "never", 5) == 0)
		return 0;
	if (sscanf(s, "%d %d/%d/%d %d:%d:%d", &wd, &y, &mo, &d, &h, &mi, &se) != 7)
		return -1;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = se;
	return (long long)timegm(&tm);
}

/* A quoted string's contents, unescaped; a byte outside printable ASCII
 * makes the whole value empty, since it is a client's own claim. */
static inline void dhcpd_parse_quoted(const char *s, char *out, size_t out_size)
{
	size_t n = 0;
	int bad = 0;

	out[0] = '\0';
	s = strchr(s, '"');
	if (s == NULL)
		return;
	s++;
	while (*s != '\0' && *s != '"') {
		char ch = *s++;

		if (ch == '\\' && *s != '\0')
			ch = *s++;
		if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e)
			bad = 1;
		if (n + 1 < out_size)
			out[n++] = ch;
	}
	out[n] = '\0';
	if (bad)
		out[0] = '\0';
}

static inline int dhcpd_leases_put(struct dhcpd_leases *l, const struct dhcpd_lease *rec)
{
	int i;

	/* Last record wins: replace this address's earlier one in place. */
	for (i = 0; i < l->count; i++)
		if (l->v[i].ip_be == rec->ip_be) {
			l->v[i] = *rec;
			return 0;
		}
	if (l->count == l->cap) {
		int cap = l->cap ? l->cap * 2 : 64;
		struct dhcpd_lease *v = (struct dhcpd_lease *)realloc(l->v, (size_t)cap * sizeof(*v));

		if (v == NULL)
			return -1;
		l->v = v;
		l->cap = cap;
	}
	l->v[l->count++] = *rec;
	return 0;
}

static inline enum dhcpd_binding dhcpd_parse_binding(const char *s)
{
	while (*s == ' ')
		s++;
	if (strncmp(s, "active", 6) == 0)
		return DHCPD_BINDING_ACTIVE;
	if (strncmp(s, "free", 4) == 0)
		return DHCPD_BINDING_FREE;
	if (strncmp(s, "backup", 6) == 0)
		return DHCPD_BINDING_BACKUP;
	if (strncmp(s, "expired", 7) == 0)
		return DHCPD_BINDING_EXPIRED;
	if (strncmp(s, "released", 8) == 0)
		return DHCPD_BINDING_RELEASED;
	if (strncmp(s, "abandoned", 9) == 0)
		return DHCPD_BINDING_ABANDONED;
	return DHCPD_BINDING_OTHER;
}

/* Copies a state word ("normal", "partner-down", ...) up to " at". */
static inline void dhcpd_parse_state_word(const char *s, char *out, size_t out_size)
{
	size_t n = 0;

	while (*s == ' ')
		s++;
	while (*s != '\0' && *s != ' ' && *s != ';' && n + 1 < out_size)
		out[n++] = *s++;
	out[n] = '\0';
}

/*
 * Parses a whole lease file. Returns 0, or -1 when memory ran out. A
 * line it does not understand is skipped: the file is dhcpd's, and a
 * field this parser does not know must not make it unreadable.
 */
static inline int dhcpd_leases_parse(const char *text, size_t len, struct dhcpd_leases *out)
{
	enum { TOP, LEASE, FAILOVER, OTHER } where = TOP;
	struct dhcpd_lease rec;
	size_t pos = 0;
	int depth = 0;

	memset(out, 0, sizeof(*out));
	memset(&rec, 0, sizeof(rec));
	while (pos < len) {
		char line[512];
		size_t n = 0;
		const char *p;

		while (pos < len && text[pos] != '\n') {
			if (n + 1 < sizeof(line))
				line[n++] = text[pos];
			pos++;
		}
		pos++;
		line[n] = '\0';
		p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '#')
			continue;

		if (where == TOP) {
			char addr[64];

			if (strncmp(p, "lease ", 6) == 0 && sscanf(p + 6, "%63s", addr) == 1 &&
			    strchr(p, '{') != NULL) {
				struct in_addr ia;

				memset(&rec, 0, sizeof(rec));
				rec.starts = -1;
				rec.ends = -1;
				if (inet_pton(AF_INET, addr, &ia) == 1) {
					rec.ip_be = ia.s_addr;
					where = LEASE;
				} else {
					where = OTHER;
					depth = 1;
				}
			} else if (strncmp(p, "failover peer ", 14) == 0 && strstr(p, " state") != NULL &&
			           strchr(p, '{') != NULL) {
				where = FAILOVER;
			} else if (strchr(p, '{') != NULL) {
				where = OTHER;
				depth = 1;
			}
			continue;
		}
		if (where == OTHER) {
			if (strchr(p, '{') != NULL)
				depth++;
			if (*p == '}' && --depth == 0)
				where = TOP;
			continue;
		}
		if (where == FAILOVER) {
			if (*p == '}')
				where = TOP;
			else if (strncmp(p, "my state ", 9) == 0)
				dhcpd_parse_state_word(p + 9, out->my_state, sizeof(out->my_state));
			else if (strncmp(p, "partner state ", 14) == 0)
				dhcpd_parse_state_word(p + 14, out->partner_state,
				                       sizeof(out->partner_state));
			continue;
		}
		/* where == LEASE */
		if (*p == '}') {
			if (dhcpd_leases_put(out, &rec) != 0)
				return -1;
			where = TOP;
		} else if (strncmp(p, "starts ", 7) == 0) {
			rec.starts = dhcpd_parse_time(p + 7);
		} else if (strncmp(p, "ends ", 5) == 0) {
			rec.ends = dhcpd_parse_time(p + 5);
		} else if (strncmp(p, "binding state ", 14) == 0) {
			rec.binding = dhcpd_parse_binding(p + 14);
		} else if (strncmp(p, "hardware ethernet ", 18) == 0) {
			char mac[DHCPD_MAC_MAX + 1];

			if (sscanf(p + 18, "%18[0-9a-fA-F:]", mac) == 1 && strlen(mac) == 17)
				snprintf(rec.mac, sizeof(rec.mac), "%s", mac);
		} else if (strncmp(p, "client-hostname ", 16) == 0) {
			dhcpd_parse_quoted(p + 16, rec.hostname, sizeof(rec.hostname));
		}
	}
	return 0;
}

/*
 * Folds src into dst: one record per address. The later `ends` wins;
 * a name missing from the winner is taken from the other file's record
 * for the same address and MAC, since one peer's copy carries none.
 */
static inline int dhcpd_leases_merge(struct dhcpd_leases *dst, const struct dhcpd_leases *src)
{
	int i, j;

	for (i = 0; i < src->count; i++) {
		const struct dhcpd_lease *s = &src->v[i];
		struct dhcpd_lease *d = NULL;

		for (j = 0; j < dst->count; j++)
			if (dst->v[j].ip_be == s->ip_be) {
				d = &dst->v[j];
				break;
			}
		if (d == NULL) {
			if (dhcpd_leases_put(dst, s) != 0)
				return -1;
			continue;
		}
		{
			char keep_name[DHCPD_HOSTNAME_MAX];
			int same_client = strcmp(d->mac, s->mac) == 0;

			snprintf(keep_name, sizeof(keep_name), "%s", d->hostname);
			if (s->ends == 0 || (d->ends != 0 && s->ends > d->ends))
				*d = *s;
			if (same_client && d->hostname[0] == '\0')
				snprintf(d->hostname, sizeof(d->hostname), "%s",
				         keep_name[0] != '\0' ? keep_name : s->hostname);
		}
	}
	return 0;
}

/* Whether a lease is a client's address right now. */
static inline int dhcpd_lease_is_current(const struct dhcpd_lease *l, long long now)
{
	if (l->binding != DHCPD_BINDING_ACTIVE)
		return 0;
	return l->ends == 0 || l->ends > now;
}

#endif /* DHCPDCONF_H */
