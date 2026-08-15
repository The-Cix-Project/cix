# 0145 — Retire the file-rendered SSH-target sync mechanism (task #731)

## Status

Accepted

## Context

ADR-0144's own Decision section already stated the intent plainly: real, live-LDAP container SSH login (task #838) "replaces the file-rendering SSH-target mechanism entirely." That replacement only became real once Part 122 shipped and was verified end to end on the real `jumpbox1` container — real pubkey login via a live `AuthorizedKeysCommand` LDAP query, real password login via `pam_ldap.so`'s own bind-as-user check, both against a real throwaway LDAP account, both surviving a full daemon rebuild and reboot. Until that point the older mechanism (`POST /v1/ldap/ssh-targets`, task #731: thinC itself rendering `/etc/passwd`/`/etc/group`/`/etc/shadow`/`~/.ssh/authorized_keys` directly onto a registered container's filesystem on every LDAP mutation) was the only real, working SSH-login path this project had, and stayed in place deliberately rather than being torn out ahead of its own replacement being proven.

With the live-LDAP mechanism confirmed working, the user asked directly to remove the older one now that it's genuinely redundant, not hypothetically redundant: "let's remove the old mechanism." Keeping both indefinitely would be a real, ongoing violation of **No Parallel Implementations** — two ways to get a Unix account onto a container for SSH purposes, one of which (the file-rendered copy) is exactly the "copy vs. query live" gap ADR-0144 was raised to close in the first place.

## Decision

Remove the file-rendered SSH-target mechanism entirely, not deprecate it in place:

- **Daemon**: `daemon/src/ldap.c`'s whole "SSH-backed login sync" block (`g_ssh_targets`, `ldap_ssh_target_register`/`unregister`/`forget`, `write_managed_tail()`, `write_accounts_for_pid()`, `ldap_ssh_sync_all()`) and its call sites (`ldap_record_sync_all()`, container-delete cleanup, daemon startup init/repoint). `daemon/include/ldap.h`'s matching declarations.
- **API**: `POST`/`GET /v1/ldap/ssh-targets`, `DELETE /v1/ldap/ssh-targets/{container}`, and their request/response schemas — gone from `docs/api/openapi.yaml`, not deprecated-but-present.
- **CLI**: `thincctl ldap ssh-target register/ls/unregister` — gone from `cli/src/main.c` and its own help text.
- **State**: `ldap_ssh_targets.json` dropped from the legacy flat-layout migration list (`migrate_flat_layout_to_grouped()`) — no code path ever references that filename again.

What stays: `ssh_public_key` itself, still a real field on `ldap_user` — now rendered exclusively as glauth's own `sshkeys = [...]` LDAP attribute (Part 119), the input the live `AuthorizedKeysCommand` query reads. Removing the sync mechanism doesn't touch the underlying data model, only the now-redundant second consumer of it.

No migration path is provided for an existing registration — this project has exactly one real deployment (192.168.15.95), and `jumpbox1` (the only container that was ever registered) had already been recreated onto the live-LDAP mechanism before this removal landed, confirmed via `GET /v1/ldap/ssh-targets` returning no registrations immediately before the daemon code was deleted.

## Consequences

- A real, breaking API/CLI removal, not an addition — anything outside this project that called `POST /v1/ldap/ssh-targets` or ran `thincctl ldap ssh-target ...` stops working the moment the new daemon build deploys. Acceptable here because this project has exactly one real operator and one real deployment, both consulted directly before the removal.
- `docs/api/README.md`'s "Real, live-LDAP SSH login" section is now the *only* SSH-login mechanism documented — no more "two mechanisms, pick one" framing for future readers to reconcile.
- Confirms ADR-0144's own original decision was correct in substance, not just in stated intent: the replacement genuinely works standalone, with nothing left leaning on the older mechanism as a fallback.
