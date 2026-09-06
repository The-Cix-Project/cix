#ifndef API_SRCPOLICY_H
#define API_SRCPOLICY_H

#include <stddef.h>

/* ADR-0255's REST surface: the source-policy axis (see srcpolicy.h for
 * why it is not pkgpolicy.h's) and the catalogue that policy resolves
 * into (see srcresolve.h). Kept together because they are one ADR's
 * surface -- the catalogue is what a policy is FOR, and splitting them
 * across files would separate a setting from its only visible effect. */
void handle_upstream_kinds_get(int fd);
void handle_source_catalogue_get(int fd);
void handle_source_policy_get(int fd);
void handle_source_policy_default_put(int fd, const char *body, size_t body_len);
void handle_pkg_source_policy_put(int fd, const char *name, const char *body, size_t body_len);
void handle_pkg_source_policy_delete(int fd, const char *name);

#endif /* API_SRCPOLICY_H */
