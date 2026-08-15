# 0116 — `json_parse()` gains narrow `\uXXXX` support, closing a write/parse asymmetry that silently broke `thincctl ps`

## Status

Accepted

## Context

Found live during task #760's full feature regression sweep on 192.168.15.95: `thincctl ps` (and `thincctl --json ps`) returned nothing against a real, healthy box with `ldap-1`/`ldap-2` running — no error, just silence, `--json` printing a bare `null`. `curl`'s own raw fetch of the same `GET /v1/containers` endpoint returned a complete, valid 7009-byte JSON document (confirmed independently parseable by Python's own `json` module).

Root cause: `daemon/src/json.c`'s `jw_escaped_string()` (the writer half of this project's own hand-rolled JSON support) has always encoded any control character below `0x20` as a `\u00XX` escape, to stay valid JSON (a raw, unescaped control byte inside a JSON string is not legal per RFC 8259). `parse_string_raw()` (the parser half of the *same file*) rejected every `\u` escape outright, `free()`-ing its buffer and returning `NULL` the instant it hit one — and because `json_parse()` has no partial-success mode, that single rejected string failed the *entire* surrounding document, regardless of how much of it was otherwise well-formed.

This asymmetry has existed since `json.c` was first written, deliberately: `json.h`'s own header comment stated "`\uXXXX` escapes are an explicit, documented scope boundary... since no field in this project's API needs one." That was true when written. It stopped being true the moment `capture_output` (ADR-0112, task #761) started relaying a container's *real* stdout/stderr verbatim into the `captured_output` JSON field — and `ldap-1`/`ldap-2` run `glauth`, whose `zerolog`-based logging colorizes its own terminal output with real ANSI escape codes (`ESC` = `0x1b`), exactly the class of byte the writer has always had to `\u`-escape. Every `GET /v1/containers` call against a box with a colorized program's `captured_output` populated has been silently failing to parse client-side ever since — this had simply never been exercised live before this sweep, because no earlier live-verification pass happened to include both `capture_output` and a colorized program's output in the same request at once.

## Decision

`parse_string_raw()` gains a `case 'u':` branch: reads exactly 4 hex digits, decodes them as a single byte (`0x00`-`0xFF`), and rejects (fails the parse, same as before) any value above `0xFF` or malformed hex — deliberately still narrower than full RFC 8259 (no UTF-16 surrogate-pair handling), matching what the writer side actually ever produces (`jw_escaped_string()` only emits `\u00XX` for control characters below `0x20`) rather than attempting general Unicode support this project's own API has never needed. `json.h`'s own scope-boundary comment updated to describe the new, narrower-but-real contract instead of claiming `\uXXXX` is unsupported outright.

A real regression test closes the gap that let this ship unnoticed: `test/output_child.c` (the exec target `test_container_lifecycle.c`'s own `capture_output` test, step 10, uses) now writes a real ANSI color escape into its stdout line, and the test asserts the raw `ESC` byte survives the full write-then-parse round trip through the exact same `kx_client_request()`/`json_parse()` path every CLI command uses.

## Verification

Direct reproduction before the fix: extracted the real `GET /v1/containers` response body from 192.168.15.95 into a file, fed it to a throwaway harness linking this project's own `json.c` directly — `json_parse()` returned `NULL` (confirmed the parse genuinely failed, not a client-side red herring), while Python's `json.load()` on the identical bytes succeeded. After the fix: the same harness against the same file reports `PARSE OK`, and `thincctl ps` run locally against the live box on 192.168.15.95 correctly lists all 7 running containers.

Full clean rebuild (`-Wall -Werror`), zero warnings. Full regression sweep under `sudo`/`dangerouslyDisableSandbox`: 23 daemon-linked and host-side tests, including the newly-extended `test_container_lifecycle` (which now genuinely exercises this exact code path and would have failed before the fix), all pass.

## Consequences

- Every future response containing a captured, colorized program's stdout/stderr now parses correctly for every client sharing this `json.c` (`thincctl`, and any future consumer of the same library) — not just this one endpoint; `\u`-escaped control characters can appear in any string field the writer produces.
- The scope boundary is real, not just relaxed to "full support": a `\uXXXX` value above `0xFF` (a genuine multi-byte Unicode codepoint, or a UTF-16 surrogate half) still fails the parse deliberately, since nothing in this project's writer ever emits one and pretending to support them without real UTF-8 re-encoding would be a correctness trap, not a fix.
- This is the first-ever direct, standalone verification of `json.c`'s own round-trip correctness (previously only ever exercised indirectly through whichever fields a given test happened to touch) — the harness pattern used to reproduce this (link `json.c` directly, feed it a captured real response body) is a reusable diagnostic for any future "the daemon's raw response looks fine but the CLI shows nothing" report.
