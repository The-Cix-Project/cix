# ADR-0105: real freshness tracking for `pkg hostbuild cix --deploy`

## Status

Accepted

## Context

`cixctl pkg hostbuild cix --build-image=... --deploy` chains three
things: the hostbuild itself (`pkg_hostbuild_start()`), a server-side
follow-on assembly of `cixd-root.squashfs` triggered once the hostbuild
reaches `PKG_STATE_INSTALLED` (`spawn_cix_bootroot_assembly()`,
ADR-0057), and finally `POST /system/update --image=<artifact_path>/
cixd-root.squashfs`. The CLI's own `--wait` polls the hostbuild's own
state until it leaves `fetching`/`building`; once it reports `installed`,
`--deploy` treated `cixd-root.squashfs` existing on disk at the
artifact path as "ready" and proceeded straight to the deploy.

That file existing is not the same thing as it being *fresh*. The
assembly step is asynchronous and takes real time (a full `mkbootroot`
invocation); the file at that path is a leftover from whichever assembly
last succeeded, not necessarily the one this round's own hostbuild just
triggered. Reproduced directly on 192.168.15.95 (task #737, discovered
during #672's own deploy verification): `--deploy` reported success
immediately, but `GET /v1/disks` still lacked the fields a redeployed
daemon should have shown -- the box had silently booted a stale artifact
from an earlier round. Only a second, unprompted "cix bootroot
assembly: succeeded" log line (ADR-0087's own logstore reporting) and a
manual re-deploy actually landed the real build.

## Decision

### A monotonic generation counter, not a timestamp or a poll-and-hope retry

`daemon/src/main.c` gains three small pieces of process-lifetime state:
`g_bootroot_assembly_started` (incremented once per real assembly attempt,
right after `fork()` confirms a real child now exists -- not on a failed
`fork()`, which never attempted anything), `g_bootroot_assembly_completed`
(set to that attempt's own generation number *only* on a confirmed success,
`WIFEXITED && WEXITSTATUS==0`, in `handle_bootroot_assemble_event()` --
never advanced on failure), and `g_bootroot_assembly_running` (true for the
duration of one attempt, letting a client distinguish "still working" from
"gave up, that one failed" instead of polling `completed` forever after a
real failure).

A generation counter, not a wall-clock timestamp: at most one assembly is
ever in flight (it's only ever triggered by the "cix" hostbuild's own
single-job-constrained completion event, so there's no concurrent-attempt
case to disambiguate), so a plain incrementing integer is sufficient and
avoids clock-skew/resolution questions a timestamp comparison would raise
for no real benefit here.

### Exposed via `GET /system/boot`, not folded into `pkg.c`'s own generic status JSON

`GET /v1/pkg/hostbuild/{name}` is generic (`pkg_get_one()`, shared with
every other hostbuild name and with `pkg install`) -- ADR-0057's own
comment in `main.c` already establishes that `pkg.c` stays fully agnostic
to what any package or hostbuild name *means*; "cix" is a plain string
match `main.c` itself owns. Adding fields to `pkg_get_one()`'s own JSON
output for one specific hostbuild name would break that separation.
`GET /system/boot` (ADR-0077) is already the natural home for "state about
the currently/most-recently-assembled boot image" -- reporting the
assembly generation there, rather than inventing a new endpoint, keeps the
subject matter coherent without giving `pkg.c` any new opinions.

### The CLI captures a real baseline before triggering anything

`cmd_pkg_hostbuild()` reads `bootroot_assembly_completed_generation` via
`GET /system/boot` *before* `POST /v1/pkg/hostbuild` is even sent (only
when `--deploy` was requested for the "cix" hostbuild specifically --
every other hostbuild name deploys straight from its own `artifact_path`
with no server-side follow-on to wait for). Capturing it any later --
e.g. right before the deploy step, after `--wait`'s own poll already
confirmed `state=="installed"` -- would itself already be racing a slow
but real assembly from an *earlier* round that simply hadn't finished yet,
which would look exactly like "the baseline" to a check done that late.

Once the hostbuild itself reaches `installed`, the CLI's new
`wait_for_fresh_bootroot_assembly()` polls `GET /system/boot` (the same
500ms interval `poll_hostbuild()` already uses) until either the completed
generation advances past that baseline (a genuinely fresh artifact is now
on disk -- proceed to `--image=.../cixd-root.squashfs`), or the
assembly is no longer running and never advanced past baseline (a real,
reported failure -- `"cix bootroot assembly did not produce a fresh
artifact (see GET /system/logs for the real failure)"`, exit 1, no
deploy attempted). This is the real fix shape the originating task
description asked for: "compare assembly completion timestamp/generation
against what `--deploy` last saw," not a poll-and-hope retry loop.

## Consequences

- `--deploy` for the "cix" hostbuild can no longer silently redeploy a
  stale artifact from an earlier round -- it either waits for a
  confirmed-fresh one or fails loudly with a pointer to the real
  diagnostic (`GET /system/logs`, already wired since ADR-0087/task #669).
- No new persisted state -- the three counters are process-lifetime only,
  matching every other async-job tracking mechanism in this daemon
  (disk format, ISO assembly, pkg fetch) which are all also
  process-lifetime, not persisted across a restart.
- `GET /system/boot`'s response shape grows three fields; every existing
  consumer that only reads `build_version`/`build_time`/`slot`/
  `kernel_version` is unaffected (additive, not a breaking change).
