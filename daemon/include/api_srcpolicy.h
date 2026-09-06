#ifndef API_SRCPOLICY_H
#define API_SRCPOLICY_H

#include <stddef.h>

/* ADR-0255: the source-policy REST surface. See srcpolicy.h for the
 * axis itself and why it is not pkgpolicy.h's. */
void handle_upstream_kinds_get(int fd);
void handle_source_policy_get(int fd);
void handle_source_policy_default_put(int fd, const char *body, size_t body_len);
void handle_pkg_source_policy_put(int fd, const char *name, const char *body, size_t body_len);
void handle_pkg_source_policy_delete(int fd, const char *name);

#endif /* API_SRCPOLICY_H */
