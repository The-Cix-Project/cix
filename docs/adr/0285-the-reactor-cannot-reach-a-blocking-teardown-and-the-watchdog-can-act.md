# 0285 — the reactor cannot reach a blocking teardown, and the watchdog can act

## Status

Accepted

Extends [ADR-0180](0180-async-container-teardown.md) and
[ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md). Neither is
reversed: ADR-0180's asynchronous teardown becomes the only teardown, and
ADR-0247's non-blocking reactor remains the defence. This adds the two things
each of them left as prose.

## Context

On 2026-09-13 the control plane on 192.168.15.103 stopped answering and stayed
that way for eleven minutes, until the owner reset the box from the hypervisor.
TCP handshakes completed and ICMP answered throughout; no HTTP request completed
on port 80 or 443. `cixd` is `init=/bin/cixd`, so there was no other process on
the machine to ask.

The watchdog caught it, and its record is the whole diagnosis:

```json
{"ts":1789265508,"event":"stall-continues","seconds":637,"state":"S","wchan":"do_wait",
 "activity":"",
 "children":[{"pid":133,"comm":"cixd","state":"S","wchan":"fuse_dev_do_read"},
             {"pid":134,"comm":"cix-init","state":"S","wchan":"do_sys_poll"},
             ... ten more cix-init, all do_sys_poll ...]}
```

`wchan: do_wait` is a blocking `wait`-family syscall on the one thread that also
serves HTTP. `activity` is empty because it was entered from a **timer**, not
from a request. The call chain is:

```
handle_rolling_restart_timer_event()        main.c:27055
  └─ registry_remove(name)                  registry.c:426
       ├─ sys_pidfd_send_signal(…, SIGKILL)
       └─ registry_mark_exited(e)           registry.c:333
            └─ container_wait(&e->handle, …)
                 └─ waitid(P_PIDFD, h->pidfd, &info, WEXITED)   container.c:1275
```

A `pkg install` into an image triggers a rolling restart of every container
following it. That is the route from an ordinary package install to a box that
needs a physical reset.

### The cause is not "a blocking wait". It is two implementations of one job.

ADR-0180 built the asynchronous teardown — `begin_container_stop()` → SIGTERM →
pidfd `EPOLLIN` → `container_exit_finalize()` — after this same wait froze the
control plane twice, and wired `DELETE` and `POST .../stop` through it. It then
left `registry_remove()`'s synchronous kill-and-reap in place, and ten reactor
call sites kept using it. Each is guarded by a **comment** arguing the entry
cannot be running at that point. Seven of those arguments are correct. Three are
not:

| site | path | can the container be running? |
|---|---|---|
| `main.c:27055` | rolling restart after an image update | yes — this is the one that froze the box |
| `main.c:22090` | `POST /v1/pkg/cancel` | yes — cancelling a live build is the entire point |
| `main.c:15500` | container storage-migration finalize | yes — no running check of any kind |

