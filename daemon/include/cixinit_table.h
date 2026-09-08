#ifndef CIXINIT_TABLE_H
#define CIXINIT_TABLE_H

#include "cixinit.h"
#include "json.h"

#include <stddef.h>

/*
 * ADR-0260: the daemon's side of the service table.
 *
 * One builder for both of the places a container is created -- the
 * REST create path and the package build path -- so there is exactly
 * one translation from "what a container declares" to what cix-init
 * is handed. It validates the declaration the way the rest of the
 * create path validates its fields (a message a 400 can carry), orders
 * the services so every `after` points backwards (cix-init trusts that
 * and never has to detect a cycle), and sends the result as one
 * SEQPACKET message per record.
 *
 * The declared shape, in a container body:
 *
 *   "services": [
 *     { "name": "hostkeys", "type": "oneshot", "cmd": ["/usr/bin/ssh-keygen", "-A"] },
 *     { "name": "nslcd", "cmd": ["/usr/sbin/nslcd", "-d"],
 *       "ready": { "socket": "/run/nslcd/socket" } },
 *     { "name": "sshd", "cmd": ["/usr/sbin/sshd", "-D", "-e"],
 *       "after": ["hostkeys", "nslcd"] }
 *   ]
 *
 * Per service: name (1-31 of [A-Za-z0-9_-], unique); type "daemon"
 * (default) or "oneshot"; cmd (1-32 strings, 1023 bytes packed);
 * after (names declared in the same list, any order, no cycles);
 * ready ({"tcp_port": N} | {"socket": PATH} | {"command": [...]}, plus
 * "timeout_seconds" 1-300, default 30); on_exit "restart" | "stop" |
 * "fail-container" (daemon default restart, oneshot default
 * fail-container; a oneshot cannot restart); restart_delay_seconds
 * 1-300 (default 2); stop_signal by name (default SIGTERM);
 * stop_timeout_seconds 1-300 (default 10); uid and gid (numeric,
 * default: the container's root).
 */

struct cixinit_table {
	struct cixinit_hello hello;
	struct cixinit_service svc[CIXINIT_MAX_SERVICES];
	int count;
	/*
	 * The index each service had in the request body, so responses and
	 * the persisted declaration can keep the operator's order while
	 * cix-init gets the topological one. order[i] = body index of svc[i].
	 */
	int order[CIXINIT_MAX_SERVICES];
};

/*
 * Builds the table from a body's "services" array. 0 on success; -1
 * with a 400-shaped message in err. container_ip_be (network order,
 * may be 0) is what a tcp probe connects to from inside the container.
 */
int cixinit_table_from_json(const struct json_value *jservices, unsigned int container_ip_be,
                            struct cixinit_table *out, char *err, size_t err_size);

/*
 * A table of exactly one service -- the build container's "build"
 * oneshot -- from an argv the caller already holds. argv is a
 * NULL-terminated vector. 0, or -1 if it does not fit.
 */
int cixinit_table_single(struct cixinit_table *out, const char *name, char *const argv[], int type,
                         int on_exit);

/* Index of the named service in the table's (topological) order, or -1. */
int cixinit_table_index(const struct cixinit_table *t, const char *name);

/*
 * Writes the hello then every record, one message each, to a
 * SOCK_SEQPACKET fd. The caller has created the socket with a buffer
 * large enough for the whole table to be written before cix-init
 * exists (cixinit_table_socket_bytes() says how much). 0, or -1 with
 * errno.
 */
int cixinit_table_send(const struct cixinit_table *t, int fd);

/* How many bytes the table occupies on the wire, hello included. */
size_t cixinit_table_socket_bytes(const struct cixinit_table *t);

/* The stop signal's number for a name like "SIGTERM" or "TERM"; -1 if unknown. */
int cixinit_signal_from_name(const char *name);
/* The name for a signal number this table accepts, or "?" */
const char *cixinit_signal_name(int sig);

/* Human forms of the wire constants, for responses. */
const char *cixinit_type_name(int type);
const char *cixinit_on_exit_name(int on_exit);

#endif /* CIXINIT_TABLE_H */
