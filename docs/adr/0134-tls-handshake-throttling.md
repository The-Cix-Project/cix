# 0134 — Per-source-IP throttling for repeated failed HTTPS handshakes

## Status

Accepted

## Context

Found live on 192.168.15.95, watched directly on the physical/serial console: a sustained flood of `thincd: https handshake failed: sslv3 alert certificate unknown` — over 500 occurrences per minute, ongoing. Two real gaps, reported by the user in the same conversation:

1. **No peer IP.** `log_tls_error()` (added under ADR-0126 specifically to stop OpenSSL's own raw error-queue dump from spamming the physical console on every failed handshake) logged only `"<context>: <reason>"` — nothing identifying which client this was. `GET /system/logs` showed the flood clearly but gave no way to act on it.
2. **No way to stop it.** Every failed attempt still cost a real `accept4()` + `SSL_new()` + `SSL_accept()` — and since this daemon deliberately never does HTTP keep-alive (`Connection: close` on every response, `docs/api/README.md`'s own stated v1 scope boundary), each one is a brand-new TCP+TLS connection. A client stuck in a reconnect loop, or a TLS/cipher scanner (`nmap --script ssl-enum-ciphers`, `sslyze`, `testssl.sh` all send many rapid handshakes by design), turns into hundreds of real per-connection allocations a minute with no cost to the source and no way for the operator to make it stop short of firewalling the port from outside this daemon entirely.

The user asked for the peer IP to be logged, and for throttling — explicitly requiring it be "API driven and configurable," consistent with this project's own API-First Mandate (every capability exists as a REST endpoint first, CLI/web second).

## Decision

**`accept4()` now captures the real peer address** (`daemon/src/main.c`'s `accept_loop()`, previously passed `NULL` for the sockaddr out-param, discarding it) and stores it on `struct conn` (`peer_ip`, `CONN_CLIENT` only). `log_tls_error()` takes it as a second argument and includes it in the log line.

**A new module, `daemon/src/connthrottle.c`/`connthrottle.h`**, tracks per-source-IP failure counts in a fixed 256-entry in-memory table (not persisted — the same "an in-flight thing the daemon restarts through is simply lost" posture every other transient daemon state already has, e.g. `g_iso_build_state`). `connthrottle_record_failure(ip)` is called only from `client_conn_advance_handshake()`'s real handshake-failure path (a local resource failure — `SSL_new()`/`SSL_set_fd()` erroring — is logged with the peer IP for context but never counted against it, since it isn't the peer's own doing). A source that reaches `threshold` failures within a rolling `window_seconds` window is blocked for `block_seconds`.

**Enforcement is a single check in `accept_loop()`**, applied uniformly to *both* listeners (plain HTTP and HTTPS) at the earliest possible point — right after `accept4()` returns the peer address, before any `malloc()` or `SSL_new()`: `connthrottle_should_block(peer_ip)` just closes the fd if blocked. One shared table, not two, and a source flooding one listener is a reasonable thing to also stop bothering with on the other — this project's own "No Parallel Implementations" maxim applied to enforcement, not just code.

**A clean request resets the count.** `connthrottle_record_success(ip)` fires on a successful TLS handshake, and — after a real design iteration during testing surfaced the gap directly — also on any complete, well-formed HTTP request on either listener (`handle_client_event()`, right after `http_conn_try_parse()` returns a complete request, regardless of what it dispatches to). Without this, a source that behaved well on one listener could still be one stale failure away from a block that its own good behavior never gets credit for.

**Loopback (`127.0.0.1`) is never throttled or tracked, full stop** — the single most important correctness decision here, found the hard way while writing this feature's own test: `thincctl`'s own default `--host=` is `127.0.0.1`, and since enforcement applies uniformly across both listeners, a block tripped from loopback would lock out this daemon's own local admin access entirely — the same class of hazard as a firewall rule that can shut out its own operator. A genuinely hostile source is, by definition, never loopback; exempting it costs nothing real.

**API-driven and configurable, per the user's own explicit requirement**: `GET`/`PUT /v1/system/tls-throttle` (partial update, same convention `daemon-config`/`pkg/repo-config` already established — `enabled`/`threshold`/`window_seconds`/`block_seconds`, persisted via `persist.h`'s existing atomic-rewrite helper) and `GET /v1/system/tls-throttle/status` (live, read-only list of every currently-tracked source). `thincctl tls-throttle show|set|status` and a matching System > Server > TLS Throttle web dashboard page (config form + live status table) both follow, closing the loop end to end.

Defaults: `enabled=true`, `threshold=20` failures, `window_seconds=60`, `block_seconds=300` (5 minutes) — trips within ~2-3 seconds of a flood at the observed real-world rate (~8-11/sec) while tolerating a handful of incidental failures from a legitimate client without being trigger-happy.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. New `test/test_tls_throttle.c` proves, against a real daemon (bound to `0.0.0.0` so a second loopback address, `127.0.0.2`, can reach it as a genuinely distinct source — no real second host needed, `127.0.0.0/8` routes locally on Linux by default): config GET/PUT round-trips and validates its fields (an out-of-range value is rejected without silently applying the other, in-range ones); real malformed-handshake bytes sent to the HTTPS listener trip a real block after `threshold` failures; the blocked source's own peer IP appears in `GET /system/logs`; `GET /system/tls-throttle/status` shows it tracked and blocked; the block refuses the plain HTTP listener too, while loopback stays completely unaffected throughout; the block expires on its own after `block_seconds`; a clean request resets the failure count; and `enabled=false` genuinely disables enforcement even under a sustained flood. Full regression sweep (39 test binaries) confirms zero regressions — every existing daemon-linked test now exercises the new `accept_loop()`/`log_tls_error()` code paths on every connection it makes, incidentally proving them harmless for the overwhelmingly common "not currently throttled" case too.

Web dashboard verified with a real headless-browser session (Chromium + Puppeteer): the TLS Throttle page renders correctly under System > Server, the config form loads real values from the API (not placeholders), and a real click on Save round-trips a changed `threshold` through `PUT /v1/system/tls-throttle` and back.

## Consequences

- A real, previously-undiscovered operational blind spot (who is hitting the HTTPS port, and how often) is closed, with a real mitigation available, not just better logging.
- The 256-entry tracking table is a fixed, accepted limit — more simultaneously distinct failing sources than that in one window is an extreme case; the table simply stops tracking new ones until existing entries age out (window elapses) or their blocks expire, rather than evicting an active block to make room.
- Per-connection log lines elsewhere in this daemon (the audit trail, container lifecycle events) still don't carry a peer IP — closed here for the one case that was actually flooding a real deployment's logs, not generalized to every log line this daemon writes.

*(One claim revisited, found live on 192.168.15.95: [ADR-0137](0137-tls-throttle-block-scoped-to-https-listener.md) changed enforcement — "applied uniformly to both listeners," in the Decision section above — to HTTPS-only, since only a failed HTTPS handshake can ever cause a block and enforcing it against plain HTTP as well was found to strand a legitimate fallback path for a source whose only real problem was an untrusted certificate. Every other decision here — per-source tracking, the threshold/window/block-duration model, the loopback exemption, API-driven configurability — remains exactly as decided and accurate; see ADR-0137 for the one line that changed and why.)*
