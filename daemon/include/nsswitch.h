#ifndef NSSWITCH_H
#define NSSWITCH_H

#include <stddef.h>

/*
 * The one place that decides what /etc/nsswitch.conf says (#478,
 * ADR-0296).
 *
 * There are two callers and they used to carry separate string
 * literals: pkg_seed_image_baseline() writes an image's baseline copy,
 * and create_container_from_body()'s ldap_client block REPLACES it for
 * a container that joins the directory. They disagreed, and the
 * disagreement is what #478 is:
 *
 *   baseline     "hosts: files"   -- no dns backend, so glibc never
 *                                   consults DNS and a staged
 *                                   resolv.conf is inert
 *   ldap_client  (no hosts line)  -- so glibc's own compiled-in
 *                                   default supplies one, which does
 *                                   include dns
 *
 * The result was that the container given an explicit, pinned file
 * could not resolve names and the container given an incomplete one
 * could -- by accident, via the very glibc default ADR-0111 wrote the
 * file to avoid depending on. Measured on 192.168.15.95, 2026-09-17:
 * a jumpbox container on `services` had a correct resolv.conf,
 * libnss_dns.so.2 present, and `getent hosts` rc=2, while `jump`
 * resolved every name tried.
 *
 * So the hosts line is defined once, both contents are built from it,
 * and test_nsswitch asserts they agree. Two hand-written copies of one
 * rule is how they drifted the first time.
 */

/*
 * "files dns": the local file first, then the resolvers a container is
 * given (ADR-0143's dns_servers, ADR-0295's default). glibc loads the
 * dns backend as a dlopen()ed plugin, and that plugin ships in the
 * glibc PACKAGE -- measured 2026-09-17, glibc 2.44-16 in jumpbox
 * carries 13 libnss files, libnss_dns.so.2 among them, and nothing
 * in this tree stages any of them. So every image with glibc
 * already had the backend: it was present and simply never asked
 * for.
 */
#define NSSWITCH_HOSTS_LINE "hosts:          files dns\n"

/* The baseline every image gets at creation and on every install. */
const char *nsswitch_baseline_content(void);

/*
 * The replacement staged into a container that declares ldap_client.
 * Accounts resolve through nslcd; hosts resolve exactly as they do
 * everywhere else, which is the half that used to be missing.
 */
const char *nsswitch_ldap_client_content(void);

/*
 * Whether an existing file's bytes need replacing with want. True when
 * have is NULL (absent) or differs in any byte.
 *
 * This is the converge decision, and it is a decision rather than a
 * detail: the block in pkg.c used to write only when the file was
 * ABSENT, which is why every image already built kept "hosts: files"
 * and would have kept it forever. A file the platform declares the
 * content of is a file the platform has to keep correct.
 */
int nsswitch_needs_write(const char *have, size_t have_len, const char *want);

#endif /* NSSWITCH_H */
