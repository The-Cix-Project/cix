# 0246 — the control plane is one process doing five jobs, and it needs to be two

## Status

Superseded by [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md).

Built in full and deployed to 192.168.15.95, where the measurements
reversed most of what is argued below: the daemon was hung rather than
dead, the blocking-call count was wrong in kind as well as number, and
the supervisor worked while the worker proved unable to start a second
time. Kept unedited, because the reasoning that got here is the record
of how it was arrived at.

## Context

On 2026-09-05 `cixd` on 192.168.15.95 stopped answering. It had answered
normally minutes earlier. The host stayed healthy throughout: ICMP replied in
under half a millisecond, and the `jump` container kept serving real SSH
logins, running commands and reading `/proc` for the entire outage. Load
average, read from inside that container while the control plane was
unreachable, was `0.19 0.06 0.07` — the box was **idle**.

Two things about that are worth stating precisely, because both were guessed
wrong before they were measured.

It was not resource starvation. The first explanation reached for was that a
heavy build was starving the daemon, which is the failure ADR-0165's
shared-parent-cgroup budget was written against and the one this project has
seen before. At load 0.19 with no build running, that explanation does not
fit this occurrence at all.

And it is **not yet known whether the daemon was hung or dead**. The listener
went from accepting connections to refusing them while the box was idle. A
refused connection means no listener, not a slow one — a wedged loop with an
open socket gives a backlog and then timeouts, not `RST`. Whether the process
was alive with its sockets closed, or gone entirely, cannot be determined
from another machine, and this ADR deliberately does not assert it. It is
recorded as unknown rather than filled in, because the design below is
required under either answer and would be undermined by a motivating incident
that turned out to be described wrongly.

Either way the outcome for an operator was the same and is the actual
subject here: **the box could only be recovered by hand, and nothing on it
could say why.**

### What the code says

- `cixd` is single-threaded. There is exactly one `epoll_wait()` loop
  (`daemon/src/main.c`), and no `pthread_create` anywhere in the daemon.
- Of 48 `waitpid()` call sites across `main.c`, `pkg.c` and `exec.c`, 47 pass
  a `0` flag and block. `run_cmd()` is `fork()` plus `waitpid(p, &status, 0)`
  with no timeout, called inline from request handlers.
- `GET /v1/health` is served by that same loop. The endpoint whose entire job
  is to report the control plane's health is structurally incapable of
  reporting the one failure that matters, because answering requires the
  thing that has stopped.
