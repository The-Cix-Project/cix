#ifndef API_NETWORK_H
#define API_NETWORK_H

#include <stddef.h>

#include "network.h"

/*
 * api_network -- REST handlers for virtual networks: creating them,
 * their ports, and attaching or detaching real host interfaces.
 *
 * network.c owns the bridges and talks to the kernel over rtnetlink
 * directly, never through iproute2. This is where it meets HTTP.
 * respond_network_error() lives here and is exported, because the
 * container attach/detach handlers report the same failures and stay
 * in main.c with the runtime they drive.
 */

void handle_network_attach_interface(int fd, const char *net_name, const char *body,                                              size_t body_len);
/* Exported: the container attach/detach handlers report the same
 * failures and stay in main.c with the runtime they drive. */
/*
 * The pure (no fd) enum-to-status resolver behind respond_network_error()
 * below. Exported because create_container_from_body() validates its own
 * networks array before it has an fd to answer on, and reuses this rather
 * than growing a second table that would drift.
 */
int network_error_to_status(enum network_error err, const char **out_msg);

void respond_network_error(int fd, enum network_error err);

void handle_network_create(int fd, const char *body, size_t body_len);
void handle_network_detach_interface(int fd, const char *net_name, const char *ifname);
void handle_network_get_one(int fd, const char *name);
void handle_network_list(int fd);
void handle_network_ports_get(int fd, const char *name);
void handle_network_update(int fd, const char *name, const char *body, size_t body_len);

#endif /* API_NETWORK_H */
