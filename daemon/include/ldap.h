#ifndef LDAP_SERVER_H
#define LDAP_SERVER_H

#include "json.h"
#include "pwhash.h"

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
 * on its own config file whenever `watchconfig = true` is set in
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

/* ADR-0141 Phase 2: repoints without reloading -- see network_repoint()'s
 * own doc comment for the shared reasoning. */
void ldap_repoint(const char *new_state_path);

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

/* ADR/issue #66: copies up to max registered LDAP-server container
 * names into out[] (any order), returns the count -- lets main.c
 * derive a client URI list from the servers an operator already
 * registered, when no explicit client_uri is set. */
int ldap_server_list_containers(char out[][LDAP_SERVER_NAME_MAX], int max);

/* Called when a container is removed, so a stale binding never
 * lingers referencing a name that no longer exists -- same call site
 * shape as dns_server_forget(). */
void ldap_server_forget(const char *container_name);

void ldap_server_write_json_one(const struct ldap_server_binding *binding, struct json_writer *w);
void ldap_server_write_json_list(struct json_writer *w);

/*
 * LDAP user/group CRUD (task #726). Design direction, confirmed
 * directly with the user: the SAME model DNS already uses --
 * dns_record_create()/delete()/find() (daemon/include/dns.h). Cix
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
 * that point (listen address, watchconfig, TLS paths) is preserved
 * byte-for-byte. If neither marker is present yet (a fresh base config
 * with no managed section), the fresh content is simply appended at
 * EOF. baseDN is the one field of that prefix this module DOES also
 * own, as of ADR-0148: rewrite_basedn() rewrites just that one quoted
 * value (if present in the recognized "baseDN = "..."" form) to match
 * hostauth_ldap_base_dn(), the real, canonical source, before the
 * prefix is otherwise preserved untouched -- closing what used to be a
 * fourth independently-typed copy of the same value.
 *
 * Password storage: passbcrypt (a real, documented glauth "config"
 * datastore field) via a real, vendored, audited bcrypt implementation
 * (daemon/src/vendor/bcrypt.c + blowfish.c, daemon/include/pwhash.h)
 * -- ADR-0144. Originally this field held a bare unsalted SHA256 hex
 * digest instead (passsha256, via cixd's own already-linked OpenSSL
 * libcrypto), a deliberate trade-off at the time specifically to avoid
 * hand-rolling bcrypt from scratch (a "No Hacks" violation
 * reimplementing a security-critical primitive with no audit trail).
 * That reasoning held right up until these same credentials became the
 * *host's own* authentication root of trust (ADR-0144's host-auth
 * work) rather than only a container-facing directory password --
 * vendoring a real, widely-deployed reference implementation verbatim
 * resolves the original objection without reversing it: still no
 * hand-rolled crypto, just no longer avoiding bcrypt altogether.
 * Capability/ACL grants (glauth's own `capabilities` config stanza)
 * remain a separate, narrower mechanism (can_search below) from the
 * secondary-group membership this same ADR adds.
 */

#define LDAP_USER_MAX 256
#define LDAP_GROUP_MAX 64
#define LDAP_USER_NAME_MAX 32     /* POSIX-ish username length */
#define LDAP_GROUP_NAME_MAX 32
#define LDAP_USER_FIELD_MAX 128   /* givenname/sn/mail/loginshell/homedirectory */
#define LDAP_OWNER_NAME_MAX 64    /* matches REGISTRY_NAME_MAX, no header dependency -- same convention as DNS_OWNER_NAME_MAX */
#define LDAP_SSH_KEY_MAX 1024     /* comfortably fits any real ed25519/ecdsa/rsa-4096 public key line */
/* ADR-0144: real secondary/supplementary group membership, alongside
 * primarygroup below -- matches real POSIX semantics (id -Gn: primary
 * group plus a real supplementary list), needed for a proper,
 * configurable group-membership authorization model (host-auth's own
 * admin-group mapping checks this, not just primarygroup). 16 mirrors
 * every other project "small membership list" cap (e.g.
 * CLI_MAX_DEPENDS) -- generous for a real operator's own group
 * assignments, bounded so a single user record can't grow unboundedly. */
