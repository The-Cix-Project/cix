# 0164 — CLI prompt shows the full site FQDN and a real auth-state indicator

## Status

Accepted

## Context

`thincctl`'s interactive shell prompt showed only the connected daemon's bare `instance_name` (`myhost> `, ADR-0132) — never the full site identity, and never anything about whether the current session was actually authenticated. The user asked directly for the standard convention: `host.site.tld` with `>` unauthenticated, `#` authenticated (the same convention real shells and network-device CLIs already use for privilege level).

The auth-state half isn't free: write-gating (ADR-0144) deliberately leaves every ordinary GET open regardless of auth state, so there was no existing way for a client to answer "is my token currently valid" without attempting a real mutation and seeing whether it 401s. A dedicated introspection endpoint was needed first, per the API-First Mandate.

## Decision

- **`GET /whoami`** (new): `{"authenticated": bool, "username": string|null}`. Deliberately **read-only in a stronger sense than an ordinary GET** — it uses a new `hostauth_peek_token()`, not the existing `hostauth_check_token()`, because the latter mutates a session as a side effect of checking it (refreshes the sliding idle window on success; under a single-use config, `idle_timeout_seconds: 0`, it *consumes* the token outright, regardless of outcome). A prompt-refresh check calling the mutating version would silently invalidate the single-use token a user just logged in with, before they ever got to use it for a real write. `hostauth_peek_token()` looks up the same session table with no side effects at all.
- The CLI prompt becomes `<instance>.<site>.<domain>` (or `<instance>.<domain>` with no site tier set — mirrors `siteconfig_host_fqdn()`, reimplemented client-side since the CLI has no access to daemon-internal functions) followed by `#` when `GET /whoami` reports `authenticated: true`, `>` otherwise — including whenever write-gating isn't active at all yet (a fresh install), the same as an unprivileged prompt on a box with no root password set.
- Refreshed immediately after a `login` or `logout` command (checked by name in both shell loop variants), not just once at shell startup, so the indicator reflects a state change within the same session rather than only on the next `thincctl` invocation.

## A real bug found (and fixed) while building this

Verifying the `#` transition surfaced a genuine, previously-invisible bug: `cmd_login()` persisted the new token to `~/.thincctl_token` (so the *next* `thincctl` invocation would pick it up at startup) but never updated the *current* process's own in-memory `kx_client`. Every command run for the rest of that interactive session — including this new prompt's own `whoami` check — kept using whatever (possibly empty) token the process started with. A user who ran `login` mid-session saw "Logged in as X" and reasonably believed it, while every subsequent write in that same session still silently failed as unauthenticated. `cmd_logout()` had the mirror-image gap for clearing the token. Both now also call `kx_client_set_token()` on the live client object (a deliberate, commented `const`-cast — the object's real underlying storage is always `main()`'s own non-const `client`; the `const` in every `cmd_*()` signature is routing-only, not a property of the memory itself).

This was never caught before because nothing previously displayed live client-side auth state within a session — the bug was real but silent.

## Consequences

- A second, non-mutating token-check path (`hostauth_peek_token()`) now exists alongside the original (`hostauth_check_token()`). Any future introspection-style feature should reach for the peek variant, not the mutating one — same reasoning as this ADR, not re-derived each time.
- The prompt now makes two extra HTTP round-trips at shell startup (site + whoami) and two more per `login`/`logout` — negligible against a local/LAN daemon, not measured against a high-latency link.
- Mid-session `login`/`logout` now genuinely change the live session's own auth state immediately, closing a real gap that existed silently before this ADR's own verification work found it.