This is [#399](https://git.home.arpa/itdlabs/cix/issues/399)'s shape exactly: a
comment describing a safety property that was never a mechanism, read as
documentation of something that had been checked. `registry_remove()`'s own
declaration comment additionally claims the running-kill branch "remains only
for the internal storage-migration finalize path" — `grep -rn registry_remove
daemon/src/containerstoragemigrate.c` finds nothing at all. The comment was
describing an intention about a file that does not call the function.

### ADR-0247's instrument saw this call and passed it

`test_blocking_waits` carries a per-file budget of blocking waits with a stated
reason for each. `container_wait()` is in it, by name:

```c
{ "src/container.c", 4, "clone3 intermediates and container_wait" },
```

So the gate worked exactly as designed and did not catch this, because it counts
**where a blocking call is written**, not **which reactor paths can reach it**.
A budget on definitions cannot see a caller. That is not an argument for
deleting the budget — it is still the thing that makes adding a blocking call
deliberate — but it is the reason a second instrument is needed, one that acts
at the reachable boundary rather than at the definition.

### And ADR-0247 already named the other half

Its own Consequences say, in full:

> **`stallwatch` records and does not act.** Its `SIGUSR1` is gated on a
> supervisor being present, which it is not, so the daemon's watchdog is back to
> writing findings into a file readable through the API that is down whenever
> they matter (#229). Known, and not fixed here.

Tonight is what that sentence costs. The watchdog had the complete diagnosis at
five seconds and wrote it to a file reachable only through the daemon that was
not answering. An operator's sole route to it was a serial console.

### Why not restart the worker

ADR-0246 proposed exactly that, it was built, deployed and measured, and ADR-0247
records the result: the restarted worker never created its listener, ports 80 and
443 refused connections for ten minutes, and the box needed a reset anyway — *"a
restart path that cannot restart is worse than none"*. The named prerequisite is
that every step of the `--init-mode` startup path be idempotent to a second start
**in one kernel lifetime**, which `diskrole_init()`,
`diskformat_remount_present_role_disks()`, `network_init()` and a dozen
`boot_subsystem_init()` calls are not.

A reboot does not have that prerequisite. It gives a fresh kernel, which is
precisely what that startup path already assumes and the only condition under
which it has ever been known to work. So the escalation this ADR adds is a
reboot of the machine, never a restart of the worker — ADR-0247's finding stands
untouched and is the reason for the choice rather than an obstacle to it.

## Decision

### 1. The reactor cannot reach a blocking teardown

`registry_remove()` stops being able to kill. It becomes what its name says:
release the table slot of an entry that has already exited.

Given an entry it believes is running, it does **not** trust that belief — it
polls the pidfd. This distinction is load-bearing rather than fastidious.
ADR-0180 already recorded that `e->paused` "can in principle go stale", and
`e->running` is the same kind of flag; a gate keyed on the flag alone would turn
every stale-flag case from "works" into "refused", breaking
`POST .../start` on an exited-but-registered container for no gain. So:

- pidfd readable (the process is gone, the flag was stale) — reap it and
  continue, which is the behaviour callers already expect;
- pidfd not readable (it really is running) — write an error to the log store
  naming the caller's mistake, set `errno = EBUSY`, return `-1`, change nothing.

This is the gate, and it is chosen for the same reason ADR-0224's toolchain count
and ADR-0247's own wait budget were: **this project has twice established that a
prose rule does not hold and a mechanism does.** Any future path that reaches
teardown wrong now produces a log line and a failed operation, not a box that
needs a physical reset. The failure becomes loud instead of fatal, which is the
only property that survives someone not having read this ADR.

`registry_mark_exited()` gains the matching precondition on the daemon side: it
polls the pidfd and refuses to wait on one that is not ready, logging instead.
The guard goes there rather than into `container_wait()` because `container_wait()`
is the runtime library's public, legitimately-blocking API — `test/test_container_net.c`
uses it correctly five times to wait for a container it just started. The
constraint being enforced is the daemon's single-threaded reactor, so it belongs
on the daemon's side of that boundary, not imposed on every consumer of the
library.

The three running-capable callers convert, and one new teardown kind serves them:

- **`REGISTRY_TEARDOWN_MIGRATE`** — "stop it, copy its storage, then replay the
  definition." Container storage migration is a genuinely different intent from
  a restart: there is a copy pass and a disk re-pointing between the stop and the
  replay, so it gets its own kind rather than being bent into RESTART. Its
  handler splits at the point it already had a seam — everything before the
  stop stays where it is, and everything after (the final copy pass,
  `containerdef_patch_disk()`, the replay, and the abort path that restarts from
  the original location) moves into the exit completion.
- **`REGISTRY_TEARDOWN_RESTART`** — "this incarnation is over; replay the
  definition once it is really dead." The rolling restart and
  `POST /containers/{name}/restart` are the same intent and share the one kind,
  rather than each growing its own. The timer handler keeps the entry's
  `reactor_conn` (it is how the exit is learned), calls
  `begin_container_stop(e, REGISTRY_TEARDOWN_RESTART)`, and returns.
  `container_exit_finalize()` replays the definition through the same
  `create_container_from_body()` the handler used to call inline.
- **`POST /pkg/cancel`** on a running build container calls
  `begin_container_stop(e, REGISTRY_TEARDOWN_STOP)`. Its hand-written block that
  drives `pkg_build_completed()` "because the exit never reaches
  `handle_container_event()`" is then **deleted**: with the async path the exit
  does reach it, and `container_exit_finalize()` already calls
  `pkg_build_completed()` at the top. The conversion removes code rather than
  adding it, which is the sign it was a parallel implementation.
- **Storage-migration finalize** stops the container the same way and completes
  from the exit.

### 2. `stallwatch` becomes `cix-watchdog`, it is configurable, and it can act

**The name.** It has not only watched for stalls since #247 — it probes whether
the daemon is *serving*, measures per-pass loop latency, and now acts. `watchdog`
is what it is, and the rename is a clean cut-over: `daemon/src/watchdog.c`,
`daemon/include/watchdog.h`, `watchdog_*` symbols, `watchdog.jsonl` as the record
file, `test_watchdog.c`. No alias is kept (the project takes clean cut-overs over
compatibility shims).

**It is configuration, over the API.** Every threshold was a `#define`. A
watchdog that can reboot a machine must be something an operator can see, tune
and switch off, and by the API-First mandate that means a REST resource before
anything else: `GET`/`PUT /v1/system/watchdog`, a partial update in the shape
`daemon-config`/`hostauth-config`/`pkg-build-config` already established.
`GET /v1/system/stalls` becomes `GET /v1/system/watchdog/events`.

```
enabled                            true     the whole watcher, on or off
poll_interval_ms                   500      how often the child wakes (floor 100 -- a 1ms poll burns a core)
stall_threshold_seconds            5        loop quiet this long counts as a stall
repeat_seconds                     30       while a stall continues, one more record this often
slow_pass_ms                       750      a loop pass whose WORK exceeds this is a slow pass
probe_interval_seconds             5        how often to ask the daemon whether it is serving
probe_timeout_ms                   4000     how long to wait for that answer
probe_failures_for_stall           2        consecutive refusals before it is reportable
diagnose_after_seconds             60       record /proc/<pid>/syscall for the daemon and its children; 0 disables
escalation_action                  reboot   reboot | halt | none
escalation_after_seconds           600      how long a serving stall runs before the action; 0 disables
escalation_requires_service_seen   true     only escalate if the daemon has served at least once this boot
capabilities                       read-only: which escalation_actions this build can actually perform
```

**Every number and every default action above is a field, and that is the
point** -- each one was a `#define` compiled into the binary, so tuning the
watchdog meant a rebuild and a reboot of the thing being tuned.
`escalation_action` is a field rather than a fixed behaviour because the right
answer genuinely differs per box: `reboot` returns a production host to service,
`halt` leaves a lab box stopped where its state can be examined, and `none` is
today's record-only behaviour, which stays exactly expressible rather than
becoming something you lose by upgrading.

**Two numbers are deliberately NOT exposed, named here so the omission is a
decision rather than an oversight.** `setpriority(PRIO_PROCESS, 0, -10)` is a
scheduling detail with one correct value -- the watchdog must not be starved
alongside what it watches, which is a live theory for why #229's wedge left no
record -- and `STALL_ACTIVITY_MAX` is a buffer size, not policy; making either
configurable would offer an operator a way to break the watcher with no
corresponding thing to gain.

The two escalation thresholds are separate knobs rather than one `action` enum
with a threshold, because the ladder is genuinely two rungs and an operator wants
the cheap one on a box where they would never want the expensive one. `0`
disables either independently, so "record only" — today's behaviour — remains
expressible exactly.

`capabilities` is read-only and exists so a client can tell what a given build
can do rather than inferring it from the version. A dashboard offering a reboot
knob that this binary cannot honour is worse than not offering it.

**Configuration reaches the child through the shared page it already has.** The
parent writes the values on `PUT`; the child reads them at the top of each pass.
No new IPC, no file I/O in the watchdog (which must keep working when the
filesystem is the thing that is wedged), and a change takes effect immediately.

It is persisted the way every other config resource here is — a JSON file under
the state directory, loaded at startup and written on `PUT` — so a tuned or
disabled watchdog survives a reboot. That matters more for this resource than
most: an operator who switched the reboot off did so because a reboot was wrong
for their box, and a setting that quietly returns to the default on the next
boot is worse than not having the knob.

**The teeth, and their bounds.** Two rungs, escalating on the *serving* stall —
the one condition an operator cannot observe from anywhere else, because
`GET /v1/health` is served by the loop that has stopped.

**Rung 1, `diagnose_after_seconds`: read what the kernel already knows about the
blocked thread.** For the daemon and every child, the watchdog records
`/proc/<pid>/syscall` — the syscall number *and its six arguments*, plus stack
pointer and instruction pointer — alongside the `wchan` it already captures, and
attempts `/proc/<pid>/stack` best-effort.

This is the rung that answers #448's own open question. `wchan: do_wait` said the
daemon was blocked in a wait and said nothing about which of twenty-odd call
sites; the `waitid()` arguments name the pidfd being waited on, which identifies
the container and therefore the path. A diagnostic that says "stuck" is worth far
less than one that says "stuck waiting for pid 134".

The first draft of this ADR specified `/proc/sysrq-trigger` (`t` and `w`) for
this rung. It was **measured and it does not exist here**: `kernel.sysrq` is not
a key this platform's kernel has (`GET /v1/system/sysctl/kernel.sysrq` →
`no such sysctl key`), `/proc/sysrq-trigger` is absent inside a container on the
box, and `CONFIG_MAGIC_SYSRQ` appears nowhere in `recipes/package/kernel`'s
config. Building it would have shipped a placeholder as a feature. What *was*
measured working, on the box's own kernel: `cat /proc/self/syscall` returns
`0 0x3 0x7f65add05000 0x40000 0x0 0x0 0x0 0x7fff84641bb0 0x7f65adddacd2`.
`/proc/<pid>/stack` was `Permission denied` to an unprivileged container user;
whether real host root can read it here is **not yet verified**, so it is
best-effort and is recorded as unavailable rather than claimed present. No
sentence in this ADR asserts it works, and none should until a record shows
one. Turning `CONFIG_MAGIC_SYSRQ` on is a separate, defensible
kernel change and deliberately not bundled here.