#define LDAP_USER_MAX_SECONDARY_GROUPS 16

struct ldap_user {
	char name[LDAP_USER_NAME_MAX];
	int uidnumber;
	int primarygroup; /* gidnumber of an existing ldap_group */
	int secondary_groups[LDAP_USER_MAX_SECONDARY_GROUPS]; /* gidnumbers; ADR-0144 */
	int secondary_group_count;
	char givenname[LDAP_USER_FIELD_MAX];
	char sn[LDAP_USER_FIELD_MAX];
	char mail[LDAP_USER_FIELD_MAX];
	char loginshell[LDAP_USER_FIELD_MAX];
	char homedirectory[LDAP_USER_FIELD_MAX];
	char passbcrypt[PWHASH_BCRYPT_LEN + 1]; /* empty: no password set yet */
	int disabled;
	/* Task #727 (container-creation auto-provisioning) only, from here down --
	 * never settable via the public user-CRUD REST body (POST/PUT .../users). */
	char owner_container[LDAP_OWNER_NAME_MAX]; /* empty: not auto-provisioned; else the
	                                             * container that owns this account --
	                                             * auto-removed via ldap_user_forget_owner()
	                                             * on container delete, same as
	                                             * dns_record_forget_owner(). */
	int can_search; /* grants a minimal glauth "search" capability (object "*"),
	                  * rendered as a [[users.capabilities]] sub-stanza -- without
	                  * it a provisioned service account can bind but not search,
	                  * useless for looking up another user's DN. Real, scoped
	                  * capability support; the general ACL/grant design for
	                  * manually-created users is still deferred to task #728. */
	char ssh_public_key[LDAP_SSH_KEY_MAX]; /* task #731 (ADR-0144 task #838):
	                  * a single OpenSSH public key line ("ssh-ed25519 AAAA...
	                  * comment"), settable via the normal user CRUD REST body --
	                  * rendered as glauth's own real sshkeys = [...] LDAP
	                  * attribute (render_users_groups_toml()), queried live by a
	                  * container's own AuthorizedKeysCommand. Empty means no key
	                  * for this user. One key per user in v1, the same "start
	                  * minimal" scope every other single-value field here
	                  * already has. */
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
	/* #419: server_tls asked for while a registered, running server has
	 * no delivered certificate. Distinct from INVALID_FIELD because the
	 * field is fine and the fleet is not, and the operator's fix is to
	 * deliver a cert to a named container. */
	LDAP_RECORD_ERR_NO_SERVER_CERT,
	LDAP_RECORD_ERR_PERSIST_FAILED
};

/* Loads persisted users/groups (if any). Called once at startup,
 * alongside ldap_init() -- see main.c. */
int ldap_record_init(const char *users_state_path, const char *groups_state_path);

void ldap_record_repoint(const char *new_users_state_path, const char *new_groups_state_path);

int ldap_username_is_valid(const char *name);
int ldap_groupname_is_valid(const char *name);

struct ldap_group *ldap_group_find(const char *name);
struct ldap_group *ldap_group_find_by_gid(int gidnumber);
enum ldap_record_error ldap_group_create(const char *name, int gidnumber, struct ldap_group **out);
/* Full field replacement (task #750), mirroring ldap_user_update()'s
 * own shape -- name is authoritative from the URL path, gidnumber is
 * the only other field a group has. Rejects a gidnumber collision
 * with a DIFFERENT existing group (LDAP_RECORD_ERR_DUPLICATE); does
 * NOT cascade-update any user whose primarygroup referenced the old
 * gidnumber -- same "no cascading validation" posture this module
 * already has everywhere else.
 *
 * new_name (ADR-0147): NULL or equal to name means no rename, the
 * pre-existing behavior. A real, different new_name renames the
 * group in place -- validated the same way ldap_group_create() would
 * (ldap_groupname_is_valid(), no collision with a different existing
 * group). Unlike gidnumber, renaming a group DOES have one real,
 * deliberate cascade: if the group's OLD name is currently a member
 * of hostauth-config's own admin_groups list, it's rewritten to the
 * new name there too (hostauth_rename_admin_group()) before this
 * function's own rename is considered committed -- a renamed admin
 * group must never silently fall out of write-gating, the exact
 * class of incident ADR-0146 already closed once for a different
 * cause. Every user's own primarygroup (a gidnumber, not a name) is
 * unaffected by a group rename; only the *rendered LDAP DN* of its
 * members changes (cn=<user>,ou=<new-name>,<base-dn>), which any
 * external config referencing the old DN (nslcd.conf's own binddn,
 * for instance) does not learn about automatically -- documented,
 * not silently handled, matching this module's existing posture on
 * cross-references it cannot see. */
