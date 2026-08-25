# 0109 — LDAP redesign: glauth, config-render write-through, and owner-tracked provisioning (tasks #723-727)

## Status

Accepted

## Context

This project's first LDAP deployment (Phase 21, ADR-0036) used `lldap` (Rust + a bundled WASM web frontend) as the LDAP-serving workload. That frontend is dead weight here: Cix's own REST layer was always going to own user/group management end to end, with no human-facing admin UI ever needed against the LDAP server itself. `lldap`'s dependency footprint (a full Rust toolchain, WASM build step) is disproportionate to what this project actually uses it for.

Replacing it opened four real, sequential design questions, tackled as tasks #723-727:

1. **What LDAP server, and how does Cix build it?** (#723/#724)
2. **How does Cix know which running container is the/a LDAP server?** (#725)
3. **How does Cix get user/group data into that server, and keep it current?** (#726)
4. **How does a container that needs to bind against LDAP get its own credentials, automatically?** (#727)

### Question 1: which server, and how is it built

`glauth` (`github.com/glauth/glauth`, plain Go) was chosen: single static binary, no bundled frontend, and — critically for question 3 below — two genuinely different backend "datastore" modes. Bootstrapping the Go toolchain (task #723) turned out to be already-satisfied: the same `/usr/local/go` toolchain staging `gitea.recipe` (Phase 20/21) already proved was re-verified working with a fresh trivial round-trip, no new work needed.

`pkg/recipes/glauth/2.4.0/recipe.sh` (task #724) builds glauth from source, dynamically linked against system glibc per this project's own TCC-toolchain-wide rule (`CGO_ENABLED=1` compiles the C amalgamation this project's toolchain already builds elsewhere, e.g. `mattn/go-sqlite3` in `gitea.recipe`). `lldap.recipe` itself is left untouched, not deleted — a real, working recipe kept for potential future use, per explicit user direction.

### Question 2: server registration

`daemon/include/ldap.h`/`daemon/src/ldap.c` gained `struct ldap_server_binding {container_name, config_path}` and `ldap_server_register()`/`unregister()`/`find()`/`forget()` — deliberately mirroring `dns_server_register()`'s own shape (`daemon/src/dns.c`), including `ldap_servers.json` persistence from the very start (`ldap_init()`), so this never repeats DNS's own real, previously-hit gap (ADR-0091: DNS server bindings were purely in-memory for a while, silently lost on every restart).

One deliberate simplification versus DNS: LDAP registration takes no `pid`/`pidfd` and does no filesystem write or signal at registration time — it is pure bookkeeping. DNS's own registration needs both because dnsmasq only reads its hosts file once at process startup, so a fresh write has to be pushed and SIGHUP'd in. Whether glauth needs the same treatment is answered by question 3.

`LDAP_SERVER_MAX` is 32 — multiple LDAP-serving containers can be registered simultaneously, not just one. This matters for question 3's own consequences (see "Multiple servers / redundancy" below).

### Question 3: how data gets into glauth, and the SQLite course-correction

The first implementation attempt (never committed, caught in review) wrote directly into glauth's SQLite backend as a new stateful format Cix would own outright — a new `libsqlite3` C dependency, confirmed to build cleanly under this project's own TCC toolchain and `-Wall -Werror` flags. Working code. But raised directly by the user as the wrong shape: *"i don't like the sqlite thing, I prefer we write as per dns, and glauth slurps the records in somehow, and keeps them updated? same as we have for dns?"* — i.e. Cix should own the durable record and render it into whatever format the target server actually reads, the same split DNS already uses (`dns_record_*()` for the durable record, `dns_write_hosts_file()`/`dns_server_sync_all()` for the rendered push), not a second, independently-owned stateful datastore.

Confirmed directly against glauth's real upstream source (not assumed) that this is not only possible but simpler than DNS's own mechanism: `v2/glauth.go`'s `startConfigWatcher()` is a genuine `fsnotify` watcher on glauth's own config file, active whenever that file sets `watchconfig = true`, and it reloads automatically on **any** write — no signal needed at all. glauth's "config" datastore backend (as opposed to "sqlite"/"embed"/"ldap"/"owncloud"/"plugin") defines users and groups directly as `[[users]]`/`[[groups]]` TOML array-of-tables stanzas within the **same** config file that also carries the server's own bootstrap settings (`[backend]`, `[ldap]`, `[api]`, TLS paths) — unlike dnsmasq's separate `--addn-hosts` data file, there's no dedicated data-only file to write.

**Decision**: rebuilt around the "config" datastore. `daemon/include/ldap.h`/`daemon/src/ldap.c` own `struct ldap_user`/`struct ldap_group` as durable records (`<data-dir>/ldap_users.json`/`ldap_groups.json`, mirroring `dns_record_create()`/`update()`/`delete()`/`find()` exactly), and `ldap_record_sync_all()` (the `dns_server_sync_all()` analog) re-renders the *entire* current user/group set as TOML and writes it into every currently-registered, currently-running server's config file on every mutation — matching `dns_write_hosts_file()`'s own always-whole-file-rewrite behavior, not an incremental patch. `ldap_write_config_file()` preserves everything the operator staged above the first `[[users]]`/`[[groups]]` marker byte-for-byte before appending the fresh render, using the fact that a TOML array-of-tables header can only ever start a line at column 0 as an unambiguous, syntactically-sound truncation boundary (`memmem()` on `"\n[[users]]"`/`"\n[[groups]]"`) — not a heuristic guess.

The abandoned SQLite work was fully reverted: `libsqlite3`'s link-line addition (`Makefile`), its staging into `mkbootroot.c`'s `shelled_bin_libs[]`, and the never-committed `sqlite.recipe` were all removed; `glauth.recipe` (task #724) was simplified in the same pass to drop its own `-tags embedsqlite` CGO/go-sqlite3/submodule complexity, since the config datastore needs none of it.

Passwords: `passsha256` (a real, documented glauth "config" datastore field) via cixd's own already-linked OpenSSL `libcrypto` SHA-256 — never persisted or echoed as plaintext, and never returned by any `GET` (only a `has_password` boolean is).

**Multiple servers / redundancy**: because `ldap_record_sync_all()` iterates every entry in `g_bindings[LDAP_SERVER_MAX]`, not a single pinned target, registering two (or up to 32) glauth-serving containers gives every one of them the identical, always-current user/group set with zero extra work per write — the same fan-out redundancy model DNS already provides for multi-server dnsmasq deployments. Client-side failover in front of them (round-robin, a VRRP-fronted VIP, DNS SRV records) is an external topology choice, outside this daemon's own scope, same as it would be for any LDAP deployment.

### Question 4: automatic provisioning

Task #727 extends `POST /v1/containers` with `ldap_provision`/`ldap_user`/`ldap_group`/`ldap_uid`/`ldap_secret_dir`, closely mirroring `dns_register`'s and `pki_issue`'s own container-creation-hook shape (parsed in `create_container_from_body()`, fired in the same post-`register_container_pidfd()` block `pki_issue` already uses, since secret delivery needs a live `entry->handle.pid`).

Two real design forks were resolved explicitly with the user via `AskUserQuestion` rather than assumed, since both materially shape task #731's own downstream design (wiring a jump box's SSH auth to LDAP):

1. **What gets provisioned**: a **service/bind account for the container itself**, not a human login account. Human accounts are created directly via task #726's own `POST /v1/ldap/users` — this hook never creates one. The provisioned account's `owner_container` field is set to the container's name (mirroring `dns_record.owner_container`/PKI's own cert-owner field exactly), and `ldap_user_forget_owner()` — searching BY the owner field, not by name (the same deliberate post-ADR-0092 pattern `dns_record_forget_owner()`/`pki_cert_forget_owner()` already established, since a container's own name and its LDAP username can differ when `ldap_user=` overrides the default) — removes it automatically on container delete.
2. **Default capabilities**: a minimal `"search"` capability (`action = "search"`, `object = "*"`) is granted now rather than deferring all capability/ACL work to task #728, since glauth denies all LDAP operations by default and a zero-capability bind account couldn't do anything useful. Real TOML syntax was verified directly against glauth's own `v2/sample-simple.cfg` before writing any rendering code.

The delivered credential (`ldap_generate_secret()`: 16 raw bytes from `/dev/urandom`, hex-encoded to 32 characters — a new, minimal, in-process randomness primitive; PKI key generation shells out to `openssl` instead, but a single bind secret doesn't need a real keypair) is written into the container's own filesystem at `<ldap_secret_dir>/bind.secret` (default `/etc/cix-ldap`), chmod `0600`, via `/proc/<pid>/root/` — byte-for-byte the same `persist_mkdir_p()` + `persist_atomic_write()` + `chmod` shape `pki_cert_deliver()` already established for a TLS keypair, just for one secret file. **The plaintext secret is never persisted anywhere in Cix's own state** — only its SHA-256 hash (`passsha256`) survives in the durable user record — so every single fire of this hook, including every `restart:"always"` respawn, generates and re-delivers a fresh secret even though the underlying account may already exist (`LDAP_RECORD_ERR_DUPLICATE` tolerated the same way `pki_issue` already tolerates `PKI_ERR_DUPLICATE` on a respawn's fresh pid).

## Decision

Adopt, as the settled shape for this project's LDAP subsystem going forward:

- **glauth**, its "config" datastore, dynamically linked, no CGO/SQLite complexity.
- **Registration is pure bookkeeping** (no pid/signal needed) because glauth's own `fsnotify` watcher does the work DNS's own SIGHUP push exists to work around.
- **Cix owns the durable record; the target server's own config file is a rendered, always-fully-rewritten projection of it**, preserving any operator-authored prefix above the managed `[[users]]`/`[[groups]]` tail — the same split DNS already uses, not a second independently-owned datastore.
- **Multiple registered servers are already fully supported** with zero extra mechanism — `ldap_record_sync_all()` fans out to every one.
- **The provisioning hook creates service/bind accounts only**, owner-tracked and auto-cleaned-up on container delete, with a minimal default capability grant and a never-persisted, always-freshly-generated secret.

## Consequences

- No new external dependency (`libsqlite3`) ships in the final design — one less thing `mkbootroot.c` needs to stage, one less link-line entry.
- The write-through model is now genuinely uniform across DNS and LDAP: the same mental model, the same marker-based-preservation technique, the same multi-server fan-out — a real instance of "solve a problem once, reuse it" rather than two parallel designs for a structurally identical problem.
- A container's own LDAP bind credential is never a long-lived secret an operator has to rotate manually — every restart mints a fresh one automatically, at the cost of every `restart:always` respawn briefly needing its client-side consumer (task #730/#731's jump box) to tolerate a credential change. This tradeoff was made deliberately (an agent-recommended, user-unchallenged design decision during #727), matching PKI's own "delivery is one-time, no live resync" precedent in spirit but going further since a bind secret, unlike a TLS cert, has no meaningful "still valid" state to preserve across a respawn.
- Task #728 (this document) closes out the LDAP redesign's own deferred documentation debt; task #731 (jump box SSH auth wired to LDAP) can now proceed with a settled, load-bearing answer to "what account does a consuming container actually get."
