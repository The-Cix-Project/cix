# 0247 — the reactor does not block, and that is the defence

## Status

Accepted

Supersedes [ADR-0246](0246-the-control-plane-is-one-process-doing-five-jobs.md).

## Context

ADR-0246 was written the day the control plane stopped answering. It argued
that `cixd` is the init, the supervisor, the event loop, the REST API, the
container runtime and the executor of unbounded blocking work — five jobs
with five failure profiles sharing one fate — and that the answer was to
split them, give the watchdog authority to act, and add a channel that does
not route through the worker.

That was built in full, deployed to 192.168.15.95, and measured. Almost every
load-bearing claim in it turned out to be wrong, and the corrections point
somewhere else. This ADR records where.

### What the box established

**The daemon was hung, not dead.** `GET /v1/system/processes` reported pid 1
as `cixd`. A dead pid 1 is `Attempted to kill init!`, a kernel panic; the box
never panicked, replied to ICMP throughout, and the `jump` container served
real SSH logins for the whole outage — so the process cannot have exited. The
console then showed it directly: `state S`, `wchan n_tty_read`. The refused
connections that first read as "no listener" fit a hang rather than
contradicting it, because a full accept queue sends `RST`.

**The blocking-call count was wrong, and the shape of the correction matters
more than the number.** ADR-0246 said 47 blocking `waitpid()`s. That counted
blocking *calls* rather than calls that can *block*. Measured across
`daemon/src` and `src` there are 65, and they are not one population:

- **14 sit in `handle_*_event()` pidfd callbacks.** `EPOLLIN` on a pidfd means
  the child has already exited, so the wait collects a zombie and returns
  immediately. These cannot block, and converting them would remove nothing.
- **Roughly 14 reap the immediately-exiting intermediate of a double fork.**
  Bounded by construction.
- **Most of the rest run a bounded external tool** — `sfdisk`, `blkid`,
  `openssl`, `tar`, `modprobe`.

**Both incidents that actually took the host down were blocking fd I/O, not
`waitpid`.** #294 was a `read()` on a console pty master; the wedge that
motivated ADR-0246 showed the same `n_tty_read` shape. So the rule that
matters is *every fd the reactor polls is non-blocking*, and that was audited
rather than assumed: the listeners use `accept4(SOCK_NONBLOCK)`, client
sockets are non-blocking, the console pty was fixed in #294, and the
container-output pipe **disables capture entirely rather than register a
blocking fd** when `O_NONBLOCK` fails. The one `open(tty_path, O_RDWR)`
without it runs in a forked child after `setsid()`, so it can hang that child
and never the reactor. No blocking fd reaches epoll today.

**The supervisor worked, and the worker could not be restarted.** `cix-init`
came up as pid 1, spawned the worker, answered its out-of-band status port
while the worker was dead, and restarted the worker on demand — `restarts:
1 → 2`, a new pid, and a correct `"worker killed by signal 9"`. The new
worker never created its listener: ports 80 and 443 answered
connection-refused for ten minutes, and `stallwatch` starts *after* the
listener, so nothing was watching either. The box needed a reset.

The cause was not the mounts. **The whole `--init-mode` startup path assumes a
fresh kernel** — `diskrole_init()`,
`diskformat_remount_present_role_disks()`, `network_init()` and a dozen
`boot_subsystem_init()` calls have never run twice in one kernel lifetime,
because until now nothing could restart the worker. `boot_init()` was the
visible half of a much larger assumption.

**One property held throughout, and the whole design rests on it.** `dns-1`
and `dns-2` kept answering DNS on port 53 with the control plane dead and the
host at 0.4 ms ICMP. A control-plane failure is not a workload failure.

## Decision

**The defence is a reactor that does not block, held there by a gate. Not a
supervisor that restarts one that does.**

ADR-0246 sequenced the blocking work *last*, arguing that it reduces how often
a restart is needed but does not provide the restart. That optimised for
having a recovery path over not needing one, and it delivered neither: the
recovery mechanism produced a worse outage than the failure it was recovering
from, because a restart path that cannot restart is worse than none — it
invites the kill, and `DELETE` on the worker's own pid now looks like an
ordinary operator action.

