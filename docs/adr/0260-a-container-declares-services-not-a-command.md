# 0260 — A container declares the services it runs, not a single command

## Status

Accepted
Decided by the owner after reading the proposal: *"i want to remove cmd and make it service, and I
want to be able to make services depend on other services right? so cmd gets dropped"*, together
with the four follow-up answers recorded in the Decision below.

Raised by the owner, looking at a real router: *"the cmd is /usr/sbin/keepalived -n -l -D -P -f
/etc/keepalived/keepalived.conf ...? I am not sure this makes sense to me since we run bird,
keepalived and maybe will run other stuff on the box."*

Pairs with [ADR-0261](0261-reaching-inside-a-container.md), which covers the other half of the
same muddiness — how an operator reaches in.

## Context

A container has exactly one `cmd`, an argv `execve()`d as PID 1. That was true when a container
meant one daemon, and it is no longer true: `cr-1`/`cr-2` run keepalived *and* bird, and `jump`
runs sshd *and* nslcd.

The workaround is a shell script inside the image that backgrounds both and `wait -n`s. **It is
worse than it looks, and the measurement makes the case better than the argument does.** Both
routers, read from the API on the same box:

    cr-1   cmd = /usr/sbin/keepalived -n -l -D -P -f /etc/keepalived/keepalived.conf ...
    cr-2   cmd = /usr/bin/bash /usr/local/bin/router-start.sh

`cr-1` is the old, single-daemon recipe and it is *honest* — it names what runs. `cr-2` is the
new one and names **nothing that actually runs**. Adding a second daemon did not just fail to add
information; it destroyed the information that was there.

What the platform gives up, all of it a consequence of seeing one PID where there are two or more:

- **Which one died is unanswerable.** `wait -n` returns, the script exits, the container restarts.
  `exit_reason` describes bash. An operator is told the container restarted, never that bird
  crashed and keepalived was fine.
- **Output has no attribution.** `capture_output` interleaves both daemons' stderr into one
  stream with nothing marking which wrote which line.
- **Restart is all-or-nothing.** Bouncing bird means bouncing keepalived, which means a VRRP
  transition to fix a routing daemon.
- **Ordering lives in shell.** jump's script already sleeps in a loop waiting for nslcd's socket
  before starting sshd. That is a dependency, expressed as a retry loop, in a file inside an
  image, where nothing can see it.
- **The supervision policy is per-image and unreviewable.** Every image that needs two daemons
  reinvents it, and the quality of a container's process supervision depends on who wrote its
  start script.

The platform already models the analogous thing one level up: containers have `depends_on`, with
cycle detection and readiness gating. Inside a container there is no model at all.

## Decision

**`cmd` is removed. A container declares `services[]`, and `cix-init` supervises them.**

A clean cut-over, per this project's standing no-backward-compatibility rule: `cmd` is not
deprecated alongside `services`, it is gone, a body carrying it is refused, and every container
recipe in this repository migrates in the same change. Two ways to say the same thing is exactly the
duplicate state One Source of Truth forbids.

### `cix-init` is PID 1 in every container, without exception

Whatever a container declares — one bare daemon or seven services with dependencies — the process
the kernel starts is `cix-init`, and it starts the declared services. There is no second shape for
PID 1 and no predicate choosing between them; a boundary like that only gets drawn in the wrong
place later.

**The bash wrapper is gone as a concept**, not merely discouraged. A start script that backgrounds
daemons and `wait -n`s is not a pattern this platform has any more, and `jump`, `cr-1` and `cr-2`
lose theirs.