enum ldap_record_error ldap_group_update(const char *name, const char *new_name, int gidnumber,
                                          struct ldap_group **out);
enum ldap_record_error ldap_group_delete(const char *name);
void ldap_group_write_json_one(const struct ldap_group *g, struct json_writer *w);
void ldap_group_write_json_list(struct json_writer *w);

struct ldap_user *ldap_user_find(const char *name);

/* ADR-0179: 1 if any managed user's uidnumber falls in [lo, hi] --
 * the allocator's active collision check before committing a
 * subordinate range. */
int ldap_user_uidnumber_in_range(long long lo, long long hi);
/* password == NULL: leave passbcrypt unset (create) or unchanged
 * (update). password == "" is treated the same as NULL -- an empty
 * credential is never written. owner_container: task #727's own
 * field (see struct ldap_user's own comment) -- every caller outside
 * the auto-provisioning hook in main.c passes NULL. can_search
 * (task #727, exposed on the real public API by ADR-0144 task #838):
 * a real, durable bind/service account (nslcd's own binddn, a live
 * AuthorizedKeysCommand's own search bind) needs this grant with no
 * other legitimate way to get it, so unlike owner_container it is a
 * real, settable field on both create and update, not internal-only.
 * secondary_groups/secondary_group_count (ADR-0144): NULL/0 means no
 * secondary groups; each gidnumber must already name a real group
 * (LDAP_RECORD_ERR_GROUP_NOT_FOUND otherwise, same validation
 * primarygroup already gets). */
enum ldap_record_error ldap_user_create(const char *name, int uidnumber, int primarygroup,
                                         const int *secondary_groups, int secondary_group_count,
                                         const char *givenname, const char *sn, const char *mail,
                                         const char *loginshell, const char *homedirectory,
                                         const char *password, int disabled,
                                         const char *owner_container, int can_search,
                                         const char *ssh_public_key, struct ldap_user **out);
/* new_name (ADR-0147): NULL or equal to name means no rename, the
 * pre-existing behavior. A real, different new_name renames the user
 * in place (ldap_username_is_valid(), no collision with a different
 * existing user) -- no cross-reference cascade needed on the user
 * side (nothing in this codebase indexes another record by a user's
 * name the way admin_groups indexes hostauth-config by group name;
 * secondary/primary group membership is already by gidnumber, not
 * name). External config referencing the user's old rendered DN
 * (e.g. a nslcd.conf binddn built from this exact user) is, same as
 * for a group rename, not automatically updated -- documented, not
 * silently handled. */
enum ldap_record_error ldap_user_update(const char *name, const char *new_name, int uidnumber,
                                         int primarygroup, const int *secondary_groups,
                                         int secondary_group_count, const char *givenname,
                                         const char *sn, const char *mail, const char *loginshell,
                                         const char *homedirectory, const char *password, int disabled,
                                         const char *ssh_public_key, int can_search,
                                         struct ldap_user **out);
enum ldap_record_error ldap_user_delete(const char *name);
void ldap_user_write_json_one(const struct ldap_user *u, struct json_writer *w);
void ldap_user_write_json_list(struct json_writer *w);

/* ADR-0144: real primary-or-secondary group membership check, by
 * name -- the one place "is this user in that group" is decided,
 * shared by the local host-auth backend (daemon/src/hostauth.c) and
 * anything else that ever needs the same POSIX-shaped answer (primary
 * group counts as membership too, matching real `id -Gn` semantics).
 * Returns 1 if user_name is disabled==0 and a member (primary or
 * secondary) of group_name, 0 otherwise (including "no such user" or
 * "no such group" -- never distinguished from "not a member" to the
 * caller, same fail-closed posture every other auth check here has). */
