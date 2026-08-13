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
 * that point (baseDN, listen address, watchconfig, TLS paths) is
 * preserved byte-for-byte. If neither marker is present yet (a fresh
 * base config with no managed section), the fresh content is simply
 * appended at EOF.
 *
 * Password storage: passbcrypt (a real, documented glauth "config"
 * datastore field) via a real, vendored, audited bcrypt implementation
 * (daemon/src/vendor/bcrypt.c + blowfish.c, daemon/include/pwhash.h)
 * -- ADR-0144. Originally this field held a bare unsalted SHA256 hex
 * digest instead (passsha256, via kanxeod's own already-linked OpenSSL
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
	char ssh_public_key[LDAP_SSH_KEY_MAX]; /* task #731: a single OpenSSH public
	                  * key line ("ssh-ed25519 AAAA... comment"), settable via the
	                  * normal user CRUD REST body -- empty means this user gets
	                  * no account/key rendered onto any registered SSH target
	                  * (ldap_ssh_target_*, below). One key per user in v1, the
	                  * same "start minimal" scope every other single-value field
	                  * here already has. */
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
 * already has everywhere else. */
enum ldap_record_error ldap_group_update(const char *name, int gidnumber, struct ldap_group **out);
enum ldap_record_error ldap_group_delete(const char *name);
void ldap_group_write_json_one(const struct ldap_group *g, struct json_writer *w);
void ldap_group_write_json_list(struct json_writer *w);

struct ldap_user *ldap_user_find(const char *name);
/* password == NULL: leave passbcrypt unset (create) or unchanged
 * (update). password == "" is treated the same as NULL -- an empty
 * credential is never written. owner_container/can_search: task
 * #727's own fields (see struct ldap_user's own comment) -- every
 * caller outside the auto-provisioning hook in main.c passes NULL/0.
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
enum ldap_record_error ldap_user_update(const char *name, int uidnumber, int primarygroup,
                                         const int *secondary_groups, int secondary_group_count,
                                         const char *givenname, const char *sn, const char *mail,
                                         const char *loginshell, const char *homedirectory,
                                         const char *password, int disabled,
                                         const char *ssh_public_key, struct ldap_user **out);
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

struct ldap_config {
	int start_uid;
	int start_gid;
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

void ldap_config_write_json(struct json_writer *w);

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
 * LDAP server's own config file from Kanxeo's own record store -- the
 * dns_server_sync_all() analog, called after every user/group
 * mutation AND right after a server registers (see handle_ldap_
 * server_create()) so a fresh/replacement glauth instance starts
 * current instead of empty. Also re-syncs every registered SSH target
 * (ldap_ssh_sync_all(), below) -- both are "push current LDAP state
 * into a registered consumer's own filesystem" and share this one
 * trigger point, matching every other user/group mutation call site
 * already funneling through here rather than each caller remembering
 * two separate sync calls.
 */
void ldap_record_sync_all(void);

/*
 * SSH-backed login for LDAP users (task #731): registers a running
 * container as a target that should receive a real, locally-resolvable
 * Unix account (uid/gid/home/shell) plus an authorized_keys entry for
 * every current LDAP user that has a non-empty ssh_public_key -- NOT
 * an LDAP-protocol integration (no libnss_ldap/pam_ldap; this project
 * builds openssh without PAM at all, task #729's own recipe decision).
 * Same "Kanxeo owns the durable record, renders into the consumer's
 * own filesystem" posture DNS/LDAP-for-glauth already established --
 * sshd itself never talks to LDAP or to kanxeod, it just reads real
 * /etc/passwd,/etc/group,/etc/shadow,~/.ssh/authorized_keys entries
 * that happen to be kept in sync from the LDAP user store.
 *
 * A locked password (`*` in /etc/shadow, NOT `!` -- openssh treats a
 * leading `!` as "account locked", which blocks every auth method
 * including pubkey, confirmed directly via `sshd -d` during task #730)
 * is always written; only pubkey auth is ever possible for a synced
 * account. Deliberately does not attempt real NSS/LDAP integration
 * (libnss_ldap has no recipe in this project yet, and would need a
 * new from-source build + real testing this task's own scope doesn't
 * cover) -- this is a real, working v1, not a stop-gap for a design
 * that was never going to ship: it fully solves "log into a jump box
 * container with your LDAP-managed key," the concrete capability
 * requested.
 */
#define LDAP_SSH_TARGET_MAX 16
#define LDAP_SSH_TARGET_NAME_MAX 64 /* matches REGISTRY_NAME_MAX, no header dependency */

struct ldap_ssh_target {
	char container_name[LDAP_SSH_TARGET_NAME_MAX];
};

enum ldap_ssh_error {
	LDAP_SSH_OK = 0,
	LDAP_SSH_ERR_DUPLICATE,
	LDAP_SSH_ERR_FULL,
	LDAP_SSH_ERR_NOT_FOUND,
	LDAP_SSH_ERR_CONTAINER_NOT_FOUND,
	LDAP_SSH_ERR_CONTAINER_NOT_RUNNING,
	LDAP_SSH_ERR_PERSIST_FAILED
};

/* Loads persisted SSH targets (if any) at startup, alongside ldap_init(). */
int ldap_ssh_init(const char *state_path);

void ldap_ssh_repoint(const char *new_state_path);

/* container_name must already exist and be running (self-contained
 * check via registry_find(), same as ntp_server_register() -- not
 * relying on a main.c pre-check). Immediately syncs the target on
 * successful registration so a fresh/replacement container starts
 * current instead of empty, matching ldap_server_register()'s own
 * "sync right after registering" behavior for glauth. */
enum ldap_ssh_error ldap_ssh_target_register(const char *container_name);
enum ldap_ssh_error ldap_ssh_target_unregister(const char *container_name);

/* Called from the same container-delete cleanup path as dns_server_
 * forget()/ldap_server_forget()/ntp_server_forget() -- safe no-op if
 * container_name was never registered. */
void ldap_ssh_target_forget(const char *container_name);

void ldap_ssh_target_write_json_list(struct json_writer *w);

/* Re-renders Unix accounts + authorized_keys for every current LDAP
 * user with a non-empty ssh_public_key into every currently-
 * registered, currently-running SSH target -- called from ldap_
 * record_sync_all() (see above) and right after a target registers.
 * Best-effort per target: an unreachable container right now is
 * simply skipped and stays stale until it next registers or the next
 * user/group mutation retries every target again. */
void ldap_ssh_sync_all(void);

#endif /* LDAP_SERVER_H */
