# 0260 — A container declares the services it runs, not a single command

## Status

Proposed.

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

## Decision (proposed)

**A container declares `services[]` — named processes — and the platform supervises them.**

Each service carries a name, an argv, and its own lifecycle policy: restart behaviour, stop signal
and stop timeout, and `depends_on` naming *other services in the same container*. PID 1 becomes a
small supervisor this project writes in C, which starts services in dependency order, reaps them,
applies each one's restart policy, and attributes output per service.

The REST surface follows from that: services appear in `GET /containers/{name}`, with per-service
state and exit reason; a single service can be started, stopped and restarted without disturbing
its neighbours; captured output is retrievable per service.

`cmd` becomes the one-service shorthand for the common case, so nothing existing has to change.

Ordering is by **name**, through the `depends_on` this platform already has — not by number. That
is the direct answer to the shape the owner floated and distrusted (below).

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

If accepted, this is real work — a supervisor process, an API surface, per-service output
plumbing — and it is the kind that gets bigger the longer it waits, because every image that grows
a second daemon meanwhile writes another bespoke start script.

It also changes what a container *is* in this platform, from "a process in namespaces" to "a
supervised set of processes in namespaces". That is a genuine conceptual cost and the reason this
is an ADR rather than a ticket.

Until it is decided, `cr-1`/`cr-2` and `jump` keep their start scripts, and the limitation above
is real rather than theoretical: a bird crash on a router presents as a container restart with no
indication which daemon failed.
