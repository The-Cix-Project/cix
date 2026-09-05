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

## The second thing holding it together: 66 path globals

`respond_error()` was the blocker for the first three subsystems. The
fourth found the other one.

`api_swap` would not compile because it needed `CONTAINERS_DIR`, then
`DISKS_MOUNT_DIR`, then `SWAP_FILE_PATH`. All 66 filesystem paths this
daemon derives from `--data-dir` were `static` in `main.c`, which meant
any handler that touches its own subsystem's directory could not leave.

Threading them through signatures was tried first and abandoned at the
third: a handler taking three path strings it immediately passes on is
not a better boundary, it is the same coupling written out longhand.

Two existing headers had already been living with this and said so.
`disk.h` and `device.h` both note, in as many words, that they have "no
knowledge of main.c's own `CONTAINERS_DIR` global, so the caller passes
it". That workaround was correct while the paths were static; it is
unnecessary now.

`daemon/include/daemonpaths.h` declares all 66 `extern`. The definitions
stay in `main.c` beside the code that computes them from `g_base_dir`,
with the comments explaining what each is for, so **no call site
anywhere changed** — main.c's own 61 uses of `CONTAINERS_DIR` included.
They are written once at startup and read-only afterwards.

A related move fell out of the same slice. `set_disk_quota()` and its
`resolve_backing_device()` helper were `static` in `main.c` and called
from two different concerns — volume quota changes and container
creation. They are now `quotamap_apply()` in `quotamap.c`, which already
owned which project id a name gets. Keeping the assignment and the
application in different files meant neither owned "quota".

## Consequences

Per slice this is code motion with no behaviour change, which is the point:
it is verifiable by compiling, and every call site is untouched.

`main.c` is 28,977 lines, down from 31,284, across nine modules: sysctl,
kmod, ntp (with the host clock, which shares ntp's error mapping),
syslog, resolv, keys, route, swap and volume — the last at 833 lines the
largest single group in the file.

Two handlers were deliberately *not* moved, and both are rule 2:
`handle_ntp_sync_post()` arms a timerfd, and the daemon-config handlers
rebind the live listen socket. Both stayed, and each says why where it
sits.

**The risk is real and worth naming.** This is a large refactor of the
process that is the entire control plane, and the dev sandbox can compile it
but cannot run the daemon-linked suite (it builds `cixctl` and nothing else).
So correctness rests on the full selftest on a real Cix host, and the
mitigation is slice size: one subsystem per commit, each independently
revertable, none of them changing a line of logic.

**A third shared blocker turned up with the logs slice**, and it is the
same shape as the first two: `url_query_param()` was `static` in
`main.c` with 18 call sites. It now lives in `apiroute.c`, which already
parses the query string — `api_route_query_unknown()` walks the same
grammar to decide what an operation declares, and two readers of one
grammar in two files is how they stop agreeing.

**Not every group can move, and stopping is part of the rule.**
`api_backup` was built and then reverted: the schedule PUT re-arms a
timerfd, the snapshot path calls into `do_system_backup()`, and forcing
it would have meant exporting main.c's internals to relocate 130 lines.
It waits for the system-backup group. `handle_backup_config_put()` and
the daemon-config handlers stayed for the same reason, and each says so
where it sits.

Two checks catch what per-file compilation cannot. A duplicate or missing
definition is caught by the host link, and `test_lint` (ADR-0248) caught a
real implicit-declaration during this very work, when `respond_ntp_error()`
moved out from under a caller that stayed.
