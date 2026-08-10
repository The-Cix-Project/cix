# 0111 — Jump box SSH auth backed by LDAP: rendered Unix accounts, not real NSS/PAM (task #731)

## Status

Accepted

## Context

Task #730 built the jump box image and stood up a working container with SSH pubkey login, but only via a single, manually-provisioned local account (`op`) baked in at container-creation time. The follow-on ask (task #731) is to wire that SSH auth to LDAP — an LDAP user's own key should work, and adding/removing/disabling an LDAP user should add/remove/disable their SSH access without touching the container by hand again.

The obvious approach — a real LDAP-protocol client on the jump box, via glibc's NSS (`libnss_ldap`) and PAM (`pam_ldap`) — was considered and rejected directly against this project's own constraints, not assumed:

- Neither `libnss_ldap` nor `pam_ldap` has a recipe in this project. Building either from source would be a real, separate undertaking (both are old, glibc-NSS-ABI-sensitive codebases) with no other use case pulling for it.
- `openssh.recipe` (task #729) was deliberately built **without** PAM — confirmed directly via `sshd -d` reporting `Unsupported option UsePAM` when a config tries to enable it. Wiring PAM in now would mean revisiting and rebuilding openssh's own recipe, a second, unrelated scope expansion.
- Even with both built, a live NSS/PAM-over-LDAP handshake against glauth (an LDAP interface bolted onto a SQLite-backed Go server, not a battle-tested directory server) would need its own real integration testing — exactly the kind of thing task #747 (glauth's TCP listener not accepting connections on real hardware) shows isn't yet a fully solid foundation to build on.

Kanxeo already has a proven, working pattern for exactly this shape of problem, used twice: **Kanxeo owns the durable record, and renders into the consumer's own filesystem/format** on every change — not the consumer querying Kanxeo or a directory protocol live.

- DNS: `dns_write_hosts_file()`/`dns_server_sync_all()` renders every current DNS record into a registered dnsmasq container's own hosts file.
- LDAP-for-glauth (task #726): `ldap_write_config_file()`/`ldap_record_sync_all()` renders every current user/group into a registered glauth container's own TOML config file.

Extending this same pattern a third time — Kanxeo renders real `/etc/passwd`/`/etc/group`/`/etc/shadow` entries plus a dedicated `~/.ssh/authorized_keys` per user directly into a registered SSH target's own filesystem — solves the concrete capability actually requested (an LDAP user's key logs into the jump box) without any of the above undertaking. `sshd` on the target never talks to LDAP or `kanxeod` at all; it just reads ordinary, locally-resolvable account files that happen to be kept in sync.

## Decision

### A new `ldap_ssh_target` registration mechanism

`daemon/include/ldap.h`/`daemon/src/ldap.c` gain a self-contained sibling to the existing `ldap_server_*`/`ntp_server_*` registration mechanisms: `ldap_ssh_target_register()`/`unregister()`/`forget()`, `POST`/`GET`/`DELETE /v1/ldap/ssh-targets`. Validation is self-contained (`registry_find()` directly, not a `main.c` pre-check) — a container must already exist and be `running`, mirroring `ntp_server_register()`'s own shape (no pid/pidfd bookkeeping needed, same reasoning NTP registration already established: nothing to signal). Registering a target syncs it immediately, so a fresh registration starts current rather than empty — the same behavior `ldap_server_register()` already has.

### `ssh_public_key` on `struct ldap_user`

A new field, settable via the existing `POST`/`PUT /v1/ldap/users` CRUD (no new user-management surface). Empty means "not eligible for SSH target sync" — the common case for service/bind accounts (task #727's auto-provisioning hook explicitly passes `NULL` for these). A user also needs `disabled: false` to be synced; a disabled-but-keyed user is skipped, matching the existing meaning of `disabled` elsewhere in this record store.

### Rendering: `write_accounts_for_pid()`

For every registered, running target, every eligible user gets:

- One `passwd` line: `name:x:uidnumber:primarygroup:givenname-or-name:homedirectory-or-/:loginshell-or-/usr/bin/bash`.
- One `shadow` line, using **`*`** for the password field, never `!`. This is a real, previously-discovered gotcha (task #730): OpenSSH treats `!` as "account fully locked", which blocks *every* auth method including public-key — `*` means "no valid password", which is exactly what's wanted (pubkey-only), and is the same convention the jump box's manually-provisioned `op` account already used.
- One `group` line per **distinct** `gidnumber` actually referenced (deduplicated — multiple users can share a `primarygroup`), not one per user.
- A dedicated `~/.ssh/authorized_keys` (mode `0600`, owned by the user's own uid/gid, parent `.ssh` mode `0700`) containing exactly the user's own `ssh_public_key` line.

All three account files are written via a new `write_managed_tail()` helper: a plain marker line (`# --- Kanxeo-managed accounts below (task #731) -- do not edit by hand ---`) delimits a "managed tail" that gets fully replaced on every sync, while everything above the marker — root/sshd's own pre-provisioned entries — is preserved byte-for-byte. This is the same conceptual approach `ldap_write_config_file()` already uses for glauth's TOML config (a structural boundary it can truncate at), adapted for `/etc/passwd`-family files that have no such native syntax boundary of their own.

`ldap_ssh_sync_all()` hooks into the existing `ldap_record_sync_all()` — called from every existing user/group `POST`/`PUT`/`DELETE` handler already — so no new call sites are needed anywhere else; every LDAP mutation that already re-syncs glauth also re-syncs every registered SSH target, for free.

### A general fix found and fixed along the way: `libnss_files.so.2`

Rendering correct `/etc/passwd` content is necessary but not sufficient — task #730 already discovered that a minimal image's shared-library closure (built by following `ldd`'s ELF `NEEDED` graph) never includes `libnss_files.so.2`, because glibc's NSS "files" backend is a `dlopen()`ed plugin, not a linked dependency of anything. Without it, `getpwnam()`/`getgrnam()` silently return empty even against a perfectly well-formed `/etc/passwd`, so SSH login still fails, just with no useful signal pointing back at the real cause.

This was previously worked around ad hoc for the jump box image specifically; it's now fixed generally in `pkg_seed_image_baseline()` (the function every newly-created image already gets baseline runtime files from): `libnss_files.so.2` is staged unconditionally into every image's shared-lib closure, and a default minimal `/etc/nsswitch.conf` (`passwd/group/shadow/hosts: files`) is written if the image doesn't already have one — both idempotent, following the exact same pattern the function's existing `runtime_libs[]` table and CA-trust-bundle staging block already use.

## Consequences

- No new build dependency, no new recipe, no PAM rebuild, no live NSS/PAM-over-LDAP integration to test — the entire mechanism is host-side C code writing plain files, reusing infrastructure (`persist_atomic_write()`, `persist_mkdir_p()`, the managed-tail truncation idea) this project already has.
- `sshd` on a registered target has zero LDAP awareness. This is a deliberate, stated trade-off, not an oversight: it means Kanxeo's own sync cadence (immediate on every mutation) is the actual latency between "an LDAP user's key is added" and "that key works on the target" — no caching, no TTL, but also no live query-time dependency on glauth being reachable at login time.
- The `libnss_files.so.2`/`nsswitch.conf` fix benefits every future image, not just SSH targets — any image that ever needs real `getpwnam()`/`getgrnam()` resolution (this feature is the first, but not necessarily the last) now gets it automatically at creation time.
- Deleting a container automatically forgets its SSH-target registration (`ldap_ssh_target_forget()`, wired into the same container-delete cleanup path as `ldap_server_forget()`/`dns_server_forget()`) — but does **not** touch the container's own filesystem, since there is nothing more to clean up once the container itself is gone.
- Unregistering a still-running target also does not roll back previously-rendered accounts — the managed tail is simply never touched again. Re-registering later re-syncs from current state, so this is consistent with every other "registration is bookkeeping, not a filesystem mutation on its own" precedent in this codebase.
