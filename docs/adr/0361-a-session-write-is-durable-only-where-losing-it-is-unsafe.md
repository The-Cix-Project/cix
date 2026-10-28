# 0361 — A session write is durable only where losing it is unsafe

## Status

Proposed, 2026-10-28, for #659. It narrows the persistence clause of [ADR-0325](0325-a-login-survives-a-restart.md): a login still survives a restart, and what a power loss may cost is restated.

## Context

[ADR-0325](0325-a-login-survives-a-restart.md) persists the session table to `hostauth_sessions.json` so that a login survives a restart. `save_sessions()` writes it with `persist_atomic_write()`, which writes a temporary file, fsyncs it and renames it over the table, on the event loop. It is called on login, on logout and revoke, when a single-use session is consumed, when a sliding window has moved 60 s past what the file records, and when the expiry reaper drops a lapsed session.

Measured on 192.168.15.95:

- `POST /v1/login` held the loop 1422 ms at ts 1793148654 (2026-10-28), "most in session table write (1421 ms, 1 calls)". That is the stallwatch tally #646 added.
- The `session-expiry-timer` held it 2700 ms, 984 ms and 959 ms on 2026-10-26. That timer's only write is the reaper's.

Why a single fsync took 1.4 s is not measured. Contention with a build is a guess: the builds write sda, the table is on vdb4, and whether those share a backing store is not visible from the guest.

Read from the code, `hostauth.c`:

- `load_sessions()` already drops every session whose `expires_at` has passed ("lapsed while cixd was down"). So the reaper's writes, and the one made when a lookup finds an expired session, change nothing that a later start reads.
- A rename is atomic. Without an fsync, the new table survives a daemon crash or restart, and it survives a clean reboot, because unmounting flushes it. Only a kernel crash or a power loss can lose it.

## Decision

1. **Dropping an expired session writes nothing.** Neither the reaper nor a lookup that finds an expired session writes the table. The next write of the table leaves the lapsed session out, as `save_sessions()` already does, and a start ignores it, as `load_sessions()` already does.
2. **A login and a sliding refresh write the table without fsync.** It is still written to a temporary file and renamed. A power loss may then lose a login, and the user signs in again, or a refresh, and the window is up to 60 s shorter. Neither can lengthen a session or bring one back.
3. **Logout, revoke and consuming a single-use session write it durably, with fsync, as now.** Losing one of those to a power loss would bring back a session that was ended deliberately or a token that was already used. That is a security property, so it costs the fsync. These are operator actions and rare.
4. **One writer, one choice.** `save_sessions()` takes whether the write must be durable, and the no-fsync form is a separate persist function named for the contract it keeps. It has this one caller, with this reason.

## Alternatives

- **Write in a helper child, still with fsync** (ADR-0278's pattern, coalescing latest-wins). The loop would no longer wait, but it would keep an fsync whose only effect on login and refresh is surviving a power loss, and it adds a second state of the table, written but not yet durable, which a revoke racing a login would have to order. The question this ADR asks first is whether the fsync should exist at all.
- **Drop the fsync everywhere.** That would let a power loss bring back a revoked session or a used single-use token. Refused.
- **Keep everything and accept the stall.** It is the control plane not answering for over a second, measured, while something else uses the disk.

## Consequences

- The expiry timer no longer touches the disk, and a login or a dashboard's refresh no longer waits for one.
- After a power loss, a session started or refreshed in the last moments before it may be gone or up to 60 s shorter. ADR-0325's "a crash can shorten a live session's window by at most 60 seconds" becomes "a crash or power loss can lose a login made just before it, or shorten a window by at most 60 seconds; it can never lengthen one or bring an ended one back".
- #646's other half, the announcement self-check's in-process reads, is not addressed here.
