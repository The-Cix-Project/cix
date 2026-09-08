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

**`cmd` is removed. A container declares `services[]`, and the platform supervises them.**

A clean cut-over, in keeping with this project's standing no-backward-compatibility rule: `cmd` is
not deprecated alongside `services`, it is gone, a body carrying it is refused, and every container
recipe in this repository migrates in the same change. Two ways to say the same thing is exactly
the duplicate state One Source of Truth forbids.

**A service has a type.** `daemon` is long-running and supervised; `oneshot` runs to completion and
must exit 0. The oneshot kind is load-bearing rather than a convenience: `jump`'s start script does
real work before any daemon starts — `mkdir`, `ssh-keygen -A`, and hard-linking the nslcd socket
into sshd's privsep chroot — and without run-to-completion steps that work has nowhere to go and
the shell script survives. A model that only halves the problem is not worth the disruption of
changing the model.

**Ordering is `depends_on`, by service name, within the container.** Cycle-detected, exactly as the
container-level `depends_on` already is. Not numbers — see the alternatives below.

**A dependency waits for readiness, not for spawn.** A service may declare a `ready` probe: a
listening TCP port, a **unix socket path**, or a command that exits 0. `depends_on` waits for the
probe to pass when one is declared, and for "started" when it is not. The unix-socket form exists
because that is jump's real case — its script polls for `/run/nslcd/socket` for up to six seconds
today, and a dependency model that could not express that would have pushed the same retry loop
back into a wrapper script.

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
        depends_on: [hostkeys, nslcd]

**PID 1 is a supervisor this project writes in C**, which starts services in dependency order,
reaps them, applies each one's restart policy, and attributes output per service. Services appear
in `GET /containers/{name}` with their own state and exit reason, and one can be started, stopped
and restarted without disturbing its neighbours.

**`cix-init` reaches an image as an ordinary Cix package**, declared in that image's manifest the
same way `glibc` already is, and dynamically linked like everything else this project builds. No
exception to "never `-static`" is taken, and no new staging mechanism is invented.

The obvious objection is glibc skew — the supervisor is built once in `cix-builder` and installed
into images carrying other glibc revisions (2.44-14 and 2.44-16 both exist on the box today). Two
things answer it. The failure is directional: a binary built against an older glibc runs on a newer
one, and only the reverse breaks. And that reverse case is already gated — `daemon/src/elfcheck.c`
refuses to install a binary whose symbols do not resolve, so the failure surfaces at install time,
against a named package, rather than at `execve()` of PID 1 where it would present as a container
that never starts.

An image with no `cix-init` cannot run a container, and the daemon refuses at create time with a
message naming what to install — the same shape as the existing "image has no C library" refusal,
reusing that reasoning rather than inventing a second one.

**Stopping is the reverse dependency order.** Nothing else is defensible once ordering is a graph.

**A `ready` probe of the `command:` kind runs inside the container** as the same uid as the service
it probes, with a five-second default timeout, its output discarded and only its exit status read.
Stated here because a probe that could log, hang or run as someone else is three more things to
reason about during an incident.

**A daemon gets a pty only when it asks for one.** Attaching a console to a running service's stdio
([ADR-0261](0261-reaching-inside-a-container.md)) needs more than a plain pipe, but most daemons
neither want a tty nor behave the same when they see one, so it is declared per service rather than
given to every service by default.

**A failing daemon restarts alone; it does not cascade into its dependents.** Narrowing the blast
radius is the entire point of modelling services separately — a crash that took down every
dependent would reproduce the all-or-nothing restart this decision exists to remove. A dependent
that genuinely cannot survive its dependency restarting is a service whose own restart policy
should say so.

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
