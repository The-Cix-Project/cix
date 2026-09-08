# 0260 — A container declares the services it runs, not a single command

## Status

Accepted.

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

**`cix-init` is statically linked** — the single exception to `CLAUDE.md`'s "never `-static`", named
there and at its own link line. Two problems dissolve at once. It is built by the control-plane
build rather than as a package, so no build container needs it before it exists: the package route
considered first collided head-on with `daemon/src/pkg.c`'s own statement that build containers
*"already go through container_create() like any other"*, which made `__pkgbuild-<n>` a container
that would have required `cix-init` in order to build `cix-init`. And a binary that resolves nothing
at runtime cannot skew against whichever glibc an image happens to carry — 2.44-14 and 2.44-16 both
exist on the box today. The rule it excepts exists so Cix binaries track the platform's glibc; a
PID 1 whose whole job runs before the image's userspace is the one binary that must not.

**The daemon stages it at container-create time**, copying it into the container's tree as it
already stages `files[]`. Not at image-seed time: ADR-0155 means a same-manifest reinstall would
silently discard the reseed. Create-time staging has no such trap, no manifest to forget, and
guarantees a container runs the `cix-init` its own daemon shipped with.

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
