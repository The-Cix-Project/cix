# 0268 — Write authentication is visible, and its preconditions are protected

## Status

Accepted

Issue [#370](https://git.home.arpa/itdlabs/cix/issues/370). Extends
[ADR-0144](0144-host-authentication-and-real-ldap.md)'s write-gating with the two properties it
turned out to be missing.

## Context

192.168.15.95 accepted unauthenticated writes for four days, and a full day of
deploys went through it before anyone noticed. It was found by accident, when a
script failed on a missing token file that nothing had actually needed.

Two separate things made that possible.

**Gating has a precondition nobody was protecting.** ADR-0144's rule is that a
mutating request needs an authenticated admin-group member, and
`hostauth_gating_active()` decides whether the rule is in force:

```c
if (g_config.admin_group_count == 0)
	return 0;
return ldap_user_for_each(check_one_user, NULL);
```

The second half is the trap. Gating requires an admin group to be configured
**and** an enabled user to be in it, and five ordinary operations could break the
second half while leaving the first looking perfectly correct:

1. deleting the group named in `admin_groups`
2. renaming it, or changing its `gidnumber` — the config names it by NAME and its
   members join by GID, so either orphans the invariant
3. deleting the last admin user
4. disabling that user, or removing them from every admin group
5. pointing `admin_groups` at a group nobody is in

None of them looks like "turn authentication off". Every one of them does.

**And the resulting state was invisible.** `GET /v1/health` returned
`{"status":"ok"}`. Nothing logged gating state at boot. The dashboard rendered
"logged in: <user>" once you logged in and rendered nothing at all when gating was
off entirely. An open control plane and a secured one were indistinguishable from
every surface this project offers.

The specific cause on that host was mundane — the 2026-09-06 reinstall wiped
`STATE_DIR/hostauth_config.json` and the `cix-admins` group, and nobody re-enabled
it. A reinstall doing that is a reinstall doing its job. Going four days without
anyone being able to tell is not.

## Decision

**Make the transitions unreachable through the API, and make the residual state
impossible to miss.**

Five guards refuse the operations above with `409` and a message naming the fix.
They share three predicates — `hostauth_group_is_admin_group()`,
`hostauth_user_is_admin()`, `hostauth_admin_user_count()` — and
`hostauth_gating_active()` is now written in terms of the last of them, so there
is one definition of "an admin user" rather than two that can drift.

Two of the guards are deliberately narrower than they first appear:

**Only the LAST admin is protected.** While another admin remains, deleting or
de-admining one is an ordinary administrative act and stays allowed.

**Only turning ACTIVE gating off is refused.** Setting `admin_groups` before the
admin users exist is how an operator turns gating on for the first time, and
gating is inactive throughout that. A blanket "admin_groups must have a member"
rule would obstruct enabling authentication in order to protect authentication.
The test is therefore *active now and inactive after*, not merely *inactive after*.

Visibility is added in four places: `gating_active` on
`GET /system/hostauth-config`, `auth_gating_active` on `GET /health`, a `warning`
in the log store at every boot where gating is inactive, and a banner in the
dashboard.

## Alternatives considered

**Make `hostauth_gating_active()` fail closed** — once an admin group is
configured, refuse writes whether or not anyone is in it. Simple, and it needs no
guards at all. **Rejected**: it converts an orphaned config into an API lockout
whose only recovery is [ADR-0146](0146-ldap-startup-resync-and-break-glass-recovery.md)'s `cix-recover`
boot entry, on a host with no shell. That trades a silent hole for a new way to
brick a machine, and this project has already paid for one break-glass trip. It
remains a legitimate follow-up if the owner wants it; it is not the right first
move.

**Latch gating on once it has ever been active.** Removes the lockout risk of
failing closed, since a fresh install never latches. **Rejected**: it needs
persisted state that can itself be lost or restored wrong, and a latch stuck on
with no admin user is the same lockout by a longer route.

**Report gating state, guard nothing.** Cheapest, and it does close the specific
four-day incident. **Rejected**: it leaves five one-request paths to an open
control plane and relies on somebody reading a banner. Visibility is the backstop
for state arriving from outside the API, not a substitute for the API refusing to
create that state itself.

**Change `status` in `/health` rather than adding a field.** **Rejected**: a fresh
install has no admin by design and would report unhealthy forever, which teaches
operators to ignore the field; and this project's own deploy tooling polls health
for liveness, where "is the daemon up" and "is it authenticating" are different
questions.

## Consequences

- Five operations that used to succeed now return `409` in one specific
  configuration each. All five were paths to an unauthenticated control plane.
- **One residual case remains, by construction**: state arriving from outside the
  API — a restored or hand-edited `hostauth_config.json`, or a reinstall — is not
  guarded, because there is no request to refuse. That is precisely what the
  visibility half covers, and it is the case that actually happened.
- `auth_gating_active` is served to unauthenticated callers. Deliberate: one
  unauthenticated POST already reveals it, so withholding it would protect nobody
  while keeping it from the operator who needs it.
- The boot warning must run after **both** `ldap_init()` and `hostauth_init()`.
  Gating reads the user table, which is ldap's; asking earlier would read an empty
  table and report every boot open regardless of the truth.