**`cix-init` is freestanding** — compiled `-nostdlib -static`, with its own `_start`, its own
syscall trampoline and its own `memcpy`, linked against nothing, including no header. This is the
single exception to `CLAUDE.md`'s "never `-static`", named there and at its own link line. It was
first written as "statically linked", meaning against glibc, and that route measured closed:
`probe-tcc-conformance/25` found no `libc.a` anywhere on a Cix host, by design — ADR-0251 keeps
static archives out of every artifact because the platform links dynamically always — so a
static-glibc `cix-init` would have needed a superseding ADR and a glibc rebuild for one binary.
`probe-tcc-conformance/26` and `27` then measured that the pinned tcc builds a freestanding
executable that runs: a 4860-byte binary with no `PT_INTERP`, no `DT_NEEDED` and no undefined
symbol, through a delivered signal, fork, execve, poll and wait4. Two problems dissolve at once. It is
built by the control-plane build rather than as a package, so no build container needs it before it
exists: the package route considered first collided head-on with `daemon/src/pkg.c`'s own statement
that build containers *"already go through container_create() like any other"*, which made
`__pkgbuild-<n>` a container that would have required `cix-init` in order to build `cix-init`. And a
binary that includes no header and resolves nothing at runtime cannot skew against whichever glibc
an image happens to carry — 2.44-14 and 2.44-16 both exist on the box today. The rule it excepts
exists so Cix binaries track the platform's glibc; a PID 1 whose whole job runs before the image's
userspace is the one binary that must not. **The freestanding syscall layer is `cix-init`'s own and
nothing else in Cix may use it**: a second freestanding binary is not to appear by imitation, and
`init/src/cix_init.c`'s header says so where the next reader will look.

**The daemon stages it at container-create time**, copying it into the container's tree as it
already stages `files[]`. Not at image-seed time: ADR-0155 means a same-manifest reinstall would
silently discard the reseed. Create-time staging has no such trap, no manifest to forget, and
guarantees a container runs the `cix-init` its own daemon shipped with.

### There is one `cix-init`, and it is the one that was removed

`cix-init` is not a new program. It was written for [ADR-0246](0246-the-control-plane-is-one-process-doing-five-jobs.md),
ran as the machine's pid 1, and was deleted in `594ec1fc` once [ADR-0247](0247-the-reactor-does-not-block-and-that-is-the-defence.md)
superseded that design — deleted precisely because it *shipped to every host and executed on none*,
which is the definition of a parallel implementation this project does not keep. The 469 lines are
recoverable at `594ec1fc^:init/src/cix_init.c`, together with `include/supervisor.h`, the
`CONN_SUPERVISOR_REAP` channel and `handle_supervisor_reap_event()` on the daemon side.

This ADR revives that code for the container role, and doing so **resolves** the objection that
removed it rather than repeating it: the binary now runs, on every container, as the only thing it
is for. Three properties of it were proven on the box and are inherited rather than re-derived — the
`waitpid(-1, &status, WNOHANG)` drain that runs unconditionally on every loop turn rather than only
when a `SIGCHLD` flag is set (coalesced signals make the flag lossy), the bounded pending-record ring
with an explicit dropped count, and `struct supervisor_reap_record`'s `exit_kind`/`exit_value` field
names, which exist because glibc defines `si_status` and `si_pid` as macros.

**It also settles the question of whether this platform now has two supervisors. It does not.** The
host has none: ADR-0247 chose a reactor that cannot block over a supervisor that restarts one that
did, and `init=/bin/cixd` stands. So `cix-init` is the platform's single supervisor, in the single
place a supervisor is warranted, and there is nothing for it to be a parallel of. Should the host
ever acquire one — ADR-0247 names its prerequisite, an `--init-mode` startup path idempotent to a
second start in one kernel lifetime — the decision to make then is *reuse*, not a second program.

The three defects the removed code carried do **not** come back with it, because all three were
properties of the host role rather than of the program: #297 (a worker restart orphans build
containers) and #298 (a slowly crash-looping worker never trips the fast-fail rollback) both
describe supervising `cixd` across a boot slot, which is not a thing a container supervisor does at
all. #299 (the status port binds `INADDR_ANY` unauthenticated) is dissolved by the transport this
ADR chooses rather than inherited and fixed: the service table and the reap stream travel over fds
inherited across `clone3()`, so there is no port to bind, no listener inside a container, and
nothing on the network to authenticate. Stage A must therefore not revive the status-port code, and
that is the one part of `594ec1fc^` deliberately left where it is.

### What a service is

