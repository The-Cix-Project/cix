#ifndef API_TRUST_H
#define API_TRUST_H

#include <stddef.h>

/*
 * ADR-0350: the REST surface of the trust-anchor store --
 * GET/POST /v1/system/trust-anchors and
 * DELETE /v1/system/trust-anchors/{fingerprint}.
 */
void handle_trust_anchor_list(int fd);
void handle_trust_anchor_add(int fd, const char *body, size_t body_len);
void handle_trust_anchor_remove(int fd, const char *fingerprint);

#endif /* API_TRUST_H */