int ldap_user_is_in_group(const char *user_name, const char *group_name);

/* Local, in-process credential check against this daemon's own
 * already-persisted user record -- real bcrypt verification
 * (pwhash_bcrypt_check()), no network call. Returns 1 if name exists,
 * is not disabled, and password matches; 0 otherwise (again, never
 * distinguishing "no such user" from "wrong password"). */
int ldap_user_check_password(const char *user_name, const char *password);

/*
 * Calls fn(name, ctx) for every currently-defined user's own name, in
 * table order, stopping early the first time fn returns nonzero --
 * the one real "walk every user" primitive this module exposes,
 * reused by daemon/src/hostauth.c's own bootstrap-safety check rather
 * than a second enumeration loop invented there. Returns whatever the
 * short-circuiting call returned (1 if some fn call returned nonzero,
 * 0 if every user was visited without one).
 */
int ldap_user_for_each(int (*fn)(const char *name, void *ctx), void *ctx);

/* Best-effort cleanup on container deletion, mirroring dns_record_
 * forget_owner()/pki_cert_forget_owner() exactly: deletes name's
 * account iff it exists and its owner_container is container_name
 * itself. Safe no-op for every container that was never auto-
 * provisioned, so this is called unconditionally from the
 * container-delete handler. */
void ldap_user_forget_owner(const char *container_name);

/*
 * Configurable uid/gid allocation start points (task #748, user-
 * requested): ldap_uid_alloc()/ldap_gid_alloc() below used to hardcode
 * 10000 as the search floor with no way to change it. Persisted
 * separately from the user/group records themselves (a single small
 * settings object, not a record collection) -- same "one small
 * singleton config file" shape resolv.c's own resolv_init()/
 * resolv_set() already establishes for GET/PUT /v1/system/resolv,
 * rather than folding it into ldap_server_binding or struct ldap_user.
 * Changing start_uid/start_gid only affects *future* allocations --
 * it never renumbers an already-existing user or group.
 */
#define LDAP_CONFIG_DEFAULT_START_UID 10000
#define LDAP_CONFIG_DEFAULT_START_GID 10000
/* glauth's own sample config default for its [ldaps] listener (#414). */
#define LDAP_CONFIG_DEFAULT_TLS_PORT 636

struct ldap_config {
	int start_uid;
	int start_gid;
	/*
	 * Issue #66: the LDAP *client* login settings every container that
	 * authenticates against this platform's LDAP needs -- stored once
	 * here instead of hand-copied (URI list, base DN, bind DN, and a
	 * real bind credential) into every such container's recipe. A
	 * container recipe references them as {{LDAP:URI}} /
	 * {{LDAP:BASE_DN}} / {{LDAP:BIND_DN}} / {{LDAP:BIND_PASSWORD}}
	 * tokens, resolved at recipe render time (container_recipe_
	 * render()) exactly like {{SECRET:KEY}} -- so recipes carry no
	 * values at all and an LDAP server move is one config PUT, not a
	 * recipe hunt. bind_password is stored in this module's root-only
	 * state file (the same posture PKI private keys already have) and
	 * NEVER serialized back out -- GET reports only bind_password_set.
	 * client_uri is the full, explicit URI list (e.g. "ldap://a:3893/
	 * ldap://b:3893/") rather than something derived from the
	 * registered-server bindings, which store no address/port --
	 * auto-derivation is a tracked refinement on #66, not silently
	 * half-done here. Empty strings mean unset.
	 */
	char client_uri[512];
	char base_dn[256];
	char bind_dn[256];
	char bind_password[256];
	/*
	 * Issue #414: whether clients reach this platform's LDAP over TLS.
	 * One answer for the whole client population rather than a
	 * per-server one, because these settings exist to be handed to
	 * every client identically -- a mixed fleet where some containers
	 * use TLS and some do not is not a configuration, it is a
	 * migration, and it is expressed by moving this flag once.
	 *
	 * Governs only the clients configured FROM here (nslcd.conf,
	 * {{LDAP:URI}}). The daemon's own bind has its own switch,
	 * hostauth's ldap_tls (#416).
	 *
	 * There is no client_tls_port. #419 removed it: there is one port
	 * per listener, and the port clients are given IS the server's own
	 * (see ldap_client_port()). It existed only because the server's
	 * listener config was literal TOML in a recipe and therefore
	 * unknowable from here -- which also meant nothing checked that it
	 * named a port the server was listening on.
	 */
	int client_tls;
	/*
	 * #419: which listeners each registered glauth actually serves,
	 * rendered into its own config file as [ldap]/[ldaps] enabled and
	 * listen. The daemon owns these on the terms ADR-0148 set for
	 * baseDN: the operator's file is the initial value, the API is the
	 * source of truth from then on.
	 *
	 * listeners_managed is the whole reason this is safe to deploy onto
	 * a running install. False until an operator PUTs one of the
	 * server_* fields, and while false the render touches neither
	 * section -- exactly the skip ldap_write_config_file() already
	 * performs for baseDN when ldap_base_dn has never been set, and for
	 * the identical reason: the first boot after upgrade would
	 * otherwise rewrite two live, working listeners from defaults
	 * derived from nothing. The ports still hold real defaults while
	 * unmanaged, because ldap_client_port() reads them.
	 */
	int listeners_managed;
	int server_plaintext;
	int server_plaintext_port;
	int server_tls;
	int server_tls_port;
};

