# 0370 — An app password never writes identities

## Status

Accepted by the owner on 2026-11-02, for #391: an agent should never be able to add users (*"this is not ok, and comporomises many ideas"*), enforced as *"App passwords never"*, chosen over a rule keyed on membership of `cix-agents` and over configuration alone. It amends [ADR-0317](0317-permissions-are-declared-by-the-api-contract.md)'s section 8 and reverses nothing in it.

## Context

ADR-0317 section 8 made app passwords machine credentials that carry exactly their user's permissions, with one exception: `authenticated`, which means a login session, so that a leaked app password cannot mint successors that outlive its own revocation.

[ADR-0368](0368-cixd-serves-the-api-as-mcp-tools.md) gave MCP agents their own directory users, authenticating with app passwords, and contained them by their groups. On 2026-11-02 an example agent group, `agents-full`, was given every permission. Its agent, `cix-agent-full`, the credential in agent-1's Claude Code, could then, over REST as well as MCP:

- create users and app passwords for any user;
- change what any group grants;
- delete users and revoke sessions.

An agent able to do that can widen its own reach and mint credentials by a route the `authenticated` exception does not cover. ADR-0368's containment, "a prompt injection reaches no further than that group allows", means nothing when the group allows changing groups. The owner's direction: an agent never adds users.

The audit log showed no identity write by `cix-agent-full`, and `identity:write` was removed from `agents-full` the same day. That fixed one configuration, and a single group edit could undo it.

## Decision

**An app password never satisfies `identity:write`, even when its user holds it.** `hostauth_authorize_app_password()`, the one place an app password is authorized, refuses it beside `authenticated`. Creating, changing or deleting users, groups, group permissions, sessions or app passwords takes a login session.

The rule is on the credential, not on agents by group membership, for three reasons:

- It is ADR-0317's own reason applied to the other route to the same harm. The `authenticated` exception stops an app password minting successors through `/whoami/app-passwords`; this one stops it through `/ldap/users/{name}/app-passwords` and every other identity write.
- It has one enforcement point. A rule keyed on `cix-agents` would need a membership check beside the permission check, and an agent user outside `cix-agents` would escape it.
- `tools/list` already offers a caller only what it may call, so every identity-write tool disappears from every agent's list with no MCP code.

## Consequences

- No agent, and no script holding an app password, can create or change an identity, whatever its groups say. `identity:read` is unaffected.
- A script that provisions users must log in for a session. That is the cost the owner accepted.
- An agent group should still never hold `identity:write`, and never the whole vocabulary, which would also make it an admin group under [ADR-0369](0369-an-admin-group-holds-every-permission-including-new-ones.md). This rule is the floor beneath that guidance, not a replacement for it.
- `test_hostauth` checks that `root_admin`, who holds every permission, creates groups with a session but is refused a group create and an app-password create with an app password, while still reading identities with it.
