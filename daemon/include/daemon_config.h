#ifndef DAEMON_CONFIG_H
#define DAEMON_CONFIG_H

#include "json.h"

/*
 * cixd's own live-reconfigurable settings (Part 0.5) -- deliberately
 * small. Since ADR-0287 this module owns THE single persisted truth for
 * where cixd listens off-box: management_address. The network that
 * address lives in, and that network's `management` flag, are DERIVED
 * from it in network.c (never stored there), so the two can never drift.
 * This replaces the old split where a `management_network` name lived in
 * network.c and an optional `bind_ip` override lived here (ADR-0068,
 * superseded) -- two inputs for one truth.
 *
 * http_enabled/https_enabled/https_port landed together with the real
 * OpenSSL/TLS listener they control (main.c) -- deliberately not added
 * any earlier, when there was no working HTTPS listener yet: a
 * persisted toggle with nothing real behind it would have been exactly
 * the kind of half-working stop-gap this project's own Immutable
 * Maxims forbid. daemon_config_set_http_enabled(0)/daemon_config_set_
 * https_enabled(0) each refuse if the other is already disabled --
 * this daemon must always have at least one working listener.
 *
 * Both default to enabled on a fresh install (ADR-0171), on ports
 * 80/443 (DEFAULT_PORT/DEFAULT_HTTPS_PORT) -- matching a real deployed
 * install's own actual configuration, confirmed directly against
 * 192.168.15.95. A pre-PKI-bootstrap install just has HTTPS silently
 * unavailable at boot until a host cert exists to serve TLS with; not
 * a hazard, see create_tls_ctx()'s own soft-fail in main.c.
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
	DAEMON_CONFIG_ERR_INVALID_MANAGEMENT_ADDRESS /* too long to ever be a real IPv4
	                                     * dotted-quad -- the api_network.c handler's own
	                                     * inet_pton()/subnet-membership validation
	                                     * already rejects a malformed or unmatched
	                                     * address before this module ever sees it, so
	                                     * this is a defensive backstop, not the primary
	                                     * validation path */
};

/* Longest IPv4 dotted-quad text form ("255.255.255.255" + NUL), same
 * bound INET_ADDRSTRLEN already gives -- spelled out as a literal so
 * this header doesn't need <netinet/in.h> just for one constant. */
#define DAEMON_CONFIG_MANAGEMENT_ADDRESS_MAX 16

/*
 * Loads state_path (the persisted daemon config, if any) at startup.
 * Absent file (first-ever boot, or a --data-dir= test invocation) is
 * not an error -- port defaults to 0, meaning "no persisted override,
 * use whatever main() was given on argv/compiled-in default." Returns
 * 0, or -1 if the file exists but is malformed.
 */
int daemon_config_init(const char *state_path);

/* ADR-0141 Phase 2: repoints without reloading -- see network_repoint()'s
 * own doc comment for the shared reasoning. */
void daemon_config_repoint(const char *new_state_path);

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

/* Whether the plain-HTTP listener should be up. Defaults to 1, and
 * https_enabled (below) now defaults to 1 too (ADR-0171) -- a fresh
 * install starts with both listeners on, not HTTP-only. */
int daemon_config_http_enabled(void);
int daemon_config_https_enabled(void);

/*
 * ADR-0207 phase 3: the platform-wide default for containers created
 * without an explicit "userns" field. 1 (the fresh-install default)
 * means secure-by-default -- CLONE_NEWUSER + a subordinate-ID mapping
 * unless the container opts out; 0 restores opt-in. The per-container
 * field always wins over this default in both directions.
 */
int daemon_config_userns_default(void);
enum daemon_config_error daemon_config_set_userns_default(int enabled);
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
 * The single persisted management address (ADR-0287) -- the IPv4
 * dotted-quad cixd answers on off-box -- or NULL when none is set, the
 * deliberate loopback-only state (cixd still answers on 127.0.0.1,
 * always). The address lives on the bridge of whichever network's
 * subnet contains it (that network is DERIVED, network.c computes it);
 * the api_network.c handler adds it to that bridge and rebinds the
 * off-box listeners before this module ever persists it, so this
 * accessor only ever reports state that is already real. Pointer is
 * valid only until the next daemon_config_set_management_address() call.
 */
const char *daemon_config_management_address(void);

/*
 * Persists address (a real IPv4 dotted-quad string), or clears it
 * entirely when address is NULL (reset to loopback-only). Like every
 * other setter here, does NOT itself touch any bridge or listener --
 * the api_network.c handler does the real rtnl_addr_add_ipv4()/
 * rtnl_addr_del_ipv4() work and the listener rebind first, and only
 * calls this once that has actually succeeded.
 */
enum daemon_config_error daemon_config_set_management_address(const char *address);

/* Writes {"port", "http_enabled", "https_enabled", "https_port"} into w
 * -- just the listener config. WHERE cixd binds off-box is a separate
 * resource now (GET /system/management-address, ADR-0287); this module
 * no longer contributes any address field to daemon-config. */
void daemon_config_write_json(struct json_writer *w);

#endif /* DAEMON_CONFIG_H */
