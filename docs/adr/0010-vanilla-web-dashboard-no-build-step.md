# 0010 — Vanilla HTML/CSS/JS dashboard, no framework, no build step, served by thincd

## Status

Accepted

## Context

Phase 5 required a web dashboard, constrained the same way as the CLI (ADR-0005): a pure client of `docs/api/openapi.yaml`, no capability that isn't already a REST endpoint. Two real questions had to be settled: what the frontend is written in/with, and who serves its static files (HTML/CSS/JS) to the browser.

## Decision

**Frontend stack:** plain HTML, CSS, and JavaScript, with no framework (React/Vue/etc.) and no build step (no npm/webpack/bundler). This project has had zero build tooling beyond `tcc`/`make` through four prior phases, all hand-rolled rather than dependency-pulling (custom HTTP/JSON in the daemon — ADR-0007 — custom HTTP client for the CLI — Phase 4). Introducing a Node/npm toolchain now, for one small dashboard, would be a new and disproportionate dependency category, and arguably its own kind of parallel-implementation-of-tooling problem. A web dashboard is inherently browser-rendered HTML/CSS/JS — there's no way around that — but nothing requires a framework or a compile step on top of it.

**Serving:** `thincd` itself serves the dashboard's static files (`daemon/src/staticfile.c`, `--web-root` flag, default `web/`), same origin as the API (`127.0.0.1:7620`). Confirmed with the user directly (the alternative — a separate static-file process — was considered and rejected): a different origin would mean the dashboard's `fetch()` calls to `/v1/...` become cross-origin, requiring CORS preflight (`OPTIONS`) handling and `Access-Control-Allow-Origin` headers in the daemon for zero functional benefit. Same-origin serving sidesteps that class of complexity entirely.

## Consequences

- Every future dashboard feature follows the same pattern: plain JS calling `fetch()` against `/v1/...`, no new tooling introduced to "make the frontend nicer." If that assumption is later revisited (e.g. the dashboard grows enough to want real componentization), it should be a new ADR, not a silent drift.
- `thincd` now serves two related but distinct things (the API and the dashboard's assets) from one process. Accepted as the simpler, lower-complexity answer given they're used together and always co-located; if a future requirement ever needs the dashboard served from a genuinely different origin (e.g. a CDN), that reopens the CORS question deliberately rather than by default.
- No automated test can verify the dashboard *renders and behaves* correctly (no headless-browser tooling exists in this project, and adding one for this alone would repeat the exact dependency-cost trade-off this ADR just decided against). `test/test_web.c` verifies the HTTP-level contract (correct status/content-type/path-traversal-rejection); actual browser behavior is a manual check, stated as such in `docs/roadmap/ROADMAP.md` Phase 5 rather than silently assumed.
