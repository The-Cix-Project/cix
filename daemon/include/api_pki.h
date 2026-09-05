#ifndef API_PKI_H
#define API_PKI_H

#include <stddef.h>

/*
 * api_pki -- REST handlers for the certificate authority tier: the
 * root CA, the intermediate that signs day-to-day certificates, the
 * certificates themselves, and the reset that rebuilds the lot
 * (ADR-0046/0047).
 *
 * reissue_host_pki_cert() lives here rather than in main.c because
 * three of its four callers are these handlers: creating a CA, creating
 * an intermediate and resetting the tier all invalidate the daemon's
 * own certificate. The fourth caller is a site-configuration change,
 * which is why it is exported rather than static.
 */

/*
 * Reissues the daemon's own certificate. Exported because a site
 * configuration change invalidates it too, and that handler stays in
 * main.c -- one reissue path rather than two that agree until they do
 * not.
 */
void reissue_host_pki_cert(void);

void handle_pki_ca_create(int fd, const char *body, size_t body_len);
void handle_pki_ca_get(int fd);
void handle_pki_cert_create(int fd, const char *body, size_t body_len);
void handle_pki_cert_delete(int fd, const char *name);
void handle_pki_cert_get_one(int fd, const char *name);
void handle_pki_cert_list(int fd);
void handle_pki_intermediate_create(int fd, const char *body, size_t body_len);
void handle_pki_intermediate_get(int fd);
void handle_pki_reset(int fd, const char *body, size_t body_len);

#endif /* API_PKI_H */
