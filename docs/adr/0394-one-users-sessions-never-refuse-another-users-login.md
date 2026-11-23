# 0394 — One user's sessions never refuse another user's login

## Status

Accepted, 2026-11-23, by the owner ("Accept"), for #738.

## Context

The session table holds 64 sessions (`HOSTAUTH_SESSION_MAX`), and an unused one lives 15 minutes. When it is full, a login is refused: `too many active sessions -- try again shortly`.

On gibsson-1 on 2026-11-23 the owner could not log in. `cixctl hostauth-sessions ls` showed 61 sessions for `claude` and 1 for `osakka`: my build-wait loops ran `cixctl login` on every pass. The client is fixed. The product gap remains: any one user, whether a script, an agent or a misbehaving client, can make the box refuse every login, the owner's included. The only way back is another session that already exists, which a locked-out owner does not have.

## Decision

1. **A user holds at most 16 sessions.** A login past that ends that user's least recently used session and takes its slot. A user's own excess only ever costs that user.
2. **A full table never refuses a login.** If all 64 slots are taken (64 users each within their own limit), a login ends the least recently used session of the user holding the most, and takes its slot.
3. **Each ending is logged**, naming the user and why, so a client churning sessions is visible in the log rather than only in its victims' 401s.

## Alternatives considered

- **A larger table.** Rejected: it moves the threshold and keeps the failure.
- **Refuse only the user who holds the most.** Rejected: a user cannot log out sessions whose tokens it has lost, which was exactly my case, so it would be locked out for 15 minutes for nothing.

## Consequences

- A user who really runs more than 16 clients at once sees the oldest one logged out. App passwords (ADR-0317) are the route for machines and are not sessions, so this does not touch them.
- `test_hostauth` fills one user's sessions past the limit and checks another user can still log in, and that the first user's oldest session is the one ended.
