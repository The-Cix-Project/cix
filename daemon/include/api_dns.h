#ifndef API_DNS_H
#define API_DNS_H

#include <stddef.h>

/*
 * api_dns -- REST handlers for DNS records and the server containers
 * that serve them (ADR-0049 onward).
 *
 * Two resources rather than one on purpose: a record is data this
 * platform owns, and a server is a running container registered as
 * willing to answer for it. dns.c owns both; this is where they meet
 * HTTP. See ADR-0249 for the boundary.
 */

void handle_dns_record_create(int fd, const char *body, size_t body_len);
void handle_dns_record_delete(int fd, const char *name);
void handle_dns_record_get_one(int fd, const char *name);
void handle_dns_record_list(int fd);
void handle_dns_record_update(int fd, const char *name, const char *body, size_t body_len);
void handle_dns_server_create(int fd, const char *body, size_t body_len);
void handle_dns_server_delete(int fd, const char *name);
void handle_dns_server_list(int fd);

#endif /* API_DNS_H */
