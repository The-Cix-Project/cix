# 0325 — A login survives a restart

## Status

Accepted, 2026-10-02. This fixes #562, which the owner reported as *"after adding rbac, the web ui has become flaky ... Logged out, it's not consistent, and does not inspire confidence when I compare it to proxmox's webUI"*. The owner put it next in line after ADR-0324 step C. It amends the in-memory session table that ADR-0144 (host authentication) and ADR-0317 (permissions) built on.

## Context

`hostauth.c` kept its sessions in a 64-slot array that `hostauth_init()` cleared, so every cixd restart signed out every browser and every cixctl at once. Before ADR-0317 almost nothing needed a login, and a lost session cost one prompt on the next write. Since ADR-0317 every read needs one too. A lost session now empties every panel, while the dashboard header still says "logged in: <user>".

Measured on 192.168.15.95, 2026-10-02:

- A token answered `GET /v1/whoami` with `"authenticated":true`.
- After `cixctl reboot` into 0.2.57-441, the same token got `{"authenticated":false,"username":null,"permissions":[]}`.

That day deployed six releases, each with a reboot. A rolling-release platform restarts its daemon routinely, so its sessions have to survive that routine.

## Decision

**The session table is persisted, and a token is never stored.**

- Sessions are saved to `hostauth_sessions.json`, beside `hostauth_config.json` in the state directory, with mode 0600.
- Each entry holds `sha256(token)`, the user, the expiry, the login time and the source address. A request's token is hashed and compared against that.
- The raw token appears in exactly one place, the login response, as before. Reading the file gives nobody a session: the token is 192 random bits, so its hash cannot be reversed.
- At init, `hostauth_init()` reloads the file after the config, because the idle timeout decides what has lapsed. It drops every session whose window ran out while the daemon was down. A restart never extends a session.
- The file is written:
  - on login, logout and revoke;
  - when a single-use session is consumed;
  - when a session's sliding window has moved at least 60 seconds past what the file records.

  So it is written at most about once a minute per active session, not on every request. A crash can shorten a live session's window by at most 60 seconds, and can never lengthen one.
- The file is not part of a system backup. A restored host starts with no sessions, as a new host does.

**The dashboard shows the session it really has.**

- When a request refuses the session the page holds (a 401), the page drops its token at once. The header, the Log in button and the login-required banner then agree with the daemon. No modal opens on a background poll; that is #490's rule and it stands.

## Alternatives considered

- **Stateless signed tickets, as Proxmox issues them** (an HMAC over user and time, valid for a fixed lifetime, nothing stored). Rejected because this host already has per-session state that tickets would lose:
  - `GET /v1/system/hostauth/sessions` lists who is logged in;
  - revoke signs a user out everywhere;
  - single-use sessions (`idle_timeout_seconds: 0`) are consumed on first use.

  Keeping those with tickets needs a denylist and a used-ticket list. That is a second mechanism beside the table, not a replacement for it.
- **Persisting on every request.** Rejected: a disk write per API call buys at most 60 seconds of window after a crash.
- **Storing the raw token, mode 0600.** Rejected: hashing costs one digest per lookup, and it keeps the state directory from being a credential store.

## Consequences

- A release deployed, or a reboot, no longer signs anyone out of the dashboard or cixctl.
- The 64-slot limit now spans restarts. Lapsed sessions are dropped at load and reaped at login as before, so the table holds only live sessions.
- `pkicrypto_sha256_hex()` is the in-memory sha256 helper. Hashing a buffer by writing it to a file first was the existing pattern elsewhere, and it would have put the token on disk.
