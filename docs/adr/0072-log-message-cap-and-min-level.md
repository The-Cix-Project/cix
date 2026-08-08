# 0072 — Log message cap raised, build-output tail-capture, settable minimum severity

## Status

Accepted

## Context

Raised directly by the user while a real `lldap` build ran on 192.168.15.95: "we should if I click on console be able to see what's happening on the `__pkgbuild` container right? otherwise how do we know if it's working?" — followed, once the build genuinely failed, by "we need to fix the logstore.h right? mainly the log message cap should not cause this, it's how we are logging right?" and a direct request for host/container pressure-stall stats and a settable log verbosity ("extensive enough to catch issues... settable so we do not over-log").

Diagnosing that real failure surfaced two compounding, real bugs in the consolidated log store (ADR-0070) and its one consumer that matters most for this kind of failure, `pkg.c`'s build-output capture (the mechanism ADR from the build-output-capture feature, closes a real "exit 127 is ambiguous" gap):

- **`LOGSTORE_MSG_MAX` was 512 bytes** — every log entry, regardless of source, silently truncated to roughly the first 500 bytes via `vsnprintf()`. Harmless for short diagnostic lines, but the one thing this whole capture mechanism exists for — a real build failure's own output — routinely runs to several KB before the actual error appears (a Rust build alone printed 10+ `Compiling x vY.Z` lines before hitting the 512-byte wall).
- **The capture itself only ever read the HEAD of the build's output**, via a single bounded `read(fd, buf, PKG_BUILD_OUTPUT_CAPTURE_MAX)` call (`PKG_BUILD_OUTPUT_CAPTURE_MAX` was 300). A pipe's `read()` returns whatever is sitting in the kernel buffer *first* — for any build producing more output than the cap, that is unconditionally early progress spam ("Compiling ...", "make: entering directory...") and *never* the failure line, which is almost always printed last. Confirmed live: the captured text for the real lldap failure was 100% `Compiling` lines, cut off mid-package-name, with the actual `error[...]` never reaching the log at all.

Both bugs independently make the log store useless for exactly the failure it was built to diagnose — small individually, but compounding: even if the cap were large enough, a head-only capture would still show old-but-irrelevant output for a sufficiently long build; even with tail-capture, a too-small cap would still truncate a genuinely long final error block.

## Decision

**`LOGSTORE_MSG_MAX` raised from 512 to 4096 bytes.** Still a fixed stack buffer inside `logstore_write()` (`vsnprintf()` truncates safely regardless of the exact value), still a plain JSON-lines text field on disk — raising it needs no migration and doesn't change the on-disk format, only how much future entries can hold.

**`pkg.c`'s build-output capture (`PKG_BUILD_OUTPUT_CAPTURE_MAX`, raised 300 → 3800) now drains the whole pipe and keeps the TAIL, not the head.** The child has already exited by the time this runs (confirmed: `pkg_build_completed()` is only ever called after `waitpid()` on the build container), so the write end is closed and a full drain-to-EOF loop can never block. A fixed-size sliding window (drop the oldest bytes already captured, append the newest) keeps exactly the last `PKG_BUILD_OUTPUT_CAPTURE_MAX` bytes regardless of how much total output the build produced — the actual error is almost always the last thing printed, so this is the correct default to optimize for, not an approximation of it.

**A new, persisted, write-time minimum-severity floor** (`logstore_set_min_level()`/`logstore_min_level()`, `PUT /system/logs/config`'s new optional `min_level` field alongside the existing `max_bytes`) — directly answering "settable so we do not over-log." Covers the real syslog severity scale (`emerg`..`debug`) kernel `dmesg` entries already carry via `kmsg_level_name()`; `kanxeod`/`audit` entries (today only ever "info"/"error") map onto the same scale (`"error"`/`"warn"` accepted as synonyms for the kernel's own `"err"`/`"warning"`). Checked in `logstore_write()` itself, before any segment-file I/O — an entry below the floor is dropped entirely, not merely hidden from `GET`'s pre-existing `level` query filter (which only ever filters what's already stored). Default `"debug"` (log everything) is a deliberate no-op default: a box that never touches this setting behaves exactly as before this ADR.

**Both `max_bytes` and `min_level` are independent, optional fields on the same `PUT`** — setting one never requires resupplying the other, matching this daemon's own established "only the fields given are touched" partial-update convention (`daemon-config`'s own PUT already does this).

## Consequences

- Purely additive for `max_bytes`-only callers — no behavior change unless `min_level` is explicitly set.
- The tail-capture rewrite is a real behavior change to `pkg.c`'s own build-failure diagnostics: a build failing after producing well over 3800 bytes of output will now show its own true error text where it previously showed only early, unhelpful progress lines. Verified against the full daemon-linked regression suite (23 binaries, all passing) — no test asserted on the old head-only capture's specific truncated content.
- This closes the diagnostic half of the real gap that blocked live diagnosis of a real `lldap` build failure on 192.168.15.95 — the box itself still needed a locally-rebuilt-and-pushed `kanxeod` to pick this fix up (no way to hot-patch a running daemon), and the original failure's own root cause was never captured by the old mechanism, so it still had to be reproduced separately to find.
- Does not address the user's other two same-session requests: host/per-container PSI (pressure-stall) stats (tracked separately, tasks #673/#677) and live-tailing a build's output while it's still running rather than only after failure (task #676, explicitly deferred by the user — "we'll address it much later"). Both are real, adjacent gaps this ADR does not attempt to close.
