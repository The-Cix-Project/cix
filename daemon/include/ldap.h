#ifndef LDAP_SERVER_H
#define LDAP_SERVER_H

#include "json.h"

/*
 * LDAP server registration (task #725) -- mirrors dns.h's own
 * dns_server_register()/unregister()/forget() binding mechanism
 * (daemon/include/dns.h) as closely as the two domains genuinely
 * allow, per explicit design direction: same shape (a persisted
 * container-name -> path binding table, register/unregister/forget/
 * list), same REST/CLI conventions.
 *
 * One real, deliberate difference from DNS: dnsmasq only reads its
 * --addn-hosts file once at startup, so dns_server_register() must
 * push a freshly-rendered file into the container immediately and
 * send SIGHUP on every subsequent record change (dns_server_sync_all()).
 * glauth's own embedded-SQLite backend (pkg/recipes/glauth/2.4.0/
 * recipe.sh) queries its database live on every single LDAP request --
 * confirmed directly during that recipe's own verification (a row
 * inserted with the server already running was immediately visible to
 * a subsequent ldapsearch, no restart/signal/reload needed at all).
 * So registration here is pure bookkeeping: it records which
 * container is "the" LDAP server and where its SQLite file lives
 * (resolved fresh, via /proc/<pid>/root/<db_path>, by task #726's own
 * CRUD handlers at write time -- the same "look up the registry entry
 * fresh, never cache a pid" discipline dns_server_sync_all() already
 * established) -- there is no push-and-signal step to mirror, because
 * nothing here needs it.
 */

#define LDAP_SERVER_MAX 32
#define LDAP_SERVER_NAME_MAX 64 /* matches REGISTRY_NAME_MAX */
#define LDAP_SERVER_PATH_MAX 256

struct ldap_server_binding {
	char container_name[LDAP_SERVER_NAME_MAX];
	char db_path[LDAP_SERVER_PATH_MAX]; /* absolute, as the container itself sees it */
};

enum ldap_server_error {
	LDAP_SERVER_OK = 0,
	LDAP_SERVER_ERR_INVALID_PATH,
	LDAP_SERVER_ERR_DUPLICATE,
	LDAP_SERVER_ERR_FULL,
	LDAP_SERVER_ERR_PERSIST_FAILED,
	LDAP_SERVER_ERR_NOT_FOUND
};

/* Loads persisted bindings (if any) at startup. */
int ldap_init(const char *state_path);

/*
 * Registers container_name as the LDAP-serving target, with db_path
 * (its own view of the absolute path to glauth's SQLite database
 * file) recorded for task #726's CRUD handlers to resolve against.
 * db_path is rejected (LDAP_SERVER_ERR_INVALID_PATH) unless it's an
 * absolute path containing no "..", mirroring dns_server_register()'s
 * own hosts_path_is_valid() rule exactly.
 * LDAP_SERVER_ERR_DUPLICATE if container_name is already registered --
 * unregister first to change its db_path.
 */
enum ldap_server_error ldap_server_register(const char *container_name, const char *db_path);
enum ldap_server_error ldap_server_unregister(const char *container_name);

/* Resolves container_name's own currently-registered db_path, or NULL
 * if it isn't registered -- the lookup task #726's CRUD handlers use. */
const struct ldap_server_binding *ldap_server_find(const char *container_name);

/* Called when a container is removed, so a stale binding never
 * lingers referencing a name that no longer exists -- same call site
 * shape as dns_server_forget(). */
void ldap_server_forget(const char *container_name);

void ldap_server_write_json_one(const struct ldap_server_binding *binding, struct json_writer *w);
void ldap_server_write_json_list(struct json_writer *w);

#endif /* LDAP_SERVER_H */