So:

1. **Every fd the reactor polls is non-blocking**, and where that cannot be
   guaranteed the feature is declined rather than the fd registered. Audited
   above; this is the rule that both real incidents violated.

2. **The set of places the reactor can block is counted, not argued about.**
   `test_blocking_waits` carries a per-file budget with the reason each entry
   is currently acceptable. It deliberately does **not** claim the daemon
   never blocks — it asserts that the set of places that can is known, so
   adding one is a deliberate act visible in a diff. This is the same
   instrument as ADR-0224's toolchain count and #285's curl guards, chosen
   because this project has twice found that a prose rule does not hold and a
   number somebody must edit does.

3. **A restartable worker is not a goal of this phase, and it has a named
   prerequisite**: every step of the `--init-mode` startup path must be
   idempotent to a second start in one kernel lifetime. Until that is true,
   nothing above the worker can usefully restart it, and `init=/bin/cixd`
   stands.

`cix-init` is correct, it is proven to work, and it is not `init`. It goes
back in **behind** its prerequisite, not in front of it.

*(Factual correction, not a reversal. As first written this paragraph said
`cix-init` "remains built, staged and installed" — true that day, and made
false by this same ADR's own Consequences below, which removed it. The
decision is unchanged: the host's defence is a non-blocking reactor, and
`init=/bin/cixd` stands. Where `cix-init` went back in was the container, not
the host — see [ADR-0260](0260-a-container-declares-services-not-a-command.md).)*

## Consequences

**The daemon is harder to wedge than when ADR-0246 was written, and by a
mechanism that cannot silently rot.** The gate fails the build rather than
relying on anyone remembering the rule.

**The recovery path is gone again, deliberately.** A hang that the fd rule and
the wait budget do not prevent still requires a hypervisor reset, exactly as
before. That is a real regression against ADR-0246's *intent* and an
improvement against its *behaviour*, and it is the right trade until a worker
can start twice.

**`stallwatch` records and does not act.** Its `SIGUSR1` is gated on a
supervisor being present, which it is not, so the daemon's watchdog is back to
writing findings into a file readable through the API that is down whenever
they matter (#229). Known, and not fixed here.

**The supervisor is removed, and its three findings close with it** — #297
(a worker restart orphans build containers and other undefined children),
#298 (a slowly crash-looping worker never trips the fast-fail rollback), #299
(the status port binds `INADDR_ANY` unauthenticated).

That is the half of this decision that was left undone when it was first
written. The boot entry went back to `init=/bin/cixd` immediately, but
`cix-init` stayed built, staged into every bootroot and installed by the
recipe — a superseded implementation shipping on every host, executing
never, carrying three known defects. Dormant code with open bugs against it
is a parallel implementation and a stop-gap at once: it cannot be reasoned
about as live, it cannot be relied on as dead, and the three issues could not
be honestly closed or honestly fixed while it sat there.

So it is gone: `init/src/cix_init.c`, `include/supervisor.h`, the reap
channel and its `CONN_SUPERVISOR_REAP` connection kind, `container_adopt()`
and the registry's `adopted` bookkeeping, `stallwatch`'s `SIGUSR1`-to-pid-1
path, `hostproc_kill()`'s supervised self-kill allowance, and the
`mkbootroot` staging that put the binary in the image. `container_adopt()`
went with it rather than being kept for later: its only caller passed
`supervisor_reap_available()`, so with no supervisor it could never run, and
a container this daemon did not start cannot have its exit observed at all
once nothing above it is reaping.

**If a restartable control plane is wanted again, the prerequisite is
unchanged and now unencumbered**: `--init-mode` startup has to become
idempotent first. Rebuilding a supervisor against an idempotent startup path
is a smaller job than keeping a non-working one alive against a
non-idempotent one, and this ADR's own history is the record of why.

**ADR-0246 is superseded rather than edited**, per
[ADR-0000](0000-adr-process.md): the history of changing our mind is itself
worth keeping, and that ADR's own reasoning — including the parts the box
disproved — is the record of how this was arrived at.
