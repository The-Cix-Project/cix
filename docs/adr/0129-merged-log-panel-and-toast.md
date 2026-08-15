# 0129 — Merged bottom log panel + auto-dismissing toast notifications

## Status

Accepted

## Context

Part 3 of the logging/web-UI epic (Parts 1/2/5: ADR-0126/0127/0128), covering the user's items 3 and 7:

> "let's move the logs themselves (the display) on the web ui to the logs area on the page (bottom), why are they in the tree? Only the configuration stuff should be in the tree/page, right?"

> "The green/red info box that comes on, should be a popup with a disappear timer, and it should be shown in the logs right? these are the web UI logs, web UI logs are per web UI since they are not a system component, so we don't send them back to the server"

Two things already existed, independently, before this part: a Logs page under the tree (System > Server > Logs) with a browsing table/filter form *and* a size-cap config form together, and a separate bottom-right "action log" panel (`#log-panel`) that already logged every mutating REST request the dashboard itself made — client-side only, never sent to the server, exactly matching what the user described as "web UI logs" without that panel having been named that yet. Confirmed via `AskUserQuestion` before implementing (Q2): the new bottom panel merges server logs and web-UI logs into **one filtered stream via a source dropdown** (`kernel`/`thincd`/`audit`/`container`/`web-ui`), not separate tabs — and, reading the code, the cleanest way to deliver that was to extend the panel that already existed rather than build a second one (a "No Parallel Implementations" call: two bottom panels doing overlapping jobs would themselves have been the violation).

## Decision

**One merged buffer, one panel.** `web/app.js`'s existing `#log-panel` (bottom of every page, collapsible, `localStorage`-persisted state — all unchanged) now holds a single `logBuffer` array (capped at `MAX_LOG_ENTRIES`, unchanged from before) fed from two sources:
- **`web-ui`**: every mutating REST call this dashboard itself makes (`logLine()`, unchanged trigger — still gated on `method !== "GET"` inside `apiRequest()`, same "don't drown real activity in routine polling" reasoning as before), plus every toast (see below).
- **`kernel`/`thincd`/`audit`/`container`**: polled in from `GET /v1/system/logs` (`pollServerLogs()`, called once per the existing 2s `poll()` cycle) — starting from page-load time forward (`logsSinceTs` initialized to "now," not `0`), since this panel is a live tail, not a history browser; the dedicated Logs page (below) still exists for that. A `since=`-based incremental fetch with a small same-second dedupe set (`logsSeenAtSinceTs`) avoids both re-fetching the whole store on every poll and re-rendering entries already shown, given the store's own timestamp resolution is whole seconds and `since=` is an inclusive floor.

A new `<select id="log-panel-source">` in the panel's own header (right-aligned via `margin-left: auto`, its own `click` listener stops propagation so it doesn't also toggle the panel's collapse state) filters the *rendered* view; `logBuffer` itself always holds everything up to the cap regardless of the current filter, so switching the dropdown triggers a full re-render from the buffer (`rerenderLogPanel()`) rather than losing history the filter had been hiding.

**The Logs page (tree) now configures, not browses.** `view-logs` keeps only the size-cap form (`GET`/`PUT /v1/system/logs/config`) — the table, filter form (`lf-source`/`lf-level`/`lf-tail`), and their own fetch/render functions (`refreshLogs()`/`renderLogsTable()`) are removed outright, not hidden or left dead. Matches the user's own framing exactly: "only the configuration stuff should be in the tree/page."

**Toast, not a status bar.** `showStatus()`/`clearStatus()` keep the exact same signature and the same target element (`#status`) — only its CSS changed, from normal document flow to `position: fixed` (top-right, a box-shadow, a short fade-in keyframe) with a `setTimeout`-based auto-dismiss (`STATUS_AUTO_DISMISS_MS`, 5s), canceled and restarted if a new toast arrives before the old one would have faded. No caller of `showStatus()`/`clearStatus()` anywhere in this file needed to change. Every toast now also calls `addLogEntry()` with source `"web-ui"` — directly answering "it should be shown in the logs right?" — so a toast that faded before it was read is still recoverable from the log panel.

## Verification

`node --check web/app.js` (no build step exists for this project's own vanilla-JS dashboard, ADR-0010 — this is the closest thing to a compile check). Full clean rebuild (`-Wall -Werror`, zero warnings — no daemon-side code changed in this part) + full regression sweep (37 test binaries, including `test_web.c`) confirm zero regressions. `web/app.js`/`web/index.html` fetched from a real, live scratch daemon and confirmed byte-for-byte identical to the on-disk source (this sandbox has no headless browser to drive real DOM interaction — flagged honestly, same limitation every prior web-dashboard change in this project has already noted).

## Consequences

- One less page an operator needs to navigate to for live activity — the merged panel is visible from anywhere in the dashboard, matching the user's own stated expectation.
- `logBuffer`'s cap (`MAX_LOG_ENTRIES`, 300, shared across every source combined) means a burst on one source (e.g. many container log lines) can push older `web-ui` entries out of the buffer sooner than before, when the two logs were separate and each had its own effective budget — an accepted trade-off of "one stream," not a bug; the server's own full history remains queryable via `GET /v1/system/logs` directly (`thincctl logs`) regardless of what this panel currently holds.
- `pollServerLogs()` adds one more GET to the existing 2s poll cycle — negligible cost, same "one more `await refresh*()` call" pattern every other polled resource in this file already follows.
