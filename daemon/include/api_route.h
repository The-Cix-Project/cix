#ifndef API_ROUTE_H
#define API_ROUTE_H

#include <stddef.h>

/*
 * api_route -- the box's own real kernel IPv4 routing table (ADR-0066/67).
 *
 * Worth having at all because there is no SSH and no general shell on an
 * installed host: without this endpoint nobody can ever see the routing
 * table. Writes go straight to rtnetlink, never to an iproute2 binary.
 */

void handle_route_add(int fd, const char *body, size_t body_len);
void handle_route_del(int fd, const char *body, size_t body_len);
void handle_route_list(int fd);

#endif /* API_ROUTE_H */
