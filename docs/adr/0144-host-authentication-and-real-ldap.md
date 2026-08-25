# 0144 — Host authentication (group-based, LDAP-or-local) and real LDAP for container login

## Status

Accepted

## Context

Investigated directly while following up on the container-SSH work (ADR-0143's own follow-on question, "can we SSH into jumpbox with a key or password"): `cixd`'s REST API has **no authentication at all**, and never has — every `cixctl`/web-dashboard write this entire project has ever made succeeded with zero credentials, protected only by network reachability. Separately, the existing SSH-account mechanism for containers (`ldap ssh-target register`, ADR-0093-class work) is itself not real LDAP authentication — `openssh.recipe` builds with `UsePAM no`, and no `libnss_ldap`/`pam_ldap` recipe exists anywhere in this project; `sshd` has never actually spoken LDAP. Instead, `cixd` renders `/etc/passwd`/`/etc/group`/`/etc/shadow`/`authorized_keys` as flat files copied onto a registered container's filesystem on every LDAP user/group mutation. Raised directly by the user once this was understood: "we're engineering, not hacking" — real LDAP throughout, for containers *and* the host, not another rendered-copy mechanism layered on top.

Investigated live against the real `glauth` server before designing anything: confirmed the real DN structure (`cn=<name>,ou=<primarygroup-name>,dc=glauth,dc=com`, verified both from `glauth`'s own source and a live successful bind in its logs) and a real, blocking gap — `glauth` refuses every search, even a successfully-bound user searching for its own entry, because no capability grant exists anywhere in the current config; `glauth` also supports `[behaviors] IgnoreCapabilities = true` to bypass this outright.

## Decision

### 1. Data model foundation (this ADR's own first, prerequisite piece)

**Real bcrypt**, not the prior unsalted-SHA256 `passsha256` field. The original SHA256 choice was itself a deliberate, documented "No Hacks" decision (avoid hand-rolling bcrypt, a security-critical primitive with no audit trail) — correct at the time, when this was only ever a container-facing directory password. It stops being correct once these same credentials become the *host's own* authentication root of trust. Resolved without reversing the original reasoning: **vendor a real, audited, widely-deployed reference implementation** (OpenBSD's own `bcrypt.c`/`blowfish.c`, ISC-licensed, `daemon/src/vendor/`) rather than writing one from scratch or shelling out — still no hand-rolled crypto, just no longer avoiding bcrypt altogether. `glauth` supports `passbcrypt` as a first-class field, so this is a strict strengthening with zero loss of compatibility. `daemon/include/pwhash.h` is the thin project-facing wrapper (fixed cost factor 12, no runtime self-calibration — a known, documented cost beats one that could silently drift between logins on hardware of different speeds).

**Real secondary/supplementary group membership**, alongside `primarygroup` (`ldap_user`'s own `secondary_groups[]`/`secondary_group_count`, up to 16) — matches real POSIX semantics (`id -Gn`), needed for a proper, configurable group-membership authorization model rather than a single hardcoded group check. Rendered into `glauth.cfg` as `othergroups = [...]` (glauth's own real `User.OtherGroups []int` field). `ldap_user_is_in_group()`/`ldap_user_check_password()` are the two shared primitives everything below (both the local and LDAP host-auth backends) consult — one real answer to "is this user valid and in that group," not reimplemented per caller.

### 2. Host authentication: one authorization rule, two interchangeable backends

Write operations (`POST`/`PUT`/`DELETE`, and the container console — a `GET`-verb WebSocket upgrade that is functionally arbitrary command execution, judged by intent rather than HTTP method) require the caller to be authenticated *and* a member of a configurable admin group (or groups — a list, not a single hardcoded name). Reads stay open, unconditionally, forever.

**No second, separate "local host users" store.** `ldap user add`/`ldap group add`'s existing records — the same ones `glauth` renders from — become, from this ADR on, *the* host directory, checked one of two ways:

- **LDAP backend** (enabled and reachable): a real, live LDAP simple-bind against `glauth`, then a real search for group membership. Hand-rolled minimal LDAPv3 client in the daemon itself (bind + search only, no `libldap` dependency) — consistent with this project's own established precedent of hand-rolling protocols for the trusted daemon process rather than linking external client libraries (rtnetlink instead of `iproute2`/`libnl`, hand-rolled HTTP/WebSocket/JSON).
- **Local backend** (LDAP disabled, or unreachable within a bind timeout): `ldap_user_check_password()`/`ldap_user_is_in_group()` directly, in-process, no network call — the exact same underlying data, not a cache or a copy.

**Bootstrap safety, no separate break-glass secret.** Writes stay open exactly until the first user lands in a configured admin group; the instant one exists, writes lock down automatically. A fresh install can never be permanently locked out of its own API, and no separate enable-password/recovery mechanism is needed.

**Sessions**: an opaque bearer token, sliding idle timeout (configurable; 0 means every write re-authenticates, no session reuse at all), in-memory only — wiped on every `cixd` restart, matching this project's own "ephemeral unless there's a real reason to persist" precedent for the registry/container state.

### 3. Container-level real LDAP (the original ask)

Replaces the file-rendering SSH-target mechanism entirely, for any container that opts in: real OpenLDAP client libraries (`libldap`/`liblber`, built `--disable-slapd --without-cyrus-sasl` — client-only, no server), Linux-PAM, `nss_ldap`/`pam_ldap`, `openssh` rebuilt with `--with-pam`. `glauth` itself gets `IgnoreCapabilities = true` (recreating `ldap-1`/`ldap-2` to apply it — a static config-prefix setting, not something the existing managed-tail sync mechanism can touch live). SSH keys are **never** a PAM concern in OpenSSH — real pubkey auth needs `AuthorizedKeysCommand` doing a live LDAP search at login time, not a rendered `authorized_keys` file, closing the same "copy vs. query live" gap password auth already closes.

## Verification

This ADR shipped across 14 tracked parts (`docs/roadmap/ROADMAP.md` Parts 111-125, each with its own full clean-rebuild/regression-sweep verification) rather than one landing — recorded here as a consolidated summary, since this file itself had never gathered it into one place (a real, previously-existing documentation gap, closed by this note).

**Data model + local backend** (Parts 111-112): `test/test_hostauth.c`'s full real lifecycle — bootstrap-safety (writes open until the first admin-group user exists, then lock automatically), wrong-password rejection, unauthenticated/garbage-token rejection, a real authenticated-but-non-admin user correctly failing authorization while still passing authentication, logout invalidation and its own idempotency, and the `idle_timeout_seconds=0` single-use-token contract. Every one of the 46 pre-existing tests at the time continued to pass completely unmodified — none of them creates an admin-group user, so gating correctly never activates for them, proving the bootstrap-safety design in practice rather than only asserting it.

**LDAP backend** (Part 113): `ldapclient.c` verified standalone against a real local `glauth` process — bind, wrong-password rejection, nonexistent-user rejection, closed-port failure, single- and multi-term search — every case cross-checked against `ldapsearch`/`ldapwhoami`, not just self-consistency. **Live-verified against the real deployed `glauth` servers** (192.168.15.95's `ldap-1`/`ldap-2`): deployed via this project's own self-hosted rebuild path (`pkg hostbuild cix --deploy`), rebooted, confirmed the box came back on the new build, and a real throwaway LDAP user bound successfully (`resultCode=0`) against both servers directly.

**Container-level real LDAP** (Parts 114-124): CLI/web login flow, `nss-pam-ldapd`/`linux-pam`/`openldap-client` built from source under TCC, `openssh` rebuilt with real PAM support, `sshkeys`/`can_search` rendered as real `glauth` attributes, per-file owner/group on `files[]`, and — the original ask this whole ADR traces back to — real, live-LDAP SSH login (`nslcd`/PAM/`AuthorizedKeysCommand`) verified end to end on the real box: both password and pubkey auth succeed, a wrong password is correctly rejected. The old file-rendering SSH-target mechanism was then removed outright (Part 125's own follow-on), not left running in parallel.

**Full, independent real-box re-verification** (this session, after a genuine full reinstall of 192.168.15.95): every piece above reconfirmed from a blank slate — `ldap-1`/`ldap-2` rebuilt and switched from the local backend to live-LDAP, **verified across a real reboot** (the exact scenario ADR-0146's break-glass tool exists for), `jumpbox1` reconfirmed with real live-LDAP SSH login both ways. Host-auth write-gating itself was then actually activated on the real box (previously verified only in test harnesses and locally) and has protected every mutating call against it for the remainder of this session, including the extensive package/image/container work in later parts (task #843/#849/#850) — this ADR's own write-gating has been exercising itself continuously in production use, not just in its own dedicated test.

## Consequences

- The single largest net-new piece of infrastructure this project has added in one effort — real authentication where none existed, for both the host API and container login, replacing a rendered-copy mechanism with live protocol queries throughout.
- `cixctl`/web-dashboard usage patterns change materially: every write now needs a prior login once an admin-group user exists. Every deploy/verification workflow this project's own development process relies on needs to account for that from this point forward.
- The vendored bcrypt implementation is now a real, permanent dependency of this project's own credential storage — any future audit of "what crypto does this project trust" starts here, not with an in-house implementation.
- `glauth`'s `IgnoreCapabilities = true` means *any* successfully-bound user can search the whole directory — a real, deliberate trade-off (needed for NSS/host-auth group lookups to work at all) documented here rather than discovered by surprise later.
