# 0369 — An admin group holds every permission, including new ones

## Status

Accepted by the owner on 2026-11-01, for #670: *"yes admins get everything"*. It amends [ADR-0317](0317-permissions-are-declared-by-the-api-contract.md)'s permission mapping and reverses nothing in it.

## Context

ADR-0317 made `admin_groups` derived: it is the set of groups that hold every permission in the vocabulary. The mapping is saved per group as a list of words. When it is loaded, a saved word that this build's vocabulary lacks is dropped. Nothing said what happens to a word this build has that the saved file never named.

0.2.57-647 was the first release to add a word since ADR-0317 shipped: `mcp:call` (ADR-0368). Measured on 192.168.15.95 after it booted:

- `cix-admins` held every word except `mcp:call`, so it no longer held everything.
- `admin_groups` was derived as `[]`, where before it had been `["cix-admins"]`. Nothing announced the change.
- `POST /v1/mcp` from an admin session answered 403.

A fresh host on the same release provisions `cix-admins` with `mcp:call`, because the standard admin group is granted every word. So two hosts running one release disagreed about what their admin group holds. Nobody was locked out, because the lockout guard keys on `identity:write`, which `cix-admins` still held.

## Decision

**An admin group holds every permission, including any that a later build adds.** Being an admin is not a snapshot of the vocabulary at the moment the group was granted it.

The mechanism needs no new state. The state file already saves the derived `admin_groups` beside the mapping, so that an older build can still read it after a rollback. That list records which groups held everything when the file was written. At load, each group named there is granted every word of this build's vocabulary. If that changes the mapping, the result is written back.

A group that held only some permissions gains nothing. A new word reaches it only when an operator grants it.

## Consequences

- An admin group keeps admin when the vocabulary grows, on every host, with no operator step.
- `hostauth-config set --admin-group=NAME` already granted a named group every permission. Now that is true for the rest of the group's life, not just at the moment it is named.
- A permission meant to be held only by explicit grant, such as `mcp:call` for MCP agents, is still held by admins. Its own group, `cix-agents` (ADR-0368), is how a non-admin principal gets it.
- Taking a word away from a saved admin group by editing the state file by hand, while leaving the group in `admin_groups`, no longer sticks. Every writer of the mapping derives `admin_groups` again from the mapping, so the API cannot produce that state.
- `test_hostauth` loads a state file written under a smaller vocabulary and checks that the admin group holds every word, that a partial group gains nothing, and that the grant is written back.
