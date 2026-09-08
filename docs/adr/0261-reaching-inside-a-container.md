# 0261 — Reaching inside a container: what a console is, what exec is, and what you find there

## Status

Accepted.

Decided by the owner: *"console is also a list, no dependencies, but should be very specific and
cannot be freetext, and in the webui will show a drop down, and cli will also, exactly and
consistently as we are doing everything"*, plus the two answers recorded in the Decision below.

Raised by the owner: *"the console seems to allow any arbitrary command, even though I try with
bash, and it does not allow me to see things, I think access to the containers and how stuff is
started and stopped is a little muddy."*

Pairs with [ADR-0260](0260-a-container-declares-services-not-a-command.md), which covers how
things are started and stopped. This one covers reaching in.

## Context

**The console is not broken, and establishing that first matters, because it moves the decision.**
Measured on `cr-2`, this box, this week:

    $ cixctl console cr-2 --cmd=/usr/bin/bash
    bash-5.2# ls
    bash: ls: command not found

bash started, ran as PID 1's sibling in every one of the container's namespaces, and its builtins
worked. `ls` is absent because the `router` image has no coreutils — and that is deliberate, stated
in the image recipe's own comment: *"No iproute2, no shell tooling for debugging."* The endpoint
did exactly what it promises. What is missing is anything that says what an operator should
*expect*.

Two mechanisms exist and are easy to conflate:

- **`consoles[]`** — attach points a container *declares*. `GET /containers/{name}/console`
  without `cmd` attaches to one. A container declaring none has no console (issue #248).
- **`?cmd=<absolute path>`** — exec an arbitrary program in the container's namespaces. It works
  on a container that declares no console at all, deliberately: as `openapi.yaml` puts it, anyone
  authorised to reach this endpoint can already run arbitrary code in the container, so gating it
  behind a declaration would be theatre.

Both arrive over the same endpoint, and the second overrides the first. So "console" names two
different operations — attaching to something the container offers, and starting something new —
and nothing distinguishes them for an operator. That is the muddiness, and it is a naming and
model problem rather than a defect.

Two further gaps make it sharper:

- **What you can attach to is unmodelled.** Once ADR-0260 exists, "attach to the bird service's
  stdio" is a thing an operator will want, and there is no vocabulary for it — `consoles[]`
  predates any notion of a service.
- **A minimal image contains nothing to inspect with, by design.** Every image here is its declared
  packages plus a deliberate baseline. `cr-2` has bash only because a start script needs one
  (ADR-0260); `dns-1` has no shell at all. So the honest answer to "console in and look around" is
  currently "there is nothing in there to look with", and nobody has decided whether that is fine.

## Decision

**Consoles are a declared list. There is no free-text exec.** `?cmd=<arbitrary path>` is removed;
an operator attaches to one of the entries the container declares, chosen by name. Consoles have no
dependencies on one another and no ordering — they are not services, they are ways in.

This is not a security change and is not argued as one: as `openapi.yaml` already says, anyone
authorised to reach this endpoint can run arbitrary code in the container, so gating exec behind a
declaration would be theatre. It is a **model** change. A declared, enumerable set is something the
CLI and the dashboard can both present identically — a dropdown, the same names, in both — and a
free-text path is something neither can offer consistently. Consistency across surfaces is the
reason, and it is the same reason every other resource here is declarative.

**An entry points at one of two things, and says which.**

    consoles:
      - name: birdc
        cmd: ["/usr/sbin/birdc"]      # run a program
      - name: bird
        service: bird                  # attach to a running service's stdio

Both kinds are real needs and neither subsumes the other: diagnosing bird today needed a separate
control program, while watching a daemon's live output is attaching to something already running.
After [ADR-0260](0260-a-container-declares-services-not-a-command.md) the second form has a name to
point at.

**`birdc`, not `birdcl`, and the difference is the terminal.** This example named the light client
until it was checked against what the console actually provides: `daemon/src/exec.c` allocates a
real PTY from the *container's own* devpts instance and propagates window size. `birdc` is BIRD's
readline client — line editing, history, completion — and `birdcl` exists precisely for
environments with no readline and no terminal. Given a PTY, the readline client is the one worth
declaring. Both are present in the `router` image already (measured on `cr-2`), and the recipe
carries `libreadline` specifically so that `birdc` links.

`birdcl` keeps its place on the other path: `POST /containers/{name}/exec` is a **pipe**, not a
PTY, so a one-shot scripted `birdcl show protocols` is the right tool there and a readline client
is not. The two clients map onto the two mechanisms this ADR names apart, which is a small piece of
evidence that the split is the real one.

A router should declare a third entry for the same reason ADR-0260 exists: **keepalived has no
control client at all**, so `service: keepalived` is the only way to see it. "Which daemon died"
was the question that started this pair of ADRs, and an attach-to-stdio console is half the
answer.

**Inspecting a minimal container is an image concern, not a platform capability.** A container is
reachable through what it declares; if an image is to be inspected with a shell, that image installs
one and the container declares a console for it. The platform does **not** mount a toolbox into a
running container's namespaces.

This is the owner's call and it is deliberately the more austere of the two options considered. Its
cost is stated plainly rather than glossed: **debuggability becomes a build-time decision, made
before the incident.** `dns-1` today has no shell at all, so under this decision it cannot be
inspected interactively until its image gains one and its recipe declares a console — and the
moment when someone wants that is precisely the moment it is too late to add it. The trade accepted
in exchange is that images stay minimal and auditable, and that there is no new privileged
mount-into-a-running-namespace surface to reason about.

## Consequences

The two acts are named apart, which is what makes the model teachable: attaching to something a
container offers is not the same as starting a new program inside it, and after ADR-0260 they have
different targets as well as different failure modes.

`cixctl` and the dashboard both enumerate the declared set, so an operator sees the same names in
both places and cannot type a path that only works in one of them.

Every existing container recipe that relied on ad-hoc exec for diagnosis must declare the consoles
it actually wants -- and today not one of them declares any, `cr-1`/`cr-2` included, so removing
free-text exec without that pass would leave the routers with no way in at all. They gain `birdc`,
`bird` and `keepalived` consoles for the work that found this; anything that
expects a shell must have one installed by its image. A container that declares nothing has no
interactive access at all, by design.
