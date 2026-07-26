#ifndef NETWORK_H
#define NETWORK_H

#include "json.h"

#include <stdint.h>

#define NETWORK_MAX 64
#define NETWORK_NAME_MAX 16 /* IFNAMSIZ -- a network's name IS its bridge's ifname */

enum network_error {
	NETWORK_OK = 0,
	NETWORK_ERR_INVALID_NAME,
	NETWORK_ERR_INVALID_SUBNET,
	NETWORK_ERR_DUPLICATE,
	NETWORK_ERR_OVERLAP,
	NETWORK_ERR_FULL,
	NETWORK_ERR_CREATE_FAILED,
	NETWORK_ERR_NOT_FOUND,
	NETWORK_ERR_IN_USE,
	NETWORK_ERR_DELETE_FAILED
};

struct network_def {
	char name[NETWORK_NAME_MAX];
	uint32_t base_be;    /* network address, network byte order */
	int prefix_len;
	uint32_t gateway_be; /* derived: base | 1, never persisted independently */
	int in_use;          /* 0 for free slots */
};

/*
 * Loads state_path (the persisted network list, if it exists) and
 * recreates each network's bridge idempotently (EEXIST tolerated on
 * both the bridge and its gateway address, same spirit as the
 * daemon's ensure_dir()) -- a bridge created via network_create() is
 * real, persistent kernel state that outlives this process, so a
 * restart must not forget about it. state_path is remembered for
 * subsequent network_create()/network_delete() calls to rewrite.
 * Call once at daemon startup, before serving any request.
 */
int network_init(const char *state_path);

/*
 * Validates name (1-15 chars, [A-Za-z0-9_-], unique), that subnet_str
 * parses as an IPv4 network address whose host bits are actually zero
 * for prefix_len (e.g. "172.31.0.5" with prefix_len 24 is rejected --
 * only "172.31.0.0" is valid), that prefix_len is in [8,30], and that
 * the resulting range doesn't overlap any existing network. On
 * success creates the bridge (rtnetlink) with gateway address
 * base|1, appends to the in-memory table, and atomically rewrites the
 * persisted file (write to a temp file, fsync, rename over the real
 * one -- a crash mid-write must never corrupt this state). *out
 * points at the stored entry on NETWORK_OK.
 */
enum network_error network_create(const char *name, const char *subnet_str, int prefix_len,
                                   struct network_def **out);

/*
 * Removes name. Returns NETWORK_ERR_IN_USE (not deleted) if any
 * registry entry currently reports this network as its own -- see
 * registry_network_in_use(). Otherwise deletes the bridge, removes
 * the entry, rewrites the persisted file.
 */
enum network_error network_delete(const char *name);

struct network_def *network_find(const char *name);

/*
 * network_find(name) plus IP allocation within it: delegates to the
 * existing, topology-agnostic registry_alloc_ip(), same as every
 * container's IP has been allocated since Phase 6 part 3. Returns 0
 * and fills *out_ip_be, or -1 (no such network, or the range is
 * exhausted).
 */
int network_alloc_ip(const char *name, uint32_t *out_ip_be);

void network_write_json_one(const struct network_def *net, struct json_writer *w);
void network_write_json_list(struct json_writer *w);

#endif /* NETWORK_H */
