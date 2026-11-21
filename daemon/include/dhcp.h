#ifndef DHCP_H
#define DHCP_H

#include "json.h"

#include <stdint.h>
#include <sys/types.h>

/*
 * DHCP as a self-contained service (ADR-0197), served by ISC dhcpd
 * (ADR-0386).
 *
 * Contained is the design constraint, not a tidiness preference: a
 * service that reaches into another service's registry can never be
 * lifted out and made optional. So DHCP registers its OWN servers,
 * owns its own ranges and reservations, and every endpoint it has lives
 * under /v1/dhcp. Nothing DHCP-shaped hangs off another resource.
 *
 * Every range is served by one registered dhcpd container, or by two
 * forming a failover pair in load-balancing mode: both serve, and a
 * survivor takes the whole pool by itself after auto-partner-down
 * (ADR-0386 decision 3). cixd renders /etc/dhcpd.conf into each server
 * and never speaks dhcpd's control protocol.
 *
 * A lease is not a DNS record. A record is durable operator intent
 * (dns.c, persisted, survives every server); a lease is short-lived
 * state owned by the server that issued it. Leases are read back from
 * the servers' own lease files, in a helper child (ADR-0342: the file is
 * an append log), and their names reach DNS through a file of their own
 * (dns.c's leases file), never the records.
 *
 * dhcpd reads its conf only at start, so a change that alters a
 * server's rendered conf restarts that server through the rolling
 * restart timer, and the pair's roll_group keeps the two apart
 * (ADR-0385).
 */

#define DHCP_MAX_NETWORKS 32
#define DHCP_MAX_SERVERS 16
/* ADR-0386: one server, or a failover pair. */
#define DHCP_MAX_RANGE_SERVERS 2
#define DHCP_MAX_STATIC 128
#define DHCP_MAX_DNS 4
#define DHCP_NETWORK_NAME_MAX 64  /* matches NETWORK_NAME_MAX by value, no header dependency */
#define DHCP_SERVER_NAME_MAX 64   /* matches REGISTRY_NAME_MAX by value */
#define DHCP_MAC_MAX 18           /* "aa:bb:cc:dd:ee:ff" + NUL */
#define DHCP_HOSTNAME_MAX 64

/* One hour: long enough not to churn, short enough that a range change
 * or a decommissioned client frees its address the same afternoon. */
#define DHCP_DEFAULT_LEASE_SECONDS 3600

/*
 * Fixed, well-known paths inside a serving container. A dhcpd container
 * runs `dhcpd -f -cf DHCP_CONF_PATH -lf DHCP_LEASE_PATH --no-pid` with a
 * volume at DHCP_LEASE_DIR; see docs/guides/network-services.md.
 *
 * The lease file lives on that volume, so a restart or a recreate keeps
 * every lease (#698), and dhcpd refuses to start without it (measured,
 * ADR-0386), which is why cixd creates it there, empty, when it is
 * absent (dhcp_seed_lease_file()).
 */
#define DHCP_CONF_PATH "/etc/dhcpd.conf"
#define DHCP_LEASE_DIR "/var/lib/dhcp"
#define DHCP_LEASE_FILE "dhcpd.leases"
#define DHCP_LEASE_PATH DHCP_LEASE_DIR "/" DHCP_LEASE_FILE
/* The largest rendered dhcpd.conf: every reservation, every range. */
#define DHCP_CONF_RENDER_MAX 65536

/*
 * ADR-0383: a relay's one file, rendered by cixd like a server's, and
 * the service in the relay container that reads it. A change restarts
 * that service, never the container: the cores that relay also route.
 */
#define DHCP_RELAY_CONF_PATH "/etc/dnsmasq-relay.conf"
#define DHCP_MAX_RELAYS 16
#define DHCP_MAX_RANGE_RELAYS 8
#define DHCP_SERVICE_NAME_MAX 64
#define DHCP_RELAY_SERVICE_DEFAULT "dhcp-relay"

struct dhcp_network {
	char network[DHCP_NETWORK_NAME_MAX];
	int enabled;
	uint32_t range_start_be;
	uint32_t range_end_be;
	int lease_seconds;
	/* The default route handed to clients on this network; 0 sends
	 * none (dhcpd has no default that names itself). */
	uint32_t router_be;
	/*
	 * ADR-0386: the DNS servers handed to clients. Explicit because
	 * DHCP does not read the DNS registry (containment, ADR-0197);
	 * dnsmasq used to send its own address when this was unsaid, and
	 * dhcpd sends nothing.
	 */
	uint32_t dns_be[DHCP_MAX_DNS];
	int dns_count;
	/* The registered DHCP servers that serve this range: one, or two
	 * forming a failover pair (ADR-0386). */
	char servers[DHCP_MAX_RANGE_SERVERS][DHCP_SERVER_NAME_MAX];
	int server_count;
	/*
	 * ADR-0383: the registered relays on this network that carry its
	 * requests to the servers above that are not on it. Empty for a
	 * range every server is attached to.
	 */
	char relays[DHCP_MAX_RANGE_RELAYS][DHCP_SERVER_NAME_MAX];
	int relay_count;
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
	DHCP_ERR_PERSIST_FAILED,
	/* ADR-0383: a container is a server or a relay, never both -- on a
	 * shared interface dnsmasq's relay function wins (its manual). */
	DHCP_ERR_CONFLICT,
	/* ADR-0386: a reservation inside a range's pool. dhcpd gives a
	 * fixed-address host its address without a lease, so the two would
	 * collide. */
	DHCP_ERR_IN_POOL
};

