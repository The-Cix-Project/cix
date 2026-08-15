# 0152 — host-auth session listing and per-user revocation

## Status

Accepted

## Context

User-asked, directly: where do host-auth tokens live, can they be listed (which account issued one, when it was last used), do they survive a restart, and can they be seen from the CLI or web UI? Investigated directly rather than assumed: `g_sessions[HOSTAUTH_SESSION_MAX]` (`daemon/src/hostauth.c`) is a plain in-memory array, reset to zero at every daemon startup (`hostauth_init()`) — tokens genuinely do not survive a restart, which is why every reboot this session required a fresh login. More significantly: **there was no way to see this table at all** — no REST endpoint, no CLI command, no web panel. Each session record carries only `token`, `username`, `expires_at` — no separate "issued at" or "last used at" field (the sliding-window `expires_at` update on each authenticated request is the closest thing).

Confirmed with the user this was worth closing: "yes we want to build that... with all the bells and whistles."

## Decision

**Listing** (`GET /v1/system/hostauth/sessions`): `{"sessions":[{"username":...,"expires_in_seconds":<int or null>}, ...]}`. Deliberately excludes the raw token — it is never shown again after its one-time appearance in the `POST /v1/login` response, matching how bearer tokens are conventionally handled everywhere else. `expires_in_seconds` is `null` specifically for the `idle_timeout_seconds == 0` configuration (every token single-use, consumed on next check) — there is no meaningful "expires in" for a session valid for exactly one more request, whenever that happens to be, so reporting a number there would be misleading rather than merely imprecise. An expired-but-not-yet-opportunistically-reaped entry is skipped, matching what "active" already means everywhere else in this code (`hostauth_check_token()`'s own reaping).

**Revocation is per-username, not per-session** (`DELETE /v1/system/hostauth/sessions/{username}`, revokes every active session for that user at once): the real design question this part turned on. A per-session revoke needs a stable, safe identifier for one specific session — but the only genuine per-session identifier is the raw token itself, which is never surfaced past its one-time login response by design (surfacing even a truncated/hashed form for this purpose would be a new, narrow crypto surface for a feature that doesn't need it). "Log this account out everywhere" is also the actually-useful admin action in practice (a compromised or departing account, a stuck client holding a stale session) — a hypothetical "log out just this one browser tab" has no real use case this project has ever needed, and per-username revocation gets the real one for free with no new primitive.

**CLI**: `thincctl hostauth-sessions ls`, `thincctl hostauth-sessions revoke USERNAME` — a new top-level command, flat-named to match `hostauth-config`'s own existing convention (not nested under a `hostauth <verb>` group, since there's no other `hostauth` noun with subcommands to group alongside).

**Web**: a new `Sessions` page under `Host` (`SYSTEM > Host > Sessions`), a table (username, expires-in) with a "Log out everywhere" button per row.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. New coverage in `test/test_hostauth.c`: the active session's real `expires_in_seconds` shows correctly (and no raw token anywhere in the response); revoking a user immediately invalidates its existing token (the next authenticated write with that token gets 401, same as a garbage token); revoking a user with no active session is a real no-op (204, not an error), matching `hostauth_logout()`'s own idempotent posture. **A real off-by-one bug was caught by this test and fixed before it ever shipped**: the route-matching `strncmp()`/pointer-arithmetic for `/v1/system/hostauth/sessions/{username}` used a hardcoded prefix length one byte too long (30 instead of the real 29-character `"/v1/system/hostauth/sessions/"`), so the revoke endpoint 404'd unconditionally until the test caught it. Full regression sweep (daemon/CLI/web-adjacent tests) clean afterward.

The web panel was verified against a real running daemon via the Chrome DevTools Protocol directly (`puppeteer-core` unavailable in this sandbox): logged in through the actual login form (not a raw API call), confirmed the resulting session appears with a real, live `expires_in_seconds`, clicked "Log out everywhere," and confirmed via the daemon's own audit log (visible in the dashboard's log panel) that a real `DELETE /v1/system/hostauth/sessions/osakka -> 204` fired and the table emptied — a genuine end-to-end proof, not just a rendering check.

## Consequences

- Admin visibility into host-auth sessions goes from zero to real: who's logged in, for how much longer, and a way to force a logout, all previously invisible and un-actionable.
- The per-username (not per-session) revoke scope is a real, permanent design boundary: an operator cannot target "just this one browser tab" without first building a genuine per-session identifier this project has deliberately chosen not to expose. If a real need for that surfaces later, it is a new decision, not an oversight in this one.
- Sessions still do not survive a daemon restart (unchanged, out of this ADR's scope) — the listing/revoke surface only ever shows what's currently live in memory.