- `stallwatch` is already a separate process with a shared-memory heartbeat
  and an HTTP probe of the daemon (#247). It **only records**. There is no
  `kill`, no `abort`, no restart; its only signal-related call is
  `PR_SET_PDEATHSIG`, which makes it die *with* the daemon it exists to
  watch.
- Its records are read back through `GET /v1/system/stalls` — the API that is
  down whenever they matter. That is #229, and it is not a bug in stallwatch:
  it is a consequence of the only retrieval path running through the
  subject.
- `cixd` runs as real pid 1 on an installed host (`g_pid1_mode`). There is no
  supervisor above it, so nothing can restart it.
- The registry of **running** containers is memory only. `registry.c` holds
  `static struct registry_entry g_entries[REGISTRY_MAX_CONTAINERS]` and does
  no file I/O at all — its only serialisation is the JSON writers that answer
  HTTP. What persists is `container_defs.json`, the *definitions*, from which
  `containerdef_autostart_all()` reconstructs at boot on the assumption that
  nothing is running yet.

### The actual root cause

One process is the init, the supervisor, the event loop, the REST API, the
container runtime, and the executor of unbounded blocking work. Those are
five jobs with five different failure profiles sharing one fate. Any one of
them failing takes all of them down, and the reporting of that failure is
itself one of the five.

This is not a defect in any of the five. Each is individually reasonable and
several are carefully built. It is a boundary that was never drawn.

## Decision

Split the control plane into a **supervisor** and a **worker**, give the
existing watchdog **authority to act**, and provide **one channel that does
not route through the worker**. In that order. Converting the blocking calls
is a fourth item and is deliberately sequenced last.

### 1. A supervisor process, and a disposable worker

A new, small `cix-init` becomes pid 1 on an installed host. Its entire job is
to reap, to hold whatever it must to keep running workloads alive across a
worker restart, and to restart the worker. It does not speak HTTP, does not
run recipes, does not touch rtnetlink, and calls nothing that can block
indefinitely. It should fit in one file and be readable in one sitting.

`cixd` becomes the worker: it keeps the event loop, the API, and the runtime,
and it stops being pid 1.

"Hold whatever it must" has a specific mechanism, and it is the largest part
of this item. Because the running registry is memory only, a restarted worker
would autostart duplicates on top of survivors — colliding names, colliding
bridges — so the split is not an improvement until the worker can **re-adopt**
what is already running. The anchor is the container cgroup, `/sys/fs/cgroup/
<name>`: kernel state, keyed on the name, outliving any daemon. For each
definition the worker reads that cgroup and, when it is populated, rebuilds a
`struct container_handle` from kernel state rather than calling `clone3` —
the pid from `cgroup.procs` (the one whose parent is outside the cgroup),
`cgroup_fd` by reopening the directory, `pidfd` from `pidfd_open()`,
`interfaces_netns_fd` from `/proc/<pid>/ns/net`. An empty cgroup means the
container really is gone and autostart proceeds as it does today.

Re-adoption forces one interface between the two processes. A surviving
container is reparented to `cix-init`, so it is no longer the worker's child:
the worker can poll a `pidfd` for its exit, but `waitid()` on a non-child
fails, and the exit status and term signal are reaped by the supervisor and
otherwise lost. `cix-init` therefore forwards every child it reaps — its pid, whether it
exited or was killed, and the code or signal — to the worker over a
`socketpair` handed to it
at spawn, replaying anything reaped while no worker was running. It forwards
all of them and lets the worker filter; the supervisor is not the right place
to know which pids matter. Without this channel a re-adopted container that
exits reports a null exit status with no reason, which is a placeholder.

The point of the split is that **the worker becomes disposable**. Today
killing `cixd` is indistinguishable from destroying the host, which is why no
automatic recovery could ever be built on top of it. The property that makes
this safe was observed during the incident above rather than designed:
containers kept running while the control plane was unreachable. This ADR
makes that a guarantee instead of an accident.

### 2. The watchdog acts

`stallwatch` already measures the right things. The change is what it does
with the measurement: on `PROBE_FAILURES_FOR_STALL` it signals the
supervisor, and the supervisor kills and restarts the worker. `PDEATHSIG` is
removed — it exists precisely so the watchdog can outlive the thing it
watches, and today it guarantees the opposite.

Recording is already built. Acting is the whole delta.

### 3. One channel that does not route through the worker

The supervisor answers exactly one question, on its own port, requiring
nothing from the worker: worker alive since when, how many restarts, and the
last stall reason. Nothing else. This is what makes the failure observable
from a second machine, and it is what would have answered "wait or reset" at
the moment it mattered rather than four hours later.

### 4. The blocking calls — and this ordering was wrong

This was sequenced **last**, on the argument that it reduces how often a
restart is needed but does not provide the restart, and that doing it first
would leave the recovery path untested.

That reasoning optimised for having a recovery path over not needing one, and
on 2026-09-05 it delivered neither: the supervisor restarted the worker
correctly and the worker could not come up, so the recovery mechanism caused
a worse outage than the failure it was recovering from. Items 1-3 are
containment. This is the actual robustness, and it comes first.

**The "47 blocking `waitpid()`s" figure above was wrong, and the correction
matters more than the number.** It counted blocking *calls* rather than calls
that can *block*. Measured across `daemon/src` and `src`, there are 65, and
they fall into groups with very different meanings:

- **14 sit in `handle_*_event()` pidfd callbacks.** `EPOLLIN` on a pidfd
  means the child has already exited, so the wait collects a zombie and
  returns immediately. These cannot block, and converting them would be
  churn that removes nothing.
- **Roughly 14 reap the short-lived intermediate of a double fork**, which
  `_exit()`s as soon as it has forked the grandchild. Bounded by
  construction.
- **Most of the rest run a bounded external tool** — `sfdisk`, `blkid`,
  `openssl`, `tar`, `modprobe` — synchronously. Unbounded in principle,
  bounded in practice, and worth converting in order of how long the tool
  can really take rather than all at once.

**Both incidents that actually took the host down were blocking fd I/O, not
`waitpid`.** #294 was a `read()` on a console pty master (`wchan
n_tty_read`), and the wedge that motivated this ADR showed the same shape.
So the rule that matters is *every fd the reactor polls is non-blocking*, and
that was audited: the listeners use `accept4(SOCK_NONBLOCK)`, client sockets
and the reap channel are non-blocking, the console pty was fixed in #294, and
the container-output pipe sets `O_NONBLOCK` and **disables capture entirely
rather than registering a blocking fd** when that fails. The one
`open(tty_path, O_RDWR)` without `O_NONBLOCK` is in a forked child after
`setsid()`, so it can hang that child but never the reactor. No blocking fd
reaches epoll today.

What holds this is `test_blocking_waits`, a per-file budget of blocking waits
with the reason each is currently acceptable. It deliberately does **not**
claim the daemon never blocks — it asserts that the set of places that can is
known and counted, so adding one is a deliberate act visible in a diff. That
is the instrument this project has twice found to be the only one that
actually holds (ADR-0224's toolchain count, #285's curl guards); a prose rule
in its place is what let gcc reach 21 recipes with nobody counting.

## Consequences

**A wedged or dead control plane recovers itself.** That is the whole point,
and it is what today's architecture cannot do at any level of care in the
request handlers.

**A worker restart is not free and must not be silent.** In-flight requests
die, console and exec sessions drop, and a package build in progress is
interrupted. The supervisor's restart count and reason are therefore part of
the out-of-band answer and belong in the log store once the worker is back —
a restart nobody can see is a worse failure than the one it fixed.

**The boundary must be proven, not assumed.** This is only trustworthy once a
deliberate `kill -STOP` of the worker on a real host is seen to produce a
restart, with containers still running afterwards. A restart path that has
never been exercised is not a recovery mechanism.

**The API-First Mandate is unaffected.** The supervisor's port is a
diagnostic readout with no control surface — it starts nothing, changes
nothing, and cannot be used to manage the host. Every capability remains a
REST endpoint on the worker. A read-only "is the worker alive" answer is not
a second management path, and treating it as one would mean the platform can
never report its own unavailability.

**`cixd` stops being pid 1, and issue #131 still applies to whatever is.**
Returning from `main()` as pid 1 is a kernel panic. That constraint moves to
`cix-init` rather than disappearing.

**This does not replace ADR-0165 or ADR-0244.** Starvation and OOM protection
remain the right defences against the failures they address. This ADR is
about what happens when a defence does not hold, which on this platform has
so far meant walking to the hypervisor.

### What `--init-mode` turned out to mean

Building this exposed a conflation that had never mattered before,
because until now the two halves were always true together.

`--init-mode` means, and still means, "this process is the control plane
of a real installed host": it is what makes `reboot(2)` the correct way
to shut down rather than returning from `main()`, and what selects the
A/B boot confirmation. It ALSO meant "the kernel has just booted and
nothing has mounted anything yet", which is what `boot_init()` acts on —
mounting `/proc`, `/sys`, cgroup2 and `/boot`, applying the static IP,
bind-mounting `resolv.conf`.

A restarted worker is the first thing that is ever the first without
being the second. Every one of those mounts is already present, `mount(2)`
answers `EBUSY`, and `mount_or_fail()` treats any failure as fatal — so
the worker would have exited immediately, five times, and the
supervisor's own fast-fail path would then have rebooted the machine.
The recovery mechanism would have been the outage.

The worker cannot tell the two apart: the machine looks identical from
inside either way. The supervisor is the only thing that knows, so it
says — `CIX_WORKER_START`, beside `CIX_REAP_FD`. This is the second
thing the split forces into the interface between the two processes, and
like the reap channel it is not incidental: a disposable worker needs to
know it is a replacement.

### The API follows the architecture

`DELETE /v1/system/processes/{pid}` refused this daemon's own pid. That
was right while `cixd` was pid 1 — killing it was indistinguishable from
destroying the host, and there was no recovery to return to. The check
encoded a fact this ADR changes, so the check changes with it: a
supervised worker may be killed through its own API, and an unsupervised
one may not, because a `cixd` that is pid 1 killing itself is a panic.

This is also what makes the proof below runnable. The host is
shell-less; with the old guard there was no way to kill the worker and
watch it come back.

## Resolved after the fact: it was hung, not dead

The question left open above — hang or process death — is answered, by the
measurement this section asked for. `GET /v1/system/processes` on the
recovered box reports pid 1 as `cixd`, command line
`/bin/cixd --init-mode --slot=a --bind=192.168.15.95`, which is what the
kernel cmdline `init=/bin/cixd -- --init-mode` produces and what every deploy
this project has made sets.

A dead pid 1 is `Attempted to kill init!`, a kernel panic (#131). The box did
not panic: it replied to ICMP throughout, and the `jump` container served real
SSH logins for the whole outage. So `cixd` cannot have exited. It was alive
and not answering — which the console then showed directly, reporting the
daemon in `state S` with `wchan n_tty_read`, blocked reading a pty master.
The refused connections that suggested "no listener" are consistent with
this: a full accept queue sends `RST`, which is indistinguishable from a
closed port to the machine being refused.

This does not change the decision. The ADR was written to be required under
either answer, and it is: a hung worker is precisely the case where an
in-process health endpoint cannot report and an in-process watchdog cannot
act. It does sharpen item 2 — the watchdog is watching for exactly this, and
today it can only write it down.

Recorded on #283.

## Consequences of the amendment

The re-adoption mechanism and the reap-forward channel described in item 1
were not costed when this split was first scoped as items 1-3. They are not
new items — they are what item 1 turned out to require once the registry was
read — but they are the bulk of the work, and item 1 is no longer a small
file that reads in one sitting on the worker side.

Item order within the change follows from the same finding: the reap-forward
channel is designed first because both other pieces depend on its shape,
re-adoption is written and proven next (in the suite, where a forked `cixd`
is not pid 1 and can actually be killed), and only then does `cix-init`
exist to restart anything. Re-adoption cannot be deployed alone and proven on
a real host, because today `cixd` *is* pid 1 and nothing can restart it —
that is the circularity the split exists to break.
