#ifndef API_LDAP_H
#define API_LDAP_H

#include <stddef.h>

/*
 * api_ldap -- REST handlers for the directory: server containers, the
 * client configuration every container authenticates through, and the
 * users and groups themselves (issue #66).
 *
 * The server registration is deliberately here and not with the users:
 * it is "which container provides this service", the same shape as
 * DNS/NTP/syslog server registration, while a user is identity data
 * this platform owns. ldap.c owns both; this is where they meet HTTP.
 */

void handle_ldap_config_get(int fd);
void handle_ldap_config_put(int fd, const char *body, size_t body_len);
void handle_ldap_group_create(int fd, const char *body, size_t body_len);
void handle_ldap_group_delete(int fd, const char *name);
void handle_ldap_group_get_one(int fd, const char *name);
void handle_ldap_group_list(int fd);
void handle_ldap_group_update(int fd, const char *name, const char *body, size_t body_len);
void handle_ldap_server_create(int fd, const char *body, size_t body_len);
void handle_ldap_server_delete(int fd, const char *name);
void handle_ldap_server_list(int fd);
void handle_ldap_user_create(int fd, const char *body, size_t body_len);
void handle_ldap_user_delete(int fd, const char *name);
void handle_ldap_user_get_one(int fd, const char *name);
void handle_ldap_user_list(int fd);
void handle_ldap_user_update(int fd, const char *name, const char *body, size_t body_len);

#endif /* API_LDAP_H */