**Rung 2, `escalation_after_seconds`:** `sync()`, a final record, then the
configured `escalation_action` -- `reboot(RB_AUTOBOOT)` or `reboot(RB_HALT_SYSTEM)`.
The watchdog runs as root, is a separate process, and is not blocked, so it can
do this when nothing else on the box can.

Be plain about what that is: `reboot(2)` from a root process that is not pid 1
goes straight to the kernel's own `kernel_restart()`. It does **not** route
through `cixd`'s shutdown path, no container is asked to stop, and nothing is
flushed beyond the `sync()` immediately before it. It is, deliberately, the
software equivalent of the hypervisor reset a human would otherwise perform --
chosen because by that point the software route has been proven unavailable for
ten minutes. Filesystems are journaled and the `/config` and container disks
replay, which is the same recovery every one of this project's recorded resets
has already been through.

**The reboot is armed only after the daemon has been observed serving at least
once since this boot.** Without that, a box that wedges *during* startup reboots,
wedges again, and reboots forever — an unbootable machine, which is strictly
worse than a hang, because a hang can at least be inspected from the console. The
watchdog already records `probe_last_ok_monotonic`; arming on "have I ever seen a
successful probe" costs one condition and converts the catastrophic case into the
merely broken one. This is also the property that makes the default safe to ship
switched on.

