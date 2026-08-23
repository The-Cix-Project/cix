#ifndef DHCP_H
#define DHCP_H

#include "json.h"

#include <stdint.h>
#include <sys/types.h>

/*
 * DHCP as a REST resource, served by the same dnsmasq that already
 * serves this platform's DNS.
 *
 * That pairing is the whole design, not an implementation convenience:
 * a lease handed out by an instance that is also the resolver is
 * resolvable the instant it is handed out, with no glue between the two
 * and nothing to fall out of step. So DHCP is served by EVERY
 * registered DNS server (dns_server_register()) -- "plays nice with
 * DNS" is enforced by construction rather than left to configuration.
 *
 * Redundancy is SPLIT SCOPE, and it has to be, because dnsmasq
 * implements no failover protocol -- there is no equivalent of ISC
 * dhcpd's peer relationship for it to join. The standard, real
 * technique in its absence is to give each server a disjoint slice of
 * one range: both answer, a client takes whichever offer arrives
 * first, and handing the same address to two machines is impossible
 * because no two servers hold it. Either server alone keeps serving
 * from its own slice, which is what redundancy has to mean here.
 *
 * The slices are computed by this daemon, not configured: an operator
 * setting them by hand would be maintaining, in two places, a division
 * that has exactly one correct answer given the range and the number of
 * servers.
 *
 * A lease is NOT a DNS record here. A record is durable operator intent
 * (dns.c, persisted, survives every server); a lease is short-lived
 * state owned by the server that issued it. Copying leases into the
 * record store would put two writers in one namespace and leave a
 * record pointing at an address someone else now holds the moment a
 * lease expired uncleanly. Leases are read back from the server's own
 * lease file instead -- GET /v1/dhcp/leases -- and resolve through that
 * same dnsmasq regardless.
 *
 * Two rendered files, because dnsmasq treats them differently and
 * pretending otherwise would mean silently-inert settings:
 *   - the hosts file (static MAC->IP reservations) is re-read on
 *     SIGHUP, so it updates live exactly like the DNS hosts file;
 *   - the conf file (ranges, lease time, options) is read only at
 *     startup, so changing a range restarts the serving container.
 */

#define DHCP_MAX_NETWORKS 32
#define DHCP_MAX_STATIC 128
#define DHCP_NETWORK_NAME_MAX 64  /* matches NETWORK_NAME_MAX by value, no header dependency */
#define DHCP_SERVER_NAME_MAX 64   /* matches REGISTRY_NAME_MAX by value */
#define DHCP_MAC_MAX 18           /* "aa:bb:cc:dd:ee:ff" + NUL */
#define DHCP_HOSTNAME_MAX 64

/* One hour: long enough not to churn, short enough that a range change
 * or a decommissioned client frees its address the same afternoon. */
#define DHCP_DEFAULT_LEASE_SECONDS 3600

/* Fixed, well-known paths inside a serving container -- the same
 * "one name the platform knows" convention HOST_TOOLS_IMAGE uses.
 * A dnsmasq container must be started against these three; see
 * docs/api/README.md for the exact command. */
#define DHCP_CONF_PATH "/etc/dnsmasq-dhcp.conf"
#define DHCP_HOSTS_PATH "/etc/dnsmasq-dhcp-hosts"
#define DHCP_LEASE_PATH "/run/dnsmasq.leases"

struct dhcp_network {
	char network[DHCP_NETWORK_NAME_MAX];
	int enabled;
	uint32_t range_start_be;
	uint32_t range_end_be;
	int lease_seconds;
	uint32_t router_be; /* 0 = advertise no default route */
};

struct dhcp_static {
	char mac[DHCP_MAC_MAX];
	uint32_t ip_be;
	char hostname[DHCP_HOSTNAME_MAX]; /* "" = reservation only, no name */
};

enum dhcp_error {
	DHCP_OK = 0,
	DHCP_ERR_INVALID,
	DHCP_ERR_NOT_FOUND,
	DHCP_ERR_DUPLICATE,
	DHCP_ERR_FULL,
	DHCP_ERR_PERSIST_FAILED
};

int dhcp_init(const char *path);
void dhcp_repoint(const char *path);

const struct dhcp_network *dhcp_network_find(const char *network);
enum dhcp_error dhcp_network_set(const struct dhcp_network *cfg);
enum dhcp_error dhcp_network_delete(const char *network);

enum dhcp_error dhcp_static_add(const struct dhcp_static *entry);
enum dhcp_error dhcp_static_delete(const char *mac);

/* Renders the whole current state, always in full -- the same
 * always-rewrite-everything discipline dns_write_hosts_file() uses,
 * for the same reason: a partial update is a second thing that can be
 * wrong. Returns bytes written (excluding NUL), or -1 if it did not
 * fit. */
/*
 * Renders only what THIS server should serve: its own slice of the
 * ranges on the networks it is actually attached to. A server on a
 * different bridge gets none of them -- it has no interface in that
 * subnet, so the pool would be one it could not serve on.
 */
int dhcp_render_conf(const char *server_name, char *out, size_t out_size);

/* The registered DNS servers attached to this network -- who actually
 * serves DHCP here, and therefore how many ways the range is split. */
int dhcp_servers_on_network(const char *network, char out[][64], int max);

/* The slice server_index of server_count would be given, for reporting
 * it back. Returns -1 if the range cannot be divided that many ways. */
int dhcp_slice_for(const struct dhcp_network *cfg, int server_index, int server_count,
                    uint32_t *out_start_be, uint32_t *out_end_be);
int dhcp_render_hosts(char *out, size_t out_size);

/*
 * Writes both files into every currently-running DNS server container
 * and SIGHUPs it, which puts a static-entry change into force
 * immediately. Ranges are NOT live: `changed` is filled with the names
 * of the servers whose conf now differs from what they were started
 * against -- only those need restarting, so a change on one network
 * never disturbs a server on another.
 */
void dhcp_sync_all(char changed[][64], int max_changed, int *out_changed_count);

/* Whether any network currently has DHCP enabled. */
int dhcp_any_enabled(void);

void dhcp_write_json(struct json_writer *w);
/*
 * include_slices adds which registered server holds which part of the
 * range -- computed, never stored, so it is written for the API and not
 * for the state file. Seeing the split is how an operator knows
 * redundancy is actually in place rather than configured.
 */
void dhcp_network_write_json(const struct dhcp_network *cfg, int include_slices,
                              struct json_writer *w);

/* Reads leases back from each running server's own lease file. This
 * is the server's state, not ours -- read every time, never cached. */
void dhcp_leases_write_json(struct json_writer *w);

/* Called when a network is deleted, so its DHCP config cannot outlive
 * the thing it configures. */
void dhcp_forget_network(const char *network);

#endif /* DHCP_H */
