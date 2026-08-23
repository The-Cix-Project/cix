#ifndef DNS_H
#define DNS_H

#include "json.h"

#include <stdint.h>
#include <sys/types.h>

/*
 * Phase 8 part 1: DNS as a REST resource (name -> IP records), with
 * actual resolution done by a real DNS server (dnsmasq) running as a
 * normal containerized workload -- not hand-rolled, per the same
 * reasoning BIRD wasn't hand-rolled for routing in Phase 7 (ADR-0007's
 * "hand-rolled, no external libraries" rule is about this project's
 * own platform components, not about workloads a container runs).
 */

#define DNS_MAX_RECORDS 256
#define DNS_NAME_MAX 254 /* RFC 1035 */
#define DNS_OWNER_NAME_MAX 64 /* matches REGISTRY_NAME_MAX by value, no header dependency */

struct dns_record {
	char name[DNS_NAME_MAX];
	uint32_t ip_be; /* network byte order */
	/* Empty string: created directly via POST /v1/dns/records, not
	 * tied to any container's lifecycle. Non-empty: this container's
	 * name -- the record is auto-deleted when it's deleted (see
	 * dns_record_forget_owner()). */
	char owner_container[DNS_OWNER_NAME_MAX];
};

enum dns_error {
	DNS_OK = 0,
	DNS_ERR_INVALID_NAME,
	DNS_ERR_INVALID_IP,
	DNS_ERR_DUPLICATE,
	DNS_ERR_FULL,
	DNS_ERR_NOT_FOUND,
	DNS_ERR_PERSIST_FAILED
};

/* Loads persisted records (if any) at startup. Unlike networks, there
 * is no kernel state to recreate here -- just the in-memory table.
 * servers_state_path is the sibling persisted state for dns_server_
 * register()'s own bindings (see below) -- loaded here too so both
 * come back in one call, but they're independent record sets with
 * independent files. */
int dns_init(const char *state_path, const char *servers_state_path);

/* ADR-0141 Phase 2: repoints where future saves write to, without
 * reloading/discarding already-live in-memory records/bindings -- see
 * network_repoint()'s own doc comment for the full reasoning, shared
 * verbatim by every STATE_DIR-backed module. */
void dns_repoint(const char *new_state_path, const char *new_servers_state_path);

/*
 * Hostname validation (dot-separated labels of [A-Za-z0-9-], each
 * 1-63 chars, <=DNS_NAME_MAX-1 total -- RFC 1035). Exported so other
 * modules whose own resources are conceptually hostnames (e.g. a PKI
 * certificate's CN/SANs) can reuse this instead of re-implementing
 * the same label/charset/length rules a second time.
 */
int dns_name_is_valid(const char *name);

enum dns_error dns_record_create(const char *name, uint32_t ip_be, const char *owner_container,
                                  struct dns_record **out);
/* Full field replacement (task #749) -- name is authoritative from the
 * URL path, ip is the only other field a plain (non-owner-tracked)
 * record has; owner_container is never editable via this path. */
enum dns_error dns_record_update(const char *name, uint32_t ip_be, struct dns_record **out);
enum dns_error dns_record_delete(const char *name);
struct dns_record *dns_record_find(const char *name);

/* Best-effort cleanup on container deletion: deletes name's record iff
 * it exists and its owner_container is name itself. Safe no-op for
 * every container that never had an auto-registered record, so this
 * is called unconditionally from the container-delete handler. */
void dns_record_forget_owner(const char *container_name);

void dns_write_json_one(const struct dns_record *rec, struct json_writer *w);
void dns_write_json_list(struct json_writer *w);

/*
 * Serializes every current record as "<ip> <name>" lines (dnsmasq's
 * --addn-hosts format) and writes them atomically to abs_path.
 */
int dns_write_hosts_file(const char *abs_path);

#define DNS_SERVER_MAX 32
#define DNS_SERVER_NAME_MAX 64   /* matches REGISTRY_NAME_MAX */
#define DNS_SERVER_PATH_MAX 256

struct dns_server_binding {
	char container_name[DNS_SERVER_NAME_MAX];
	char hosts_path[DNS_SERVER_PATH_MAX]; /* relative, as the container itself sees it */
};

enum dns_server_error {
	DNS_SERVER_OK = 0,
	DNS_SERVER_ERR_INVALID_PATH,
	DNS_SERVER_ERR_DUPLICATE,
	DNS_SERVER_ERR_FULL,
	DNS_SERVER_ERR_WRITE_FAILED,
	DNS_SERVER_ERR_NOT_FOUND
};

/*
 * Registers container_name as a DNS-serving target: writes the
 * current full record set to /proc/<pid>/root/<hosts_path> (the
 * container's own view of that path, reached through the magic procfs
 * symlink -- writing directly to the container's raw upperdir does
 * NOT work: the kernel does not guarantee an already-mounted overlay
 * notices changes made to the upper layer from outside it; verified
 * empirically before this was built, see docs/roadmap/ROADMAP.md Phase 8),
 * then sends SIGHUP via pidfd (dnsmasq's documented "reload
 * --addn-hosts files" signal) so an already-running dnsmasq picks up
 * this initial write immediately. hosts_path is rejected
 * (DNS_SERVER_ERR_INVALID_PATH) unless it's an absolute path (starts
 * with '/') containing no "..". DNS_SERVER_ERR_DUPLICATE if
 * container_name is already registered -- unregister first to change
 * its hosts_path.
 */
enum dns_server_error dns_server_register(const char *container_name, pid_t pid, int pidfd,
                                           const char *hosts_path);
enum dns_server_error dns_server_unregister(const char *container_name);

/* Called after every record create/delete: rewrites the hosts file
 * and re-sends SIGHUP for every currently-registered binding. Also
 * the right call to make once, after boot-time container autostart
 * completes, so freshly-live containers (fresh pid, per ADR-0091)
 * actually receive the bindings dns_init() just loaded from disk --
 * at load time no container has started yet, so any earlier sync
 * attempt would find nothing running to write to. */
void dns_server_sync_all(void);

/* Called when a container is removed, so a stale binding never
 * lingers referencing a name (and pidfd) that no longer exists. */
void dns_server_forget(const char *container_name);

void dns_server_write_json_one(const struct dns_server_binding *binding, struct json_writer *w);
void dns_server_write_json_list(struct json_writer *w);

/* Issue #81: uniform enumerator for the shared server-health prober. */
int dns_server_list_containers(char out[][DNS_SERVER_NAME_MAX], int max);

/* Whether this container is a registered DNS server. Exists so another
 * service can REPORT the pairing (a DHCP server that is also this is a
 * DHCP server whose leases resolve) without reaching into this one's
 * table or depending on it. */
int dns_server_is_registered(const char *container);

/* Issue #83: number of records currently managed -- used to detect the
 * silent state where records exist with no registered server to receive
 * them. */
int dns_record_count(void);

#endif /* DNS_H */
