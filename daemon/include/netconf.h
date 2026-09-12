#ifndef NETCONF_H
#define NETCONF_H

#include <stddef.h>

/*
 * net.conf -- the management network a booted box comes up on, held on
 * the cix-config partition.
 *
 * One module because two programs need the identical format and, until
 * PUT /v1/system/management-network existed, they did not share it:
 * cix-install wrote the file with its own snprintf() and cixd parsed it
 * with its own static parse_net_conf(). That was survivable only while
 * there was exactly one writer. Making the daemon a second writer would
 * have put two independent definitions of one format in two programs --
 * the One Source of Truth failure this project's maxims forbid -- so
 * the format moved here first and both callers were repointed at it.
 *
 * The file is four "key=value" lines:
 *
 *     ip=192.168.15.95
 *     prefix=24
 *     gateway=192.168.15.1
 *     interface=eth0
 *
 * ABSENCE of the file is meaningful and is not an error: it is how an
 * installed box records "no management network was chosen", which
 * cix-install offers deliberately. bootstrap_management_network() then
 * does nothing and cixd answers on its compiled-in loopback default.
 * An EMPTY gateway is likewise a real configuration, not a partial one
 * -- a box reachable only on its own subnet needs no default route,
 * and a loopback-managed box has nowhere to route to.
 */

/*
 * Reads path. Returns 0 when it describes a usable network, -1 when the
 * file is absent or does not (an ip, a prefix and an interface are all
 * required; a gateway is not). out_gateway is set to the empty string
 * when the file carries no gateway, which callers must treat as "no
 * default route" rather than as a parse failure.
 */
int netconf_parse(const char *path, char *out_ip, size_t ip_size, int *out_prefix,
                   char *out_gateway, size_t gateway_size, char *out_interface,
                   size_t interface_size);

/*
 * Writes path atomically -- a temporary file in the same directory,
 * fsync'd, then rename(2)d over the target. The whole point of this
 * file is to be read on the next boot, so a half-written one is a box
 * that does not come back; rename(2) is what makes a reader see either
 * the old contents or the new ones and never a mixture.
 *
 * gateway may be NULL or "" for a box with no default route; the
 * gateway= line is still written, empty, so the file's shape does not
 * depend on whether one is set. Returns 0, or -1 with errno set.
 */
int netconf_write(const char *path, const char *ip, int prefix, const char *gateway,
                   const char *interface);

#endif /* NETCONF_H */