/* Loads persisted start_uid/start_gid (if any) at startup, alongside
 * ldap_record_init(). A missing file means the compiled-in defaults
 * above -- not an error. */
int ldap_config_init(const char *state_path);

void ldap_config_repoint(const char *new_state_path);

/* Never NULL -- returns the compiled-in defaults if ldap_config_set()
 * has never been called. */
const struct ldap_config *ldap_config_get(void);

/* Both values must be > 0 (LDAP_RECORD_ERR_INVALID_FIELD otherwise).
 * Persists immediately; takes effect on the very next ldap_uid_alloc()/
 * ldap_gid_alloc() call. */
enum ldap_record_error ldap_config_set(int start_uid, int start_gid);

/*
 * Issue #66: update the client-login fields (any NULL argument leaves
 * that field unchanged; an empty string clears it). Persists alongside
 * start_uid/start_gid in the same state file.
 */
/*
 * #419: the listener configuration plus which of them clients are
 * pointed at, set together because they are one decision and the
 * combinations that must be refused span both. -1 leaves a field
 * unchanged; any non-(-1) server_* argument marks the configuration
 * managed from then on.
 *
 * LDAP_RECORD_ERR_INVALID_FIELD for: a port outside 1..65535; the two
 * ports equal; both listeners disabled; or client_tls naming a
 * disabled listener. LDAP_RECORD_ERR_NO_SERVER_CERT when server_tls is
 * being turned on and out_container names a registered, running server
 * with no delivered certificate -- glauth exits on reload if told to
 * serve TLS without one, so the whole change is refused rather than
 * taking down a working server. out_container may be NULL.
 */
enum ldap_record_error ldap_config_set_listeners(int client_tls, int server_plaintext,
                                                  int server_plaintext_port, int server_tls,
                                                  int server_tls_port, char *out_container,
                                                  size_t out_container_size);

enum ldap_record_error ldap_config_set_client(const char *client_uri, const char *base_dn,
                                               const char *bind_dn, const char *bind_password);

void ldap_config_write_json(struct json_writer *w);
/* Same, but leaves the object open for the caller to append to and
 * close -- see the definition's own comment (issue #84). */
void ldap_config_write_json_open(struct json_writer *w);

/* Returns the lowest unused uidnumber >= ldap_config_get()->start_uid
 * (a service-account range, distinct from the human-numbered 5000s an
 * operator would typically pick by hand) -- used by the container-
 * creation LDAP auto-provisioning hook (task #727), and by POST
 * /v1/ldap/users when the caller omits uidnumber entirely (task #748). */
