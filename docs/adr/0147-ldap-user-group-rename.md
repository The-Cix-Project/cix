# 0147 — Renaming an LDAP user or group in place

## Status

Accepted

## Context

`PUT /v1/ldap/users/{name}` and `PUT /v1/ldap/groups/{name}` have always treated `{name}` as the sole identifier of which record to edit — the request body's own `name` field (already accepted and parsed, since the same body shape is shared with `POST .../create`) was explicitly discarded: `daemon/src/main.c`'s `handle_ldap_user_update()` carried a `(void)body_name; /* the URL path's name is authoritative for PUT, not the body's own */` comment making this deliberate, not an oversight. The only way to rename a user or group was delete-then-recreate under the new name.

The user asked directly, after a session spent re-provisioning this project's real LDAP infrastructure from scratch: "I want to be able to adjust a user name, gid, memberships, passwords, keys on user management... right?" — confirmed as a real, current gap by reading the dispatch code directly (not assumed from the CLI's own printed `--help`, which had already been shown to omit real flags elsewhere in this codebase).

Renaming a plain user is low-risk — nothing else in this daemon indexes another record by a user's own name (group membership is already by `gidnumber`, never by name). Renaming a **group** is not: `hostauth-config`'s own `admin_groups` list (`daemon/src/hostauth.c`) is an array of group *name* strings, matched by string comparison in `hostauth_gating_active()`/`ldap_user_is_in_group()`. A group rename that didn't also update `admin_groups` would silently drop that group out of write-gating the instant the rename landed — a real, self-inflicted lockout risk of exactly the kind ADR-0146 was raised to close (a different root cause, the same class of incident: an operator action silently breaking the one mechanism protecting the API, discovered only when the next login fails).

## Decision

Both `PUT` endpoints now accept an optional `name` field in the request body, distinct in meaning from the URL path's own `{name}`:

- Omitted, or identical to the current name: no rename, the pre-existing behavior, unchanged.
- A real, different value: renames the record. Validated the same way `POST .../create` validates a new name (`ldap_username_is_valid()`/`ldap_groupname_is_valid()`, no collision with a different existing record of the same kind) before anything is committed.

**The group case carries one deliberate cascade, the user case carries none.** `ldap_group_update()` (`daemon/src/ldap.c`) calls a new `hostauth_rename_admin_group()` (`daemon/src/hostauth.c`) after the group's own record is renamed and durably persisted, but before returning success to the caller — if the group's *old* name is currently present in `admin_groups`, it is rewritten to the *new* name and persisted too. A caller observing `LDAP_RECORD_OK` is guaranteed both changes landed together; a persist failure on the `admin_groups` side reverts the group's own rename rather than leaving the two inconsistent. `ldap_user_update()` has no equivalent step — there is nothing in this codebase to cascade to.

**What is deliberately NOT handled**: neither rename updates external, operator-authored configuration that referenced the record's old rendered LDAP DN (`cn=<user>,ou=<group>,<base-dn>`) — `nslcd.conf`'s own `binddn`, an `AuthorizedKeysCommand` script's own bind credentials, anything staged via a container's `files[]` at creation time. This daemon has no visibility into config it didn't itself render, the same posture already established for a `gidnumber` change's non-cascade to `primarygroup` references. An operator renaming a group or user that's already wired into container-side LDAP config needs to update that config themselves.

CLI: `thincctl ldap group update --name=NAME --gidnumber=N [--new-name=NEWNAME]`, `thincctl ldap user update --name=NAME [--new-name=NEWNAME] ...` — a distinct flag from `--name=` (which continues to mean "which record"), not a reused/overloaded one, avoiding the exact kind of ambiguity a prior CLI design in this project (`--file=`'s own owner/group fields, caught before shipping) already ran into once.

## Consequences

- `ldap_group_update()`/`ldap_user_update()`'s own C signatures grew a `new_name` parameter; the one pre-existing internal caller outside the two REST handlers (the container LDAP-service-account auto-provisioning respawn path in `daemon/src/main.c`) passes `NULL` — no rename, unchanged behavior.
- A rename response's own `name` field genuinely changes between request and response for the first time on these two endpoints — any caller keying off `{name}` in the URL path staying stable across a `PUT` should not assume that anymore.
- New regression coverage: `test/test_ldap.c` proves the rename mechanics (old name gone, new name resolves, collision rejected) for both users and groups; `test/test_hostauth.c` proves the critical safety property directly — renaming the *currently active* admin group, confirming `hostauth-config`'s own `admin_groups` follows automatically, and that a fresh login still succeeds immediately afterward.
