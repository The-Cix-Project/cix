# 0271 — An action is attributable to a person

## Status

Accepted

Issue [#371](https://git.home.arpa/itdlabs/cix/issues/371) (Deploy 2 of four).
Prerequisite for the approval gates in that plan. Builds on
[ADR-0144](0144-host-authentication-and-real-ldap.md) (the write gate this reuses) and
[ADR-0070](0070-consolidated-log-store.md) (the store the trail lives in).

## Context

The audit trail recorded what happened and never who did it:

```c
logstore_write("audit", "info", "%s %s", req->method, req->path);
```

`daemon/src/main.c`. Method and path. Measured on a live host, a real line reads
`POST /v1/pkg/recipes` — an action with no actor. `struct api_ctx`, which every
handler receives, carried `fd`, `req` and the path parameters, and no identity at all,
so no handler could name its caller even if it wanted to.

That was tolerable while the dashboard could only look. It stops being tolerable in the
next change, which gives it buttons: an approval gate whose entire purpose is "a human
decided this" and which cannot say *which* human is theatre. It is also the wrong
posture for a box whose control plane was open two days ago
([#370](https://git.home.arpa/itdlabs/cix/issues/370)).

The information was already there and being discarded. `hostauth_authorize_write()`
resolves the caller's username into a local to check group membership, and then returns
a bare int.

## Decision

**The caller's username is resolved once at dispatch and carried on `struct api_ctx`,
and the audit line names it.**

Three things follow, each of which was a real choice.

### Resolved before the audit line, not after the gate

A rejected request is the one most worth attributing. Auditing only what got past
authorization would lose exactly the attempts an operator wants to see, so the resolve
happens above the audit write and a refused write now logs its own line at `warn`.

### `hostauth_peek_token()`, not `hostauth_check_token()`

The two differ deliberately: the check refreshes idle expiry and **consumes** a
single-use token; the peek does neither. The write gate still has to make its own real
check, so peeking first and checking after is one consume in the right place. Calling
the check twice would spend a single-use token on a log line — a session destroyed by
being written down.

### The trail always has a subject

Unauthenticated requests record `-` rather than an empty field, so every line carries a
subject in the same column and the trail stays greppable by a fixed shape. `-` is
reachable honestly: host-auth gating is off until an admin user exists, and a fresh
install must never be locked out of its own API.

A login is the one request that cannot be attributed this way, because the token does not
exist yet — it records itself as `-`. So the login handler audits by name on both
outcomes: `<user> logged in`, and `<user> login REFUSED (invalid username or password)`.
Never the password, and never which half was wrong.

## Consequences

- Every mutating request is attributable, including the refused ones.
- **The audit line's shape changed**, from `<method> <path>` to `<who> <method> <path>`.
  Anything parsing it positionally sees a new first field. This is a deliberate,
  one-time break rather than a second parallel line, which would have been two records of
  one event — the failure One Source of Truth exists to prevent.
- `g_req_user` is file-scope, matching `g_slot` and `g_bind_addr`, because this daemon
  dispatches one request at a time on a single epoll loop. It is cleared at the top of
  every dispatch so a value cannot leak from one request onto the next. **If dispatch
  ever becomes concurrent this is wrong**, and it is stated here rather than left for
  someone to discover.
- Handlers can now record who: what the approval gate in the next change needs, and what
  makes it more than decoration.
- `test_hostauth` (in `SELFTESTS`) asserts all three end to end against the real log
  store — an attributed write, an attributed login, and an audited refusal — rather than
  checking a format string, because the value only exists if it survives dispatch, the
  gate and the store.

## Alternatives considered

**Add the username to a second, parallel audit line.** Rejected: two records of one
event, which drift.

**Give `hostauth_authorize_write()` an out-parameter instead.** Reasonable, and it
already computes the name. Rejected because the gate runs *after* the audit line, so it
cannot attribute a refusal — and moving the audit below the gate is the thing this
decision explicitly does not want.

**Record the token rather than the username.** Rejected outright: a raw token is a
credential, and the store is readable over the API.