The default of 600 seconds is set from measurement rather than taste: every wedge
this project has recorded that needed a physical reset ran past ten minutes
(sixteen in ADR-0165's incident, eleven on 2026-09-13), and no healthy request on
this daemon has ever taken more than a few seconds. Ten minutes of a control
plane serving nothing is not a slow moment; it is a box that is already lost, and
the only question left is whether a human has to walk to it.

## Consequences

**The class of failure that required a hypervisor reset is bounded in time.**
Not eliminated — a hang the gate does not prevent can still happen — but it now
ends in an automatic reboot rather than in waiting for someone to notice. That is
the regression ADR-0247 accepted deliberately, and the reason it accepted it (no
worker can start twice) does not apply to a reboot.

**A forced reboot loses work, and this is the real cost.** An in-flight package
build dies with the machine. That is the trade being made explicitly: ten minutes
of a dead control plane has already cost more than any one build, and the build
can be re-run while a physical reset needs a person. An operator who disagrees
for their box sets `reboot_after_seconds: 0`.

**A wedge during startup cannot become a reboot loop**, because the reboot arms
only after a successful probe this boot. The cost of that safety is real and
stated: a box that wedges before it ever serves gets no automatic recovery at
all, which is exactly the state that needs a console. Making startup itself
watchable is a separate problem (ADR-0247 found the same edge from the other
direction: `stallwatch` starts *after* the listener, so nothing watched the ten
minutes a restarted worker failed to bind).

**A forced reboot into an unconfirmed A/B slot may roll back.** That is the
slot mechanism working as designed — a box that wedged before confirming its boot
returning to the previous known-good root is the correct outcome — but it means a
reboot is not always a no-op for what is running, and it is recorded here rather
than discovered later.

**Two callers create immediately after removing, and they now check the return.**
`POST .../start` on an exited-but-registered container and the rolling replay
both do `registry_remove(name)` and then `create_container_from_body()`. If the
remove refuses, the create would 409 on a slot that is still in use and the
operator would be shown a name collision instead of the real reason. Both check,
and surface the logstore message.

**Timer-driven work now names itself.** Tonight's record carried `activity: ""`
because it came from a timer rather than a request, and that single empty field
is most of why the call site took an hour to find. `watchdog_activity()` is set
by the rolling-restart, crash-restart and console-respawn timers, so the next
record of this shape names the path in its first line.

**`registry_remove()` returning `-1` is a new failure mode for the other callers,
which never checked it.** They are the ones whose entries are genuinely exited, so the
new branch is unreachable for them; the value of the change is that if one of
those arguments is wrong, it now surfaces as a logged error and a failed
operation rather than as a frozen box. This is the same trade the gate makes
everywhere: a wrong assumption becomes visible instead of catastrophic.

**`test_blocking_waits` keeps its budget and gains a companion.** The budget still
makes adding a blocking call deliberate. It could not have caught this, and the
gate is what does.

**One teardown implementation remains.** `registry_remove()` releases slots;
`begin_container_stop()` stops containers. That is the One Source of Truth this
ADR is really about — the wedge was not caused by a bad line of code but by two
correct implementations of the same job, where choosing between them was left to
whoever wrote the next caller.