int ldap_uid_alloc(void);

/* The ldap_uid_alloc() analog for gidnumber, seeded from
 * ldap_config_get()->start_gid -- used by POST /v1/ldap/groups when
 * the caller omits gidnumber entirely (task #748). */
int ldap_gid_alloc(void);

#define LDAP_PROVISION_SECRET_LEN 32 /* hex-encoded random bytes */

/* Fills out with a fresh, random provisioning secret (32 lowercase
 * hex chars, from /dev/urandom) for task #727's auto-provisioning
 * hook. Never persisted in plaintext anywhere -- the caller hashes it
 * via ldap_user_create()'s own password param and delivers the
 * plaintext directly into the freshly-created container's filesystem
 * in the same request, the same "generate once, deliver once, only
 * the hash survives" shape pki_cert_deliver() already established for
 * TLS keys. Returns 0 on success, -1 if /dev/urandom couldn't be read
 * (never expected on a real Linux kernel, but checked rather than
 * trusted). */
int ldap_generate_secret(char out[LDAP_PROVISION_SECRET_LEN + 1]);

/* Full re-population of every currently-registered, currently-running
 * LDAP server's own config file from Cix's own record store -- the
 * dns_server_sync_all() analog, called after every user/group
 * mutation AND right after a server registers (see handle_ldap_
 * server_create()) so a fresh/replacement glauth instance starts
 * current instead of empty.
 */
void ldap_record_sync_all(void);

/*
 * Auto-provisions (issue #80) the well-known service/bind account
 * ("svc-nslcd") that LDAP *client* containers authenticate their
 * directory searches with -- so its DN and password are never typed by
 * hand into /v1/ldap/config (the drift that left jump login pointing at
 * an account glauth never held). Idempotent and cheap: a no-op once the
 * account exists with a password on file, and a no-op if the operator
 * has deliberately configured a different (non-svc-nslcd) bind_dn.
 * Otherwise it ensures a service group + account (a fresh random secret,
 * can_search granted) and records the account's real DN + secret as the
 * client bind_dn/bind_password. base_dn always comes from the canonical
 * hostauth-config value (see ldap_config_get()). A no-op until LDAP is
 * configured (a base DN exists). Call at each point a client LDAP config
 * is about to be handed out (the ldap_login staging path) -- by then the
 * registered glauth servers are up, so the create pushes to them live.
 */
void ldap_ensure_service_bind_account(void);

/* Issue #83: managed-record counts, for detecting users/groups that have
 * no registered server to be served by. */
int ldap_user_count(void);
int ldap_group_count(void);

/*
 * The client URI every container should authenticate against: the
 * explicitly configured list when there is one (filtered, not passed
 * through -- issue #84), otherwise the registered server containers'
 * own addresses, in-service ones first.
 *
 * Here rather than in main.c because it is ldap policy that happens to
 * need the registry to turn container names into addresses, and this
 * module already depends on the registry for exactly that. Returns 1
 * when out holds a usable URI, 0 when there is nothing to point at.
 */
/* The port the LDAP service is reachable on right now: the TLS
 * listener's own port when client_tls is on, the plaintext listener's
 * otherwise (#419 -- it used to read client_tls_port and
 * HOSTAUTH_LDAP_DEFAULT_PORT, neither of which was the server's actual
 * port). Both the client URI above and cixd's own server-health probe
 * read it, so the probe can never test a port the clients were not
 * given (#416). */
int ldap_client_port(void);

/* #419: applies the managed [ldap]/[ldaps] values to a glauth config's
 * text, for staging it into a container BEFORE clone3() -- glauth binds
 * its listeners at startup and its config watcher does not, so a value
 * written into a live config is never adopted. Returns 1 with
 * *out_buf/*out_len set (caller frees), 0 when there is nothing to do
 * (listeners not managed yet, or empty content), -1 on allocation
 * failure. */
int ldap_render_listeners(const char *content, size_t content_len, char **out_buf,
                          size_t *out_len);

int ldap_effective_client_uri(char *out, size_t out_size);

#endif /* LDAP_SERVER_H */
