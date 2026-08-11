# 0132 — Log panel fixed size/perf fixes, dynamic shell prompt, ps/process style unification

## Status

Accepted

## Context

Real-world feedback after the logging/web-UI epic (ADR-0126 through ADR-0131) shipped and was used live:

1. The bottom log panel grew with content instead of staying a fixed size.
2. The web dashboard felt noticeably slower after the merged log panel (ADR-0129) shipped.
3. `kanxeoctl`'s interactive shell prompt was a fixed literal `"kanxeo> "`, not reflecting which box a session was actually talking to.
4. `process ls`'s output didn't match `ps`'s own established visual style.
5. No noun-based `container` command existed, unlike every other resource (`dns`, `ldap`, `ntp`, `syslog`, `network`, `image`, `devicemap`, `pkg`, `pki`), which all follow a `<noun> <verb>` shape.

## Decision

**(1)+(2) were the same root cause, investigated together, not patched independently.** `#log-output` (the scrollable content area inside the fixed-200px `#log-panel`) never had `min-height: 0` set — a classic flexbox gotcha: a flex item's default `min-height` is `auto` (its own content size), not `0`, so once enough `.log-entry` divs accumulated, `#log-output`'s own intrinsic height grew past `#log-panel`'s fixed flex-basis and visually spilled out of it rather than scrolling internally, since `#log-panel` also had no `overflow: hidden` of its own to backstop that. Fixed with both: `min-height: 0` on `#log-output` (the real fix — makes it actually respect `flex: 1 1 auto` and let `overflow-y: auto` do its job) and `overflow: hidden` on `#log-panel` (a defensive backstop, redundant once the first fix lands, but cheap insurance against the same class of bug recurring).

That alone doesn't explain a *slower dashboard*, though — a second, real bug, found by reading `pollServerLogs()`'s own render path rather than guessed: every single log entry rendered called `logOutput.scrollTop = logOutput.scrollHeight`, which forces a synchronous browser reflow. The web dashboard's own routine 2s poll cycle (`poll()`) issues roughly 30 `GET` requests every cycle to keep its cache fresh — once `GET /v1/system/logs` itself became one of those 30 (ADR-0129) and its own results got rendered, **every one of those ~30 routine polling GETs was also landing in the `audit` log source** (`dispatch()`'s own audit-log call previously excluded only `GET /v1/health`, the same reasoning that already existed for that one endpoint applying, un-generalized, to every other routine `GET`) — meaning each 2s poll cycle could easily surface dozens of new entries, each forcing its own synchronous reflow. Fixed two ways, together:
- **Client**: `pollServerLogs()` now batches — builds a `DocumentFragment` for the whole batch and does exactly one `appendChild()`/scroll-trim per poll cycle, not one per entry (`addLogEntriesBatch()`, new; `addLogEntry()` kept for the rare single real-time entry — a toast or one `web-ui` action — where one reflow per call is genuinely fine).
- **Daemon**: `dispatch()`'s audit-log exclusion generalized from "`GET /v1/health` only" to **every `GET`** — a `GET` is a query, never a mutation, and the original exclusion's own stated rationale ("routine polling... would drown every real action in noise for no diagnostic value") already covered this case, it just hadn't been generalized when only `/health` was polled routinely. `POST`/`PUT`/`DELETE` (the real, auditable actions) are completely unaffected — confirmed no existing test asserts on `GET` requests appearing in the audit log before making this change.

**(3)**: `SHELL_PROMPT` (a compile-time `#define`) became a runtime buffer (`g_shell_prompt`), populated once at shell startup (`shell_prompt_init()`) from `GET /v1/system/site`'s own `instance_name` — falling back to the literal `"kanxeo> "` (site config's own documented default) if the fetch fails for any reason, so the prompt is never left blank. A pure REST client fetching the *daemon's own* declared identity, not `uname()` on the CLI's own local host (which would show the wrong thing entirely — the machine running `kanxeoctl`, not the box it's talking to) — consistent with the API-First Mandate.

**(4)**: `process ls`'s formatter rewritten to match `fmt_container_line()`'s (`ps`'s own) exact visual convention — bare leading identity columns (`pid`, `comm`), then `key=value` pairs, one line per entry, no header row — instead of a fixed-width table-with-header style that was visually inconsistent with every other list command in this CLI.

**(5)**: A new `container ls` command, a pure synonym for `ps` (identical `GET /v1/containers`, identical output) — added, not a replacement. `ps`/`run`/`stop`/`start`/`rm`/`inspect`/`stats`/`console` stay exactly as they are: Docker-familiar short verbs for the container lifecycle operations are their own real usability value for anyone coming from that ecosystem, not something worth disrupting. `container ls` exists purely to give the noun-based-command habit (already the right instinct for every *other* resource in this CLI) a working entry point for "what's provisioned and what state is it in," without forcing a much larger, disruptive rename of already-established, heavily-documented verbs.

## Verification

Full clean rebuild (`-Wall -Werror`), zero warnings. Full regression sweep (38 test binaries) confirms zero regressions — none reference `source=audit` content in a way the `GET`-exclusion change could break. `node --check web/app.js`. Manual live checks: a real PTY-driven interactive shell session (Python's `pty` module, since this sandbox has no way to drive a real terminal otherwise) confirms the prompt reads `<instance_name>> ` correctly after `site set --instance-name=...`; `process ls`/`container ls` both produce correctly-styled, correct output against a real scratch daemon.

## Consequences

- The audit log's own real scope is now exactly "every mutation this daemon ever received" — arguably a *more* useful definition than before (a pure action log, not diluted by routine polling noise), not just a perf fix dressed up as one.
- `container ls` and `ps` are now two equally-valid ways to reach the same information — a small, deliberate surface-area increase, justified by matching this CLI's own established `<noun> <verb>` convention for every other resource.
