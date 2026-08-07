#ifndef DAEMON_CONFIG_H
#define DAEMON_CONFIG_H

#include "json.h"

/*
 * kanxeod's own live-reconfigurable settings (Part 0.5) -- deliberately
 * small. Which network is the management one (network_def.is_management,
 * network_set_management()/network_find_management()) is NOT duplicated
 * here: that's network.c's own persisted state, and this module only
 * ever queries it live, never stores a second copy that could drift out
 * of sync.
 *
 * http_enabled/https_enabled/https_port landed together with the real
 * OpenSSL/TLS listener they control (main.c) -- deliberately not added
 * any earlier, when there was no working HTTPS listener yet: a
 * persisted toggle with nothing real behind it would have been exactly
 * the kind of half-working stop-gap this project's own Immutable
 * Maxims forbid. daemon_config_set_http_enabled(0)/daemon_config_set_
 * https_enabled(0) each refuse if the other is already disabled --
 * this daemon must always have at least one working listener.
 */

enum daemon_config_error {
	DAEMON_CONFIG_OK = 0,
	DAEMON_CONFIG_ERR_INVALID_PORT,
	DAEMON_CONFIG_ERR_PERSIST_FAILED,
	DAEMON_CONFIG_ERR_REBIND_FAILED,
	DAEMON_CONFIG_ERR_WOULD_HAVE_NO_LISTENER,
	DAEMON_CONFIG_ERR_HTTPS_NOT_AVAILABLE, /* no host cert to serve TLS with yet
	                                         * (PKI not bootstrapped) -- see
	                                         * create_tls_ctx() in main.c */
	DAEMON_CONFIG_ERR_INVALID_BIND_IP /* too long to ever be a real IPv4 dotted-quad
	                                     * -- main.c's own inet_pton()/subnet-membership
	                                     * validation already rejects a malformed
	                                     * address before this module ever sees it, so
	                                     * this is a defensive backstop, not the primary
	                                     * validation path */
};

/* Longest IPv4 dotted-quad text form ("255.255.255.255" + NUL), same
 * bound INET_ADDRSTRLEN already gives -- spelled out as a literal so
 * this header doesn't need <netinet/in.h> just for one constant. */
#define DAEMON_CONFIG_BIND_IP_MAX 16

/*
 * Loads state_path (the persisted daemon config, if any) at startup.
 * Absent file (first-ever boot, or a --data-dir= test invocation) is
 * not an error -- port defaults to 0, meaning "no persisted override,
 * use whatever main() was given on argv/compiled-in default." Returns
 * 0, or -1 if the file exists but is malformed.
 */
int daemon_config_init(const char *state_path);

/* The persisted port, or 0 if none has ever been set (main() falls
 * back to argv's --port=/DEFAULT_PORT in that case). */
int daemon_config_port(void);

/*
 * Persists port (1-65535). Does NOT itself rebind the listening
 * socket -- main.c's own handler does that (create the new socket,
 * add it to epoll, only then tear down the old one) and only calls
 * this once the rebind has actually succeeded, so the persisted value
 * never claims a port that isn't really being listened on.
 */
enum daemon_config_error daemon_config_set_port(int port);

/* Whether the plain-HTTP listener should be up. Defaults to 1 (every
 * install starts HTTP-only, matching today's actual behavior) --
 * https_enabled defaults to 0. */
int daemon_config_http_enabled(void);
int daemon_config_https_enabled(void);
/* The persisted HTTPS port, or 0 if never set (main.c falls back to a
 * fixed default, DEFAULT_HTTPS_PORT, the same shape daemon_config_
 * port()'s 0-means-fall-back-to-argv already has). */
int daemon_config_https_port(void);

/*
 * Each persists its own field and, like daemon_config_set_port(),
 * does not itself start/stop any listener -- main.c's handler does
 * that first (only a real, successful listener start/stop earns a
 * persisted change) and calls these once it has. Refuses (DAEMON_
 * CONFIG_ERR_WOULD_HAVE_NO_LISTENER) rather than persisting a state
 * that would leave both listeners off.
 */
enum daemon_config_error daemon_config_set_http_enabled(int enabled);
enum daemon_config_error daemon_config_set_https_enabled(int enabled);
enum daemon_config_error daemon_config_set_https_port(int port);

/*
 * The persisted dedicated bind IP (ADR-0068), or NULL if none is set
 * -- in which case kanxeod binds to the management network's own
 * address instead (main.c's own fallback, this module knows nothing
 * about which network that is). A non-NULL value is always a second,
 * explicit IPv4 address living on the management network's bridge
 * alongside its own address, added there by main.c's handler via
 * rtnl_addr_add_ipv4() before this module ever persists it -- this
 * accessor only ever reports state that's already real. Pointer is
 * only valid until the next daemon_config_set_bind_ip() call.
 */
const char *daemon_config_bind_ip(void);

/*
 * Persists ip (a real IPv4 dotted-quad string), or clears the
 * persisted bind_ip entirely when ip is NULL. Like every other setter
 * here, does NOT itself touch the bridge or any listener -- main.c's
 * handler does the real rtnl_addr_add_ipv4()/rtnl_addr_del_ipv4() work
 * and the listener rebind first, and only calls this once that has
 * actually succeeded.
 */
enum daemon_config_error daemon_config_set_bind_ip(const char *ip);

/* Writes {"port", "http_enabled", "https_enabled", "https_port",
 * "bind_ip"} into w -- the caller (main.c's GET handler) merges in
 * "management_network"/"bind" from network.c's own state, which this
 * module deliberately doesn't know about. */
void daemon_config_write_json(struct json_writer *w);

#endif /* DAEMON_CONFIG_H */
