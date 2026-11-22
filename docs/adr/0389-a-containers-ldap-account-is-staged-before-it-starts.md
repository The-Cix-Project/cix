# 0389 — A container's LDAP account is staged before it starts

## Status

Accepted, 2026-11-22, by the owner (*"yes go ahead"*), after asking what it means for directory changes made while a service runs and what the drawbacks are (answered below, under Consequences). Issue #733.

## Context

A service that checks logins against the site directory usually has to search it first, and the search needs a credential of its own. Two of the site's services showed the two ways that goes wrong:

- **The forge (Gitea)** bound with the platform's own LDAP client credential, `{{LDAP:BIND_DN}}`/`{{LDAP:BIND_PASSWORD}}`, the account every container's nslcd uses. It ended up in a process argument list and in Gitea's database. Gitea can bind as the user logging in, and now does, so it holds no LDAP password at all.
- **`nas` (lighttpd's `mod_authn_ldap`)** cannot: it always searches first, with a bind DN and password from its config file. It holds that same platform credential today.

Cix already has the right identity for this, `ldap_provision`: a search-only account per container, with a fresh secret at every start, owned by the container and removed with it. Two things kept it from being usable:

1. **It ran after the container started.** The block lived after `clone3()` and wrote `bind.secret` through `/proc/<pid>/root`, so a service reading its credential at startup could start before the file existed. #414 removed exactly that race for certificates (`pki_issue` is staged before start) and missed this one.
2. **The secret existed only as a file.** A service that wants the credential inside its own config, as lighttpd does, had no way to get it there. The deployment's `{{LDAP:...}}` tokens are filled in at apply, before the secret exists.

## Decision

1. **`ldap_provision` runs at staging time, before `clone3()`.** The account is created (or, on a restart, takes the new secret), and `bind.secret` is staged into the container's tree with every other file. The after-start block is removed.

2. **A file in `files[]` may name the account:** `{{LDAP:PROVISIONED_DN}}` and `{{LDAP:PROVISIONED_SECRET}}`, filled in as that file is staged (`ldaptoken_render()`, `daemon/src/ldaptoken.c`). The DN is glauth's own form for the user, `cn=NAME,ou=GROUP,ou=users,BASE`.

3. **Refuse rather than start without an identity.** A provisioned token in a container without `ldap_provision`, or an unknown `{{LDAP:PROVISIONED_...}}` name, is a 400. An account that cannot be created or updated, or a secret that cannot be staged, is a 500. Before, failure here was logged and the container started without its credential; `pki_issue` already refuses since #414.

4. **The plaintext secret stays out of Cix's state.** Only its bcrypt hash is stored, as before. The daemon's copy is wiped (`explicit_bzero`) once the files are staged.

## Alternatives considered

- **One shared search-only account for all services.** Rejected: one password reused everywhere, which is what Gitea was just taken off. A per-container account is revoked by deleting the container.
- **Keep writing after start and have services wait for the file.** Rejected for the reason #414 gave: every image would have to work around a race the platform creates.
- **Fill the tokens at deployment apply.** Not possible: the secret is generated per start, after apply.

## Consequences

- **Directory changes made while a service runs take effect as before.** Users, passwords and group membership are looked up at each login, so a new user, a changed password or a removed member applies at the next login, with no restart.
- **The service's own account is fixed for the container's run.** It changes at each start. If that account is deleted or changed while the service runs, the service's lookups fail until it restarts. Logins stop; nobody gets in who shouldn't.
- **The secret rotates on restart, not on a timer.** For a search-only internal account that is ordinary practice, and far better than a shared credential that never rotates.
- **The secret sits in the service's config inside the container**, readable by root there. It permits only directory searches, and only as that one account.
- **A new account reaches glauth through its config reload,** within about a second. The container's start comes after that, so the first lookup normally finds it. This is not a guarantee, and if a first login is ever refused right after a start, this is where to look.
- **Bcrypt still runs on the event loop** (~350 ms, #611) at each start of such a container, as before; it has only moved earlier.