int dhcp_init(const char *path);
void dhcp_repoint(const char *path);

const struct dhcp_network *dhcp_network_find(const char *network);
enum dhcp_error dhcp_network_set(const struct dhcp_network *cfg);
enum dhcp_error dhcp_network_delete(const char *network);

enum dhcp_error dhcp_static_add(const struct dhcp_static *entry);
enum dhcp_error dhcp_static_delete(const char *mac);

/*
 * ADR-0386: why a range's servers cannot form what it asks for, or 0.
 * Two servers must be distinct, share a network for their peer channel,
 * and pair with each other only: a server is in at most one pair. The
 * reason goes to err. Reads the registry for attachments.
 */
int dhcp_pair_refused(const struct dhcp_network *cfg, char *err, size_t err_size);

/*
 * One server's whole /etc/dhcpd.conf. A pool is rendered only when it
 * can be served safely: a range of two servers whose peer cannot be
 * resolved yet (the peer not created, or sharing no network) is
 * declared without its pool, never as two pools with no failover.
 *
 * self/self_count are the container's own attachments. NULL reads them
 * from the registry; the create path passes its planned attachments,
 * because staging runs before the container is in the registry.
 * Returns bytes written, or -1 when it did not fit.
 */
struct registry_network_attachment;
int dhcp_render_conf(const char *server_name, const struct registry_network_attachment *self,
                     int self_count, char *out, size_t out_size);

/*
 * Creates DHCP_LEASE_FILE empty in a server's lease volume, host-side,
 * when it does not exist, and never touches one that does. dhcpd will
 * not start without it. 0, or -1 with errno set.
 */
int dhcp_seed_lease_file(const char *volume_host_dir);

/* ---- Servers: DHCP registers its own, like every other service ---- */

enum dhcp_error dhcp_server_register(const char *container);
enum dhcp_error dhcp_server_unregister(const char *container);
int dhcp_server_is_registered(const char *container);
int dhcp_server_list(char out[][DHCP_SERVER_NAME_MAX], int max);
/* Each server, with its failover state from the last lease scan. */
void dhcp_servers_write_json(struct json_writer *w);

/* ---- Relays (ADR-0383): registered like servers ---- */

enum dhcp_error dhcp_relay_register(const char *container, const char *service);
enum dhcp_error dhcp_relay_unregister(const char *container);
int dhcp_relay_is_registered(const char *container);
/* The service in that container that runs the relay, or NULL. */
const char *dhcp_relay_service(const char *container);
int dhcp_relay_list(char out[][DHCP_SERVER_NAME_MAX], int max);
void dhcp_relays_write_json(struct json_writer *w);
/* One relay's /etc/dnsmasq-relay.conf, from the current state and the
 * registry's attachments (self as for dhcp_render_conf()). Bytes
 * written, or -1 when it did not fit. */
int dhcp_render_relay_conf(const char *relay_name, const struct registry_network_attachment *self,
                           int self_count, char *out, size_t out_size);
/*
 * Writes every running relay's file, and fills `changed` with the relays
 * whose file now differs from the one their service started with: only
 * those need their service restarted (the caller does it).
 */
void dhcp_relays_sync(char changed[][DHCP_SERVER_NAME_MAX], int max_changed,
                      int *out_changed_count);

/*
 * Writes every running server's /etc/dhcpd.conf, and fills `changed`
 * with the servers whose file differed: dhcpd has no reload, so only
 * those need restarting, and the caller decides whether to.
 */
void dhcp_sync_all(char changed[][DHCP_SERVER_NAME_MAX], int max_changed,
                    int *out_changed_count);

/* Whether any network currently has DHCP enabled. */
int dhcp_any_enabled(void);

void dhcp_write_json(struct json_writer *w);
void dhcp_network_write_json(const struct dhcp_network *cfg, struct json_writer *w);

/* Called when a network is deleted, so its DHCP config cannot outlive
 * the thing it configures. */
void dhcp_forget_network(const char *network);

/* ---- Leases (ADR-0386 decision 7) ---- */

/*
 * The lease scan, run only in a helper child (dhcp_scan_tick() in
 * main.c): reads every running server's lease file through its mount
 * namespace, merges the pair's two answers, and writes the snapshot
 * dhcp_snapshot_apply() installs -- each server's failover state and the
 * leases current at `now` -- to fd as JSON. 0, or -1.
 */
int dhcp_leases_scan(int fd, long long now);

/*
 * Whether any running server's lease file moved (size or mtime) since
 * the last call that answered 1. One stat per server, cheap enough for
 * the loop; it decides whether a timed scan is worth a fork.
 */
int dhcp_lease_files_moved(void);

/* The parent's half: installs a snapshot the child wrote, announcing
 * getDhcpServers and getDhcpLeases when what they answer changed. 0, or
 * -1 when it did not parse (the previous one is kept). */
int dhcp_snapshot_apply(const char *json, size_t len);

/* The last snapshot's leases still current at `now`, as a JSON array. */
void dhcp_leases_write_json(struct json_writer *w, long long now);

/* The names of the reservations and of the last snapshot's leases still
 * current at `now`. */
struct dhcp_lease_name {
	uint32_t ip_be;
	char hostname[DHCP_HOSTNAME_MAX];
};
int dhcp_lease_names(struct dhcp_lease_name *out, int max, long long now);

#endif /* DHCP_H */
