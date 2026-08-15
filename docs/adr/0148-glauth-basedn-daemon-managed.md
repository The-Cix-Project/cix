# 0148 — glauth's own baseDN becomes daemon-managed

## Status

Accepted

## Context

Directly after ADR-0147 shipped, the user asked a real, pointed architecture question while re-provisioning this project's own live LDAP infrastructure by hand: shouldn't the LDAP base DN be "genuinely dynamic," not something typed by hand in several places? Investigated directly rather than assumed, the base DN turned out to live in **four independently-authored places**, only agreeing because the same string had been typed into all four during this session's own manual setup:

1. `hostauth-config`'s own `ldap_base_dn` field — real, persisted, API-owned (`daemon/src/hostauth.c`).
2. Each registered glauth server's own `glauth.cfg`, `[backend] baseDN = "..."` — an operator-authored file, staged once via `--file=` at container creation. Confirmed by reading `daemon/src/ldap.c`: `thincd` has never written a byte of this section, only the `[[users]]`/`[[groups]]` tail below it.
3. Each LDAP-*client* container's own `nslcd.conf` `base` directive — opaque `files[]` content, a different container entirely, thinC has no visibility into it once staged.
4. Each such client's own `ldap-authkeys.conf` (or equivalent `AuthorizedKeysCommand` bind-credential file) `LDAP_BASE` value — same as (3).

Not every one of these four is closable the same way. (1) is already the real source of truth. (2) is closable *for real*: a registered glauth server is a genuine, tracked thinC resource — the exact same one `ldap_record_sync_all()` already rewrites on every user/group mutation. (3) and (4) are not closable without inventing a new mechanism this project doesn't have: a way to know that some arbitrary container's own arbitrary staged file needs a thinC value substituted into it. "Jump box" is not a thinC concept; it is just a container someone built with `pkg install` and `files[]`. Templating into opaque file content on the operator's behalf would be exactly the kind of unintuitive, assumption-laden magic the user asked to avoid — worse than the current honest gap, not better.

## Decision

**Close (2), document (3) and (4) as a deliberate, permanent boundary — don't fake either.**

`daemon/src/ldap.c`'s `ldap_write_config_file()` — the function that already rewrites the `[[users]]`/`[[groups]]` managed tail on every mutation — now also inspects the preserved *prefix* portion for a line matching the real, confirmed glauth convention `baseDN = "..."` (double-quoted TOML basic string, per glauth's own `sample-simple.cfg`). If found, only that line's quoted value is rewritten to match `hostauth_ldap_base_dn()` (a new getter, `daemon/src/hostauth.c`) — every other byte of the operator's own `[backend]`/`[ldap]`/`[behaviors]`/TLS settings is untouched, the same "preserve everything, replace only what's owned" discipline the managed tail already has.

This is deliberately conservative in three ways:

- **No match is a legitimate no-op**, not an error. A config that spells `baseDN` with single quotes, or omits it entirely, is left exactly as the operator wrote it — this function never invents structure that isn't already present in the recognized form.
- **An empty `ldap_base_dn`** (never yet configured via `PUT /system/hostauth-config`) skips the rewrite entirely, rather than blanking out a value the operator already has working. `hostauth-config` is the real source of truth only once it's actually been set.
- **Client-side files stay untouched, on purpose.** `nslcd.conf`/`ldap-authkeys.conf`/anything else staged into an arbitrary container's own `files[]` is not, and will not become, something this daemon edits. That boundary is now stated explicitly in `daemon/include/ldap.h`'s and `daemon/include/hostauth.h`'s own doc comments, and in `docs/api/README.md`/`openapi.yaml` — not a silent gap for the next person (human or otherwise) to rediscover by grepping the source, the way this session did.

## Consequences

- `hostauth-config.ldap_base_dn` is now genuinely the single source of truth for one of the four copies, not a fourth independently-typed one. Changing it via `PUT /system/hostauth-config`, followed by any LDAP mutation (which already triggers `ldap_record_sync_all()`), pushes the new value into every currently-registered server automatically.
- Two pre-existing doc inaccuracies were found and fixed while writing this, both predating this change: `docs/api/openapi.yaml`/`docs/api/README.md` both claimed LDAP server registration "never touches the container's filesystem," which was already false before this ADR — registration has called `ldap_record_sync_all()` since task #726, immediately writing the current user/group set. Only "never sends a signal" was ever accurate.
- Renaming an LDAP base DN deployment-wide (a domain rename, matching the user's own original question) is now a **two-step, not one-step, but no longer four-step** operation: `PUT /system/hostauth-config` with the new `ldap_base_dn` (closes copy 2 automatically, for every registered server), then a manual pass updating any client container's own `nslcd.conf`/`ldap-authkeys.conf`/equivalent (copies 3 and 4, unavoidably manual, now documented as such rather than silently assumed away).
- New regression coverage in `test/test_ldap.c`: a registered server with a real `baseDN = "dc=old,dc=example"` line confirms the OLD value survives untouched while `ldap_base_dn` is still unset, then confirms the rewrite lands correctly (new value present, old value gone, every other prefix line — `datastore`, `IgnoreCapabilities` — still intact) once `ldap_base_dn` is set and any LDAP mutation triggers a sync.