A service has a type. `daemon` is long-running and supervised; `oneshot` runs to completion and must
exit 0. The oneshot kind is load-bearing rather than a convenience: `jump`'s start script does real
work before any daemon starts — `mkdir`, `ssh-keygen -A`, and hard-linking the nslcd socket into
sshd's privsep chroot — and without run-to-completion steps that work has nowhere to go and the
shell script survives. A model that only half-solves the problem is not worth changing the model
for.

**Service-level fields are named so they cannot be confused with container-level ones.** Container
vocabulary stays exactly as deployed — `depends_on`, `readiness`, `restart`. Services get `after`
(ordering within the container), `ready` (the probe), and `on_exit` (`restart` / `stop` /
`fail-container`). `depends_on` therefore always means containers and `after` always means
services, and no interface needs prose to disambiguate them.

**A restart delay is declared, never computed.** `on_exit: restart` carries an explicit
`restart_delay_seconds` alongside it, defaulting to the same value the container-level `restart`
already uses. This is not a tuning knob — it is the line that keeps `cix-init` an executor. The
moment the supervisor derives its own backoff schedule it is holding policy that no declaration
states and no operator can read, inside the one process this design deliberately gives no opinions
of its own. Its whole vocabulary stays: start this, stop that, wait this long, run this probe.

    services:
      - name: hostkeys
        type: oneshot
        cmd: ["/usr/bin/ssh-keygen", "-A"]
      - name: nslcd
        type: daemon
        cmd: ["/usr/sbin/nslcd", "-d"]
        ready:
          socket: /run/nslcd/socket
      - name: sshd
        type: daemon
        cmd: ["/usr/sbin/sshd", "-D", "-e"]
        after: [hostkeys, nslcd]

**`after` waits for readiness, not for spawn.** A service may declare a `ready` probe: a listening
TCP port, a **unix socket path**, or a command that exits 0. A dependent waits for the probe to pass
when one is declared, and for "started" when it is not. The unix-socket form exists because it is
jump's real case — its script polls `/run/nslcd/socket` for up to six seconds today, and a model
that could not express that would have pushed the same retry loop back into a wrapper.

A `command:` probe runs inside the container as the same uid as the service it probes, with a
five-second default timeout, its output discarded and only its exit status read. A probe that could
log, hang, or run as someone else is three more things to reason about during an incident.

### How the layers compose

**Container readiness is derived from service readiness**: a container is ready when every service
with a probe has passed it and every oneshot has exited 0. `cix-init` reports each service's state
on its control pipe and the daemon derives the container's from those reports. This keeps the
existing meaning of container-level `readiness` and `depends_on` intact — `jump` depending on
`ldap-1` still waits for glauth to accept connections. Defining container readiness as "`cix-init`
is up" was considered and rejected: `cix-init` is up in milliseconds, before any service starts, so
every cross-container dependency on the box would fire early — the nslcd-socket race, moved one
level up.

**Container-level `restart` means "`cix-init` died or errored".** Service restarts are `cix-init`'s
job; container restart is the daemon's. The two do not overlap.

**A failing daemon restarts alone.** No cascade into its dependents: narrowing the blast radius is
the whole reason for modelling services separately, and a crash that took down every dependent would
reproduce the all-or-nothing restart this decision exists to remove. A dependent that genuinely
cannot survive its dependency restarting says so in its own `on_exit`.

**A failed `oneshot` fails its dependents.** `cix-init` reports it and exits non-zero, and
container-level restart applies — `on_exit: fail-container` is the oneshot default. `ssh-keygen -A`
failing must not leave sshd starting with no host keys.

**`cix-init` exits when there is nothing left to supervise**, with the status of the last daemon to
exit. A supervisor idling over nothing would report the container `running` while it does nothing at
all, which is the "201 running only proves the child `execve()`'d" trap this project has already
paid for twice — the dnsmasq pidfile crash-loop, and the router with no `/usr/bin/bash`. Exiting
also makes a oneshot-only container a job, for free.

### Stopping, and output

**Stopping is the reverse dependency order**, each service getting its own stop signal and timeout.
Nothing else is defensible once ordering is a graph.

**`capture_output` stays one container-level flag meaning "capture everything"**, with output
attributed per service on the way in. The daemon creates one pipe per service before `clone3()` and
adds the read ends to its own epoll set — the same mechanism `container_net_child_configure()`'s
ready-pipe already uses — so attribution costs nothing extra and the existing merged
`captured_output` view survives, each line carrying the service that wrote it.

