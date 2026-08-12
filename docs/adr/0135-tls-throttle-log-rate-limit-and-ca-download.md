# 0135 — ADR-0134 follow-up: rate-limited handshake-failure logging, and a CA download/trust workflow

## Status

Accepted

## Context

Direct follow-up to ADR-0134, same day: with peer-IP logging live on 192.168.15.95, the flood's real source was identified as `192.168.10.54` — the user's own desktop. Not a hostile scanner at all: the desktop's browser had the web dashboard open over HTTPS and didn't trust this install's self-signed root CA, so every one of the dashboard's own 2-second poll requests failed TLS (this daemon never does HTTP keep-alive, so each poll is a fresh handshake) — over 800/minute at points, self-resetting below the block threshold via ADR-0134's own "a clean request forgives past failures" design (the browser's occasional successful requests, e.g. to already-cached resources, kept resetting the count).

Two real, distinct problems surfaced by this, neither actually about a hostile source:

1. **Log volume.** ADR-0134 logs every single failed handshake. A legitimate-but-untrusted client floods the log store just as effectively as a hostile one would, and does so *before* ever accumulating enough failures to justify a block — the block threshold and the logging were never actually meant to be the same knob.
2. **No easy way to fix the actual cause.** The daemon already returns the CA certificate (`GET /pki/ca`'s `cert_pem`), but there was no discoverable, guided path from "I'm seeing this error" to "my browser now trusts this install."

## Decision

**Rate-limited failure logging, decoupled from the block threshold.** `connthrottle.c` gains a second, independent per-source cooldown (`log_interval_seconds`, default 5) — at most one `logstore_write()` per source per interval, regardless of real attempt volume. Implemented by extending the *same* per-IP tracking table ADR-0134 already built (`last_logged` alongside `fail_count`/`blocked_until` — one source of truth for "have we seen this IP," not two tables), via a new `connthrottle_should_log_failure()` that both `client_conn_advance_handshake()`'s two call sites check before writing a line. Crucially, `connthrottle_record_failure()` (the count that actually decides blocking) is called unconditionally either way — only the *log write* is suppressed, never the accounting, and `log_tls_error()` still always drains the full OpenSSL error queue even when the line itself is suppressed, so errors can never silently accumulate there. Independent of whether throttling is even `enabled` — the log-noise problem exists regardless of whether blocking is active, so this uses its own always-on tracking-entry lifecycle rather than piggybacking on the enabled-gated one `connthrottle_record_failure()` uses.

**A CA download button and trust instructions, not a new endpoint.** The web dashboard's PKI > Root CA and PKI > Intermediate CA pages gain a "Download certificate (.pem)" button once each is bootstrapped — a pure client-side `Blob`/anchor-`download` save-as over `cert_pem`, a field `GET /pki/ca`/`GET /pki/intermediate` already return. No new REST endpoint: the API-First Mandate is about the *capability* existing at the API layer first, and "get the CA cert" already does — turning already-fetched JSON into a downloadable file is presentation, not a new capability. Platform-specific trust instructions (Windows/macOS/Linux/Firefox all differ meaningfully — OS-level trust stores vs. Firefox's own independent one) live in a new `docs/guides/security.md` section, linked from a short in-app hint rather than duplicated inline.

## Consequences

- The throttle-bypass behavior ADR-0134 itself worried about (a source alternating just enough successes to never trip the threshold) turned out, in the one real case observed, to be a symptom of a legitimate client failing repeatedly for an unrelated reason — not an actual gap in the blocking logic. No change to the block/reset behavior itself was made or judged necessary; fixing the root cause (browser doesn't trust the CA) is the real fix, not a stricter throttle.
- `kanxeoctl tls-throttle show`/`set --log-interval-seconds=N` and the web dashboard's TLS Throttle page both gained the new field, same partial-update convention as every other field on this resource.
- Verified: `test/test_tls_throttle.c` extended — a burst of 3 deliberately-triggered handshake failures (well under a second, real time) now asserts exactly 1 log line, not 3, with `log_interval_seconds=10` covering the whole burst; the pre-existing block-trip assertion (checking the real failure *count*, not the log) is unaffected, confirming logging and accounting are genuinely decoupled. Full clean rebuild (`-Wall -Werror`, zero warnings) + full regression sweep confirm zero regressions. Web dashboard PKI download button verified with a real headless-browser click producing a real downloaded file matching the API's own `cert_pem`.
