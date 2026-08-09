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
 * config_path is the container's own view of the absolute path to
 * glauth's own config file (the one it was started with, -c <file>),
 * NOT a dedicated data-only file the way dnsmasq's --addn-hosts is --
 * glauth has no such thing (a single TOML file carries both its own
 * bootstrap settings -- backend/listen/TLS -- and, for the "config"
 * datastore this project uses, its [[users]]/[[groups]] data). See
 * ldap_write_config_file()'s own comment for how task #726's CRUD
 * layer safely rewrites only the managed tail of that same file.
 *
 * One real, deliberate difference from DNS, in the OPPOSITE direction
 * from what was first assumed here: dnsmasq only reads its hosts file
 * once at startup, so dns_server_register() must push a rendered file
 * AND send SIGHUP on every subsequent change. glauth, confirmed
 * directly against its own source (github.com/glauth/glauth v2.4.0,
 * v2/glauth.go's startConfigWatcher()), runs a real fsnotify watcher
 * on its own config file whenever `watch_config = true` is set in
 * that file, and reloads automatically on any write -- no signal
 * needed at all, simpler than DNS's own mechanism. Registration here
 * is still pure bookkeeping (no pid/pidfd parameter, matching the
 * original task #725 design): the container's pid is resolved fresh,
 * via /proc/<pid>/root/<config_path>, at every write time by task
 * #726's own CRUD layer -- the same "look up the registry entry
 * fresh, never cache a pid" discipline dns_server_sync_all() already
 * established.
 */

#define LDAP_SERVER_MAX 32
#define LDAP_SERVER_NAME_MAX 64 /* matches REGISTRY_NAME_MAX */
#define LDAP_SERVER_PATH_MAX 256

struct ldap_server_binding {
	char container_name[LDAP_SERVER_NAME_MAX];
	char config_path[LDAP_SERVER_PATH_MAX]; /* absolute, as the container itself sees it */
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
 * Registers container_name as the LDAP-serving target, with
 * config_path (its own view of the absolute path to glauth's own
 * config file) recorded for task #726's CRUD handlers to resolve
 * against. config_path is rejected (LDAP_SERVER_ERR_INVALID_PATH)
 * unless it's an absolute path containing no "..", mirroring dns_
 * server_register()'s own hosts_path_is_valid() rule exactly.
 * LDAP_SERVER_ERR_DUPLICATE if container_name is already registered --
 * unregister first to change its config_path.
 */
enum ldap_server_error ldap_server_register(const char *container_name, const char *config_path);
enum ldap_server_error ldap_server_unregister(const char *container_name);

/* Resolves container_name's own currently-registered config_path, or
 * NULL if it isn't registered -- the lookup task #726's CRUD handlers
 * use. */
const struct ldap_server_binding *ldap_server_find(const char *container_name);

/* Called when a container is removed, so a stale binding never
 * lingers referencing a name that no longer exists -- same call site
 * shape as dns_server_forget(). */
void ldap_server_forget(const char *container_name);

void ldap_server_write_json_one(const struct ldap_server_binding *binding, struct json_writer *w);
void ldap_server_write_json_list(struct json_writer *w);

/*
 * LDAP user/group CRUD (task #726). Design direction, confirmed
 * directly with the user: the SAME model DNS already uses --
 * dns_record_create()/delete()/find() (daemon/include/dns.h). Kanxeo
 * itself is the durable source of truth for every user/group
 * (persisted here, survives a glauth container being deleted or
 * rebuilt); glauth's own config file is a derived, rebuildable
 * render target, kept in sync the exact same way dns_write_hosts_
 * file()/dns_server_sync_all() keep dnsmasq's hosts file in sync:
 * every mutation re-renders the FULL current user/group set (not an
 * incremental patch -- matching dns_write_hosts_file()'s own always-
 * whole-file behavior) and writes it into every currently-registered,
 * currently-running server's own config file.
 *
 * The one real structural difference from the hosts-file case:
 * glauth's config file also carries its own bootstrap settings
 * (backend/listen/TLS), set up once by whoever creates the container
 * (the same --file= staging convention lldap.recipe already
 * established), which this module must never clobber. ldap_write_
 * config_file() below handles that by locating the first "\n[[users]]"
 * or "\n[[groups]]" byte offset in the CURRENT file content (a
 * syntactically unambiguous TOML array-of-tables boundary -- these
 * headers can only ever start a line at column 0) and truncating
 * there before appending the freshly-rendered tail; everything above
 * that point (baseDN, listen address, watch_config, TLS paths) is
 * preserved byte-for-byte. If neither marker is present yet (a fresh
 * base config with no managed section), the fresh content is simply
 * appended at EOF.
 *
 * Password storage: passsha256 (a real, documented glauth "config"
 * datastore field) via kanxeod's own already-linked OpenSSL libcrypto
 * SHA256 -- not passbcrypt, which would mean hand-rolling bcrypt (a
 * "No Hacks" violation reimplementing a security-critical primitive
 * this project has no audited implementation of) or shelling out. A
 * real, if comparatively weaker, credential mechanism -- documented
 * here so a future reader knows it's a deliberate, scoped trade-off,
 * not an oversight. Capability/ACL grants (glauth's own `capabilities`
 * config stanza) are explicitly out of scope for this task -- directory
 * data only; see task #727/#728 for how a real consuming workload's
 * bind/search rights get provisioned.
 */

#define LDAP_USER_MAX 256
#define LDAP_GROUP_MAX 64
#define LDAP_USER_NAME_MAX 32     /* POSIX-ish username length */
#define LDAP_GROUP_NAME_MAX 32
#define LDAP_USER_FIELD_MAX 128   /* givenname/sn/mail/loginshell/homedirectory */
#define LDAP_PASSSHA256_LEN 64    /* hex-encoded SHA-256, no null in the count */

struct ldap_user {
	char name[LDAP_USER_NAME_MAX];
	int uidnumber;
	int primarygroup; /* gidnumber of an existing ldap_group */
	char givenname[LDAP_USER_FIELD_MAX];
	char sn[LDAP_USER_FIELD_MAX];
	char mail[LDAP_USER_FIELD_MAX];
	char loginshell[LDAP_USER_FIELD_MAX];
	char homedirectory[LDAP_USER_FIELD_MAX];
	char passsha256[LDAP_PASSSHA256_LEN + 1]; /* empty: no password set yet */
	int disabled;
};

struct ldap_group {
	char name[LDAP_GROUP_NAME_MAX];
	int gidnumber;
};

enum ldap_record_error {
	LDAP_RECORD_OK = 0,
	LDAP_RECORD_ERR_INVALID_NAME,
	LDAP_RECORD_ERR_INVALID_FIELD,
	LDAP_RECORD_ERR_DUPLICATE,
	LDAP_RECORD_ERR_FULL,
	LDAP_RECORD_ERR_NOT_FOUND,
	LDAP_RECORD_ERR_GROUP_NOT_FOUND, /* primarygroup doesn't name a real group */
	LDAP_RECORD_ERR_PERSIST_FAILED
};

/* Loads persisted users/groups (if any). Called once at startup,
 * alongside ldap_init() -- see main.c. */
int ldap_record_init(const char *users_state_path, const char *groups_state_path);

int ldap_username_is_valid(const char *name);
int ldap_groupname_is_valid(const char *name);

struct ldap_group *ldap_group_find(const char *name);
struct ldap_group *ldap_group_find_by_gid(int gidnumber);
enum ldap_record_error ldap_group_create(const char *name, int gidnumber, struct ldap_group **out);
enum ldap_record_error ldap_group_delete(const char *name);
void ldap_group_write_json_one(const struct ldap_group *g, struct json_writer *w);
void ldap_group_write_json_list(struct json_writer *w);

struct ldap_user *ldap_user_find(const char *name);
/* password == NULL: leave passsha256 unset (create) or unchanged
 * (update). password == "" is treated the same as NULL -- an empty
 * credential is never written. */
enum ldap_record_error ldap_user_create(const char *name, int uidnumber, int primarygroup,
                                         const char *givenname, const char *sn, const char *mail,
                                         const char *loginshell, const char *homedirectory,
                                         const char *password, int disabled,
                                         struct ldap_user **out);
enum ldap_record_error ldap_user_update(const char *name, int uidnumber, int primarygroup,
                                         const char *givenname, const char *sn, const char *mail,
                                         const char *loginshell, const char *homedirectory,
                                         const char *password, int disabled,
                                         struct ldap_user **out);
enum ldap_record_error ldap_user_delete(const char *name);
void ldap_user_write_json_one(const struct ldap_user *u, struct json_writer *w);
void ldap_user_write_json_list(struct json_writer *w);

/* Full re-population of every currently-registered, currently-running
 * LDAP server's own config file from Kanxeo's own record store -- the
 * dns_server_sync_all() analog, called after every user/group
 * mutation AND right after a server registers (see handle_ldap_
 * server_create()) so a fresh/replacement glauth instance starts
 * current instead of empty. */
void ldap_record_sync_all(void);

#endif /* LDAP_SERVER_H */