**`cix-init` is not in the console path.** Console sessions remain the daemon's own work, entering
the container's namespaces from outside via `setns()` exactly as now. The one exception is the
`service:` console kind in [ADR-0261](0261-reaching-inside-a-container.md), which needs the
supervisor to hand out a running service's stdio; that is deliberately out of the first cut, since
the `cmd:` kind covers everything done today including the `birdcl` session that diagnosed bird.

Nor is `cix-init` an init system: no socket activation, no timers, no logging daemon, no host
service management. It reads no configuration from inside the image — its service table arrives from
the daemon over an inherited fd — writes nothing to disk, and does no networking.

### A stop is an override, not an edit

Once services can be started and stopped over REST, the declaration and the running state can
disagree, and that disagreement is the classic way a control plane grows a second source of truth.
Two models are coherent and only one of them is ours.

**The declaration is the only truth. An operator action is a visible, bounded override.** Stopping a
service records it as `stopped-by-operator` — a state distinct from `exited` and from `failed`, so
`GET /v1/containers/{name}` reports both what was declared and what was done to it, and reading the
API never conceals an intervention. The override's lifetime ends at the next container start, which
therefore always reconstitutes the declared set. Drift cannot outlive a restart, and nothing an
operator does over REST ever rewrites the persisted creation body.

The rejected alternative is that a stop edits the declaration. It reads as tidier — one state, no
override concept — and it is the trap: the container's definition then says something its recipe
never said, `pkg apply-recipe` becomes a diff nobody authored, and the recipe stops describing the
container it created.

This is exactly what `container stop` already means one level up: stopping a container does not
unpersist it. The service level inherits that meaning rather than inventing a second one, and
`cix-init` stays an executor here too — an override reaches it as a message on the control fd, not
as a rule it holds.

## Alternatives considered

**Numbered rc-style start/kill scripts (`cmd 1..999`), the owner's own suggestion — and the
distrust was right.** A number is a convention, not a model. It encodes ordering without encoding
*why*, so nothing can validate it: there is no cycle to detect because there is no graph, and
inserting something between 20 and 30 is a renumbering exercise. It also keeps supervision in
scripts inside images, which is the actual problem rather than a detail of it. This project
already rejected the equivalent shape one level up — containers order by `depends_on`, not by an
integer — and having two different ordering models in one system would be its own defect.

**One process per container, with a shared network namespace.** The container-orthodoxy answer,
and genuinely attractive: bird and keepalived become two containers that happen to share a netns.
The missing primitive is smaller than a supervisor — a container-level `netns: <other container>`
plus a join at `clone3()` time. What rules it out is that it solves neither problem this ADR
exists for: two containers sharing a netns still have no ordering relationship *within* it beyond
the container-level `depends_on` they already have, and every co-located pair doubles the
container count, the recipes, and the addresses to reason about. It also does not help `jump`,
where sshd and nslcd must share a *mount* namespace (the hard-linked socket under the privsep
chroot), not just a network one. Worth revisiting on its own merits later; it is not this
decision.

**A third-party init inside the image (s6, runit, tini).** Solves supervision, and this project
does not take it: the Technology Stack is native primitives, the supervisor would be a runtime
dependency in every image that needs two processes, and — decisively — it would put supervision
back inside the image where the daemon cannot see it. The platform would still report one PID.

**Keep the shell script and document it.** The status quo. Rejected on the evidence above: it is
not neutral, it actively removes what the API reported before.

## Consequences

This is real work — a supervisor process, an API surface, per-service output
plumbing — and it is the kind that gets bigger the longer it waits, because every image that grows
a second daemon meanwhile writes another bespoke start script.

It also changes what a container *is* in this platform, from "a process in namespaces" to "a
supervised set of processes in namespaces". That is a genuine conceptual cost and the reason this
is an ADR rather than a ticket.

Every container recipe in this repository changes, and so does every test that creates a container.
That is the cost of a clean cut-over and it is paid once.

