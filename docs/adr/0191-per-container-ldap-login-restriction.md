# 0191 — Which users may log into a container is a per-container statement, enforced by nslcd

## Status

Accepted

Extends [ADR-0144](0144-host-authentication-and-real-ldap.md)'s real-LDAP container login. That decision — `sshd` queries the directory live at connection time — is unchanged; what this adds is a way to say *which* accounts a given container accepts.

## Context

`ldap_client: true` (issue #66) stages `/etc/nsswitch.conf` and `/etc/nslcd.conf` into a container, rendered from the daemon's own LDAP client settings. The result is correct and complete: the container resolves and authenticates every account in the directory.

Every account is the problem. A jump box, a build box and a database box created this way all accept the same set of logins, because none of them has ever been asked a narrower question. The directory has groups; nothing was consulting them.

The mechanism was chosen from live evidence against the deployed glauth, not from what the LDAP world usually does:

- The traditional idiom is a `host` attribute on the user entry, listing the machines that account may log into. glauth's `config` backend exposes no custom attributes, so there is nowhere to put it. Adding one would mean changing the directory backend to support a feature only this would use.
- `memberOf` is already there. A user entry carries DNs of the form `ou=<group>,ou=groups,<base-dn>` for its **primary and its secondary groups** both — confirmed directly against the running server, along with the fact that filtering on them matches a member and returns nothing for a non-member. A user can be in many groups, which is exactly the shape "may log into several hosts" needs.

## Decision

**`ldap_allow_groups` on `POST /v1/containers` names the groups whose members may log in, and it is enforced by `nslcd` itself.**

The daemon renders one line into the staged `/etc/nslcd.conf`:

```
pam_authz_search (&(objectClass=posixAccount)(uid=$username)(|(memberOf=ou=<g>,ou=groups,<base>)…))
```

`nslcd` runs that search after authenticating and refuses a login it does not match. The restriction therefore holds because of a file in the container and the daemon the container already depends on — not because anything of thinC's is still running to check it. A control plane that has to be up for a security boundary to hold is a security boundary with an availability dependency.

Three rules follow from taking that seriously:

**Absence means no filter, not a permissive one.** Omitting the field stages no `pam_authz_search` at all — the pre-existing behaviour, unchanged. A filter written to allow everyone is a filter that can be got wrong; the safest way to express "no restriction" is to have nothing there to be wrong.

**Names are refused, never escaped.** The string is written into a file `nslcd` parses as an LDAP filter. Group names are already constrained to a plain character set everywhere else in this daemon, so anything outside it is a mistake worth reporting, not something to encode around. Escaping would mean quietly accepting a name that was never a group name.

**A group that does not exist is refused at creation time.** A typo would otherwise render a filter matching nobody — a container that admits no one, discovered at the worst possible moment. And `ldap_allow_groups` without `ldap_client` is refused too: there is nothing to restrict without it, and silently ignoring the field would read as a restriction in force when it is not.

## Consequences

Login policy is now a property of the container, decided at creation, visible in its own `/etc/nslcd.conf` — the same place an operator would look to understand any other part of how that container talks to the directory.

The bound is real but not tight: the rendered filter has a fixed buffer, and enough groups will overflow it (refused, not truncated). Group membership is also the only axis available — "this user, on this host, but only for these hours" is not expressible here, and would need a directory backend that carries more than glauth's config backend does.

Changing the allow-list means re-creating the container, since the file is staged at creation. That matches how every other staged-file decision in this system already behaves, and a login policy is not something that should drift underneath a running container without a deliberate act.
