# 0317 — permissions are declared by the API contract

## Status

Proposed. The owner decided sections 1 and 3 on 2026-09-29 and raised section 8; it becomes Accepted when they accept the whole. Written for #304, on the owner's decision of 2026-09-14 (*"write the RBAC ADR; the model is spec-driven"*) and their direction on the same issue that the contract states the policy and the enforcement is generated from it. Raised again by the owner on 2026-09-29, because Cix is now being downloaded by people other than its developers. Nothing is implemented; the implementation issues follow acceptance.

## What changes when this is accepted

Nothing runs differently on the day it is accepted. Accepting it fixes the model, the enforcement point and the migration, so the implementation can be split into issues that each ship and are verified on their own. Until those land, the current rule stands.

## Context

ADR-0144 gave the API authentication. Its authorisation is one binary rule, read from the code rather than recalled:

- `hostauth_authorize_write()` (`daemon/src/hostauth.c`) returns true while gating is inactive, and otherwise only for a valid session whose user is in one of `admin_groups`. It returns a yes or a no; nothing records *what* a user may do.
- The dispatcher decides which requests need it **before** the route is matched, by path (`daemon/src/main.c`, the block before the generated-table dispatch): every non-GET except `POST /v1/login` and `/v1/logout`, the container console, and the two identity reads (#490: `GET /v1/ldap/users` and `GET /v1/system/hostauth/sessions`). HEAD is treated as GET (#498).
- A **valid session of a user who is not an admin gets 401**, the same answer as no session, so "you are not logged in" and "you are not allowed" cannot be told apart.
- An LDAP group is `{name, gidnumber}` (`struct ldap_group`, `daemon/include/ldap.h`); there is nowhere a permission could live today.

So there are two kinds of person on a Cix box: one who can do everything, and one who can read. That is not enough once other people run Cix. Real deployments need an operator who restarts a container but cannot repartition a disk, an account that publishes recipes but cannot deploy a host build, and #391's agent principal, which must be able to restart a container and must not be able to delete an image. A token that can do everything is exactly what a prompt injection would want.

Measured to size the work (2026-09-29): `docs/api/openapi.yaml` has **309 operations** (132 GET, 87 POST, 43 PUT, 46 DELETE, 1 PATCH). The top-level path is a usable spine for a permission vocabulary, but `/system` alone is 66 paths across lifecycle, logs, storage, kernel, boot and configuration, so it cannot be one permission. `x-cix-expose` is on 308 of the 309 operations, and `apigen` accepts the one without it (`DELETE /containers/{name}/files`), so "an operation without the annotation fails the build" would be new behaviour, not existing precedent.

## Decision

### 1. The contract states each operation's permission

Every operation in `openapi.yaml` carries `x-cix-permission`, and `apigen` generates it into `struct api_route` beside the fields #282 already put there. **An operation without one fails the build**, and so does an unknown value. An endpoint with no stated authorisation becomes impossible to write, not something to audit for afterwards.

The annotation names the **permission the operation requires**, not the roles that may call it. That was the owner's direction on #304 and the reason is kept here: `POST /v1/containers` requires `containers:write` forever, whoever holds it. Which people hold it is an operator's runtime decision, and it must never require editing the API contract.

**One permission per operation**, not a list. "Requires A or B" almost always means the vocabulary is wrong, and a list invites the drift the annotation exists to prevent.

**The method-derived rule seeds, it does not default.** The owner's 2026-09-14 decision said *default method-derived (GET→read, mutating→write), with per-operation overrides*. A default would make "no stated policy" representable again. Decided by the owner on 2026-09-29: the method-derived rule is used **once**, to seed the annotation onto all 309 operations in one reviewed commit, and from then on every operation states its permission explicitly.

### 2. The vocabulary is small and closed

A permission is `<area>:<verb>`, or one of two special values:

- **`public`**: needs no session.
- **`authenticated`**: needs any valid session (`/v1/logout`, `/v1/whoami`).

**Verbs:**

- `read`: see state that is not public.
- `operate`: change the running state of something that exists, without creating, destroying or reconfiguring it.
- `write`: create, delete, reconfigure.

`operate` exists only where the distinction is real: start/stop/pause/unpause and service start/stop/restart on containers, and similar lifecycle verbs. It is what makes #391's agent expressible: `containers:operate` without `containers:write` or `images:write`.

**One area outranks its verbs.** `containers:console` is its own permission, because an interactive shell inside a container is more than any `write`.

**Areas** follow the path spine and split `/system` by what it touches:

- `containers`, `images`, `volumes`, `networks`, `storage`, `packages` (pkg, pipeline, software, deployments), `services` (dns, dhcp, ntp, syslog, ldap servers), `pki`, `schedules`, `config`
- `system` (update, reboot, shutdown, boot, factory reset, backup and restore)
- `identity` (LDAP users and groups, sessions, and the permission mapping itself)

The complete per-operation table is the first implementation issue. It is generated from the seed rule, then reviewed operation by operation. It is **not** written into this ADR, because it would be a second copy of the contract.

### 3. Reads need a login

Decided by the owner on 2026-09-29: **every read needs an authenticated caller**, not only writes. Today every GET except the console and the two identity reads is open, which is free reconnaissance for anyone who can reach the port: the disks, containers, networks, installed packages and site configuration of a box on a network Cix does not control. It also means reads such as the backup bundle (`GET /v1/system/backup`), whose container definitions can carry secrets in their environment variables, need no gate of their own.

Key material was already kept out of every GET (verified 2026-09-29): PKI returns certificates, never keys; signing keys report presence and fingerprint only; tokens report `auth_token_set`. So this closes what a stranger learns about the machine, not a key leak.

**What stays `public`, and why each one must:**

- `POST /v1/login`: it is how a caller gets a session at all.
- The dashboard's own static files: not API operations, so not in the contract. Without them the login page could not load.
- `GET /v1/health`: the liveness probe that monitors and load balancers poll, which they cannot log in to. Proposed, and the owner may take it back. It reports liveness only.

Everything else is `<area>:read` or stricter. Reads still work where they do today in two cases:

- **A fresh install.** Gating activates only once someone holds `identity:write` (section 6), so a box with no users answers as it does now. The test suite runs that way too.
- **The console shell (ADR-0034).** It is an ordinary API client, so on a box where gating is active it needs `login` before it can read. That is the consequence of the decision, stated rather than left to be discovered, and the console already tells the operator to log in when a request is refused.

### 4. One enforcement point, after the route is matched

The check moves from the path-based block **before** routing to a single check **after** the generated table has matched a route and **before** its handler is called. That is the only place the route's permission is known. No handler can then forget the check, and none can run without it.

- The exemptions become annotations. Login is `public`, logout `authenticated`, the console `containers:console`, the identity reads `identity:read`.
- **HEAD takes its GET's permission** (#498: a gate that a change of verb steps around is not a gate).
- **No session** → `401`.
- **A valid session without the permission** → `403`, naming the permission required. This is the "name the offender" posture of #282. It also separates the two cases that are both 401 today.

### 5. Groups hold permissions; users hold the union

- A group is granted a set of permissions. A user holds the **union** of the permissions of every group they are in, primary and secondary.
- There is **no deny**: union-of-grants is simpler and harder to get wrong, and "everyone in X except Y" is a group design question, not a policy one.
- The mapping lives in **hostauth's own configuration**, beside today's `admin_groups`, keyed by group name. It follows a group rename the way `admin_groups` already does (ADR-0147). It is not stored in the LDAP record.
- LDAP decides membership; `cixd` decides what membership grants. glauth renders users and groups as it does now and never sees permissions, because nothing but `cixd` checks them.
- **Live-LDAP mode** (ADR-0144's `ldapclient_bind()` path): which groups a user is in comes from where `hostauth_login()` already takes it. What those groups grant always comes from `cixd`'s mapping. A group the directory has and the mapping does not name grants nothing.

### 6. Granting is a permission, and the no-lockout guarantee carries over

Changing the permission mapping, users or groups requires `identity:write`. That is the permission that matters most.

Today's best property is that gating cannot activate before an admin exists, so a fresh install cannot lock itself out on a host with no shell. The same guarantee becomes:

- Gating activates only once at least one user holds `identity:write`.
- The API refuses any change that would leave no user holding it: removing the last such user, removing them from their group, or taking `identity:write` from the last group that grants it. The refusal says why, as `hostauth_would_gate()` does today.

### 7. Migration: nobody gains or loses anything

On the first boot of the build that implements this, every group currently in `admin_groups` is mapped to **every permission**, with no operator step. Each existing admin can do exactly what they could before, and nobody else gains anything. `admin_groups` stops being a separate list: it is the set of groups holding everything, shown through the same mapping API. `cixctl hostauth-config` keeps reading it until the implementation retires that field in its own change.

The standard groups that the owner's decision says are provisioned at init follow the same rule:

- **`cix-admins`**: everything.
- **`cix-operators`**: `public`, every `read`, every `operate`.
- **`cix-readers`**: every `read`.

They are created only where absent, and an existing group of the same name is never re-granted.

### 8. Machine credentials are app passwords, the same ones glauth uses

Raised by the owner on 2026-09-29: *"should we add token storage? Glauth handles app passwords, which I think should be the same?"* Yes, and it is what keeps this from becoming a second credential system.

Automation, CI and #391's agent need a credential that is not an interactive login with an idle timeout. There is to be **no separate API-token store**. Instead:

- **Stored with the user.** A user holds any number of named **app passwords**, kept in `cixd`'s own record store beside the main password. They are hashed with bcrypt exactly as `passbcrypt` is today, never stored or returned in clear, and shown once, at creation.
- **Rendered into glauth** in its app-password field, so the same credential authenticates an application's LDAP bind. `cixd` already renders the main password into glauth's `passbcrypt` (`daemon/src/ldap.c`). glauth's config has app-password fields (`passappbcrypt`); the implementation verifies the field name and encoding against the pinned glauth source before relying on it, as `passbcrypt`'s hex encoding was verified.
- **Accepted by the API on each request**, as HTTP Basic (`username` + app password), with no session to expire. That is what scripts need. The main password keeps going through `POST /v1/login` and a session; it is not accepted per request.
- **No scopes.** An app password carries its user's permissions, no more and no less. Least privilege for a machine is a dedicated user in a narrower group: #391's agent is a user such as `agent-1` in a group granted `containers:operate` and every `read`. Per-credential scopes would be a second permission model beside the group one.
- **Revocation takes effect at once.** Deleting an app password removes it from the store and from glauth's rendered config. The API checks the store on every request, so nothing cached keeps working.
- **Audited by name.** Every request authenticated by an app password is audited as `user` plus the app password's name, so a leaked credential can be traced to what it did and revoked without touching the user's other credentials.

Managing a user's app passwords needs `identity:write`, or for a user's own app passwords `authenticated`: people may rotate their own machine credentials without being able to grant anything.

### 9. Gates

Implementation issues must carry:

- `test_apiroute` and `test_api_surfaces`: every operation has a known permission.
- A negative test in which `apigen` refuses a spec with a missing or unknown one.
- Dispatcher tests for: 401 without a session, 403 with a session lacking the permission (the body names it), and 200 with it.
- A test that a user in two groups holds the union.
- A test that the last `identity:write` holder cannot be removed.
- App passwords: a request authenticated by one gets exactly its user's permissions; the main password is refused per request; a deleted app password is refused on the very next request and is gone from glauth's rendered config; the audit line names the app password.
- With reads gated: an unauthenticated GET on a gated box gets 401, `/v1/health` still answers, and a box with no `identity:write` holder still answers everything.

## What this rejects

- **Roles named in the contract** (`x-cix-rbac-role`): the owner's direction on #304. Adding a role would mean editing every operation it reaches, and the contract would encode a customer's organisation.
- **A policy file beside the contract**: two sources of truth, which drift.
- **Deny rules**: they make "what can this user do" undecidable by reading one list.
- **A separate API-token store**: a second credential system beside the one glauth already renders, with its own storage, rotation and revocation to keep in step. App passwords are the one mechanism (section 8).
- **Per-credential scopes**: a second permission model beside groups. A narrower machine is a narrower user.
- **Permissions stored in LDAP attributes**: they would put `cixd`'s authorisation in the directory, which in live-LDAP mode `cixd` does not control.
- **Keeping the path-based gate and adding checks to handlers**: that is how an endpoint ends up with no check.

## Implementation, in order

Each is its own issue:

1. The annotation, the vocabulary, the seed commit on all 309 operations, and `apigen`'s refusal.
2. The mapping store, its API, and the migration.
3. The enforcement point, with 401 and 403.
4. The standard groups at init.
5. App passwords: store, glauth rendering, per-request Basic authentication and audit.
6. `cixctl` and the dashboard, including the dashboard's logged-out first screen now that reads need a login.
