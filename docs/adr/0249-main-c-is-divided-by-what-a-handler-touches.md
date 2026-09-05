# 0249 — main.c is divided by what a handler touches, not by what it is named

## Status

Accepted

## Context

`daemon/src/main.c` was 31,284 lines and 852 functions. Its 68 sibling
modules in `daemon/src/` average around 600 lines each, so the file is not
large because this codebase writes large files — it is large because
everything that was never deliberately extracted stayed there.

Measured, it carries the REST handlers for roughly twenty-four subsystems,
each of which already has its own module: `sysctlconfig.c`, `kmod.c`,
`ntp.c`, `dns.c`, `pkg.c` and so on. The mass is handlers — 307 `handle_*`
functions and 276 `op_*` wrappers — not subsystem logic, which had already
been extracted years of work ago.

**One thing kept them there, and it was mechanical rather than architectural.**
Every handler ends by answering the client, and the two functions that do
that — `respond_json()` and `respond_error()` — were `static` in `main.c`.
`respond_error()` alone has 722 call sites. No handler could be moved to any
other translation unit while its most common call was file-local, so nothing
ever was, and the file grew by accretion.

The existing layering is worth stating because it is good and this does not
change it: no subsystem module responds to HTTP at all. They expose domain
functions that return enums, and know nothing about status codes, JSON bodies
or file descriptors. That separation is already right, and moving handlers
*into* those modules would destroy it.

## Decision

**A new layer, `daemon/src/api_<subsystem>.c`, holds the handlers for one
subsystem. It is the only place HTTP and that subsystem meet.**

`daemon/src/apiresp.c` holds `respond_json()`, `respond_error()` and
`http_status_text()`, which is what makes any of it possible. It sits above
`http.c` — deliberately, because `http.c` is JSON-agnostic and giving the
transport a `json_writer` dependency to save one file would be a worse trade
than the one being fixed.

**What moves is decided by what a handler touches, not by its name.** Three
rules, each of which was found by hitting it rather than by design:

1. **`op_*` wrappers stay in `main.c`.** The generated route table
   forward-declares them `static` and is `#include`d there (ADR-0218), so
   they must be in that translation unit. This is a real constraint and not
   a deferral: 269 of the 276 are one line, so what stays is a dispatch
   table's worth of one-liners and what leaves is the work.

2. **A handler that drives the event loop stays with the event loop.**
   `handle_ntp_sync_post()` calls `start_ntp_sync_job()`, which arms a
   timerfd and registers a socket with epoll. Moving it would mean exporting
   the loop's internals to get one function out of one file, which trades a
   large clear boundary for a small blurred one. It stays, and its module's
   header says why.

3. **A subsystem's enum-to-status mapping is exported from its `api_` module,
   never duplicated.** `respond_ntp_error()` is needed by both the moved
   handlers and the one that stayed. One mapping that crosses a file boundary
   is right; two that agree today is how they stop agreeing.

## Consequences

Per slice this is code motion with no behaviour change, which is the point:
it is verifiable by compiling, and every call site is untouched.

`main.c` is 30,630 lines after the first three subsystems (sysctl, kmod,
ntp) plus the response helpers. That is a 654-line reduction and it is
deliberately unimpressive — the value is that the mechanism now exists and
the boundary is written down, so the remaining twenty-one subsystems are
ordinary work rather than a decision each time.

**The risk is real and worth naming.** This is a large refactor of the
process that is the entire control plane, and the dev sandbox can compile it
but cannot run the daemon-linked suite (it builds `cixctl` and nothing else).
So correctness rests on the full selftest on a real Cix host, and the
mitigation is slice size: one subsystem per commit, each independently
revertable, none of them changing a line of logic.

Two checks catch what per-file compilation cannot. A duplicate or missing
definition is caught by the host link, and `test_lint` (ADR-0248) caught a
real implicit-declaration during this very work, when `respond_ntp_error()`
moved out from under a caller that stayed.