Until the supervisor ships, `cr-1`/`cr-2` and `jump` keep their start scripts, and the limitation
above is real rather than theoretical: a bird crash on a router presents as a container restart with
no indication which daemon failed.

**Stopping a container is a message first and a signal second.** `CIXINIT_OP_SHUTDOWN` on the control
socket is the authoritative channel; `SIGTERM` goes alongside it only as a fallback. This is not
belt-and-braces — a signal alone is *wrong* here. The kernel treats PID 1 of a PID namespace as
`SIGNAL_UNKILLABLE` and **discards** any signal it has not yet installed a handler for, so a `SIGTERM`
landing in the window between `execve()` and `cix-init`'s own `sigaction()` is gone, not queued, and
the container then sits until the `SIGKILL` escalation. Found the hard way: every container stopped in
under a second except the one a test created and deleted milliseconds apart, which hung for the full
grace, every run. The command has no such window because a socket buffer keeps it until the reader
exists.

**A build container is the one place where a container's stdio and a service's output are the same
descriptor, and that made it the only place this design could break the build path.** A build
container has exactly one service, whose output *is* the build log the daemon already holds, so
`init_transport_open()` is given that write end as the shared output fd rather than making a pipe
per service. It is therefore simultaneously `spec->stdout_fd` (capture) and a member of
`spec->keep_fds` (cix-init's argv) — and `container_create()`'s pre-exec setup closed the original
`stdout_fd` after dup2-ing it onto 1 and 2, a tidy-up written years before anything needed that
descriptor to survive. The `fcntl(F_SETFD)` loop then ran against a closed fd and every package
build on the platform died with `EBADF` before producing a byte. Ordinary containers were unaffected,
because they get a pipe per service and their capture fd is a different descriptor entirely — which
is exactly why eleven containers could be verified running and ready by a daemon that could not build
anything. Fixed by never closing a descriptor the spec names as kept; recorded here because the
coincidence is a property of the design, not an accident of one function.

**The migration preserves each container's existing behaviour exactly, and per-service restart is opt-in.**
Every service translated from an old `cmd` carries `on_exit: fail-container` — so its exit still ends
the container with its status, and the container-level `restart` policy still decides what happens
next, which is what one `cmd` did. `on_exit: restart` is strictly better for most of them and is
deliberately *not* applied by the migration: arriving as a side effect of a translation is how a
behaviour change ships unmeasured. `cr-1`/`cr-2` are the case that makes this concrete — their shell
wrapper's own comment argued that a router whose routing daemon has died must not keep advertising
VRRP, and `fail-container` on both services is that argument, now stated in the declaration instead
of implied by `wait -n`.

The first cut of this migration translated everything to `oneshot` instead, and the selftest refused
it. A oneshot is ready only once it has **exited 0**, so a long-running process declared as one never
becomes ready — and since autostart blocks on readiness before the event loop, the daemon answered
nothing for minutes after a restart. Worth stating because the mistake is available to anyone reading
"one process, and its exit is the container's" and reaching for the type whose name says
run-to-completion.

**Autostart blocks on a container's readiness only when another definition depends on it.** Every
second there is a second the daemon serves nothing. The container-level check this replaced was
declared by almost nothing, so the cost was rare; a derived readiness that every container has makes
waiting the default, and waiting on a container nobody is waiting for is startup latency paid on
every boot.

**The cut-over is a dependency-ordered rolling recreate, not a loop over the container list.** Every
container must be recreated to gain a pid 1, and the box runs containers that depend on each other
and a VRRP pair that must not lose both members at once. The ordering that makes this safe is the
`depends_on` graph the platform already holds, and the pacing is the jittered rolling-restart timer a
rolling image update already uses. No new machinery — the deploy uses both rather than iterating
blindly.

**The dashboard's container detail view gains a services panel**, and it is part of this work rather
than a discovery after it: services as rows carrying state, readiness and last exit, with start and
stop per row. It is also where [ADR-0261](0261-reaching-inside-a-container.md)'s console list is
rendered, since the set of things a container runs and the set of ways to reach into it are one
panel, not two. This is the visible form of what the model actually buys — the control plane's reach
now extends to the operation of the software inside a container, not merely to the container.
