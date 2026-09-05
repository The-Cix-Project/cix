#ifndef API_HOSTAUTH_H
#define API_HOSTAUTH_H

#include <stddef.h>

/*
 * api_hostauth -- login, logout, whoami, and the host authentication
 * configuration and live sessions (ADR-0144).
 *
 * POST /v1/login is the one endpoint that always works regardless of
 * write-gating; dispatch() in main.c exempts that exact path, and the
 * exemption stays there with the dispatcher it belongs to.
 */

void handle_hostauth_config_get(int fd);
void handle_hostauth_config_put(int fd, const char *body, size_t body_len);
void handle_hostauth_sessions_get(int fd);
void handle_hostauth_sessions_revoke(int fd, const char *username);
void handle_login(int fd, const char *body, size_t body_len);
void handle_logout(int fd, const char *req_headers, size_t req_headers_len);
void handle_whoami(int fd, const char *req_headers, size_t req_headers_len);

#endif /* API_HOSTAUTH_H */
