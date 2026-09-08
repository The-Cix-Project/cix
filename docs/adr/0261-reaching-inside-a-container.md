# 0261 — Reaching inside a container: what a console is, what exec is, and what you find there

## Status

Proposed.

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

## The decision this ADR exists to make

**Is inspecting a minimal container a platform capability, or an image concern?** Everything else
here follows from that answer, and it is the owner's to give.

**(a) Image concern — "install coreutils where you want it."** Honest and cheap; keeps the
minimality that makes these images small and auditable. Cost: an operator cannot look inside a
running container that was not built in advance to be looked inside, which is exactly when they
most want to. Debugging capability becomes a build-time decision made before the incident.

**(b) Platform capability — an inspection session brings its own tools.** The daemon mounts a
read-only toolbox into the container's mount namespace for the life of the session and execs a
shell from it, so the image stays minimal and any container can still be inspected. This is the
shape `kubectl debug` settled on, for the same reason. Cost: a real feature — a toolbox image the
platform builds and ships, a mount into a running container's namespace, and a new privilege
surface to reason about.

**(c) Status quo plus documentation.** Say plainly what a console is, what `cmd` is, and that a
minimal image has no tools. Cheapest, changes no code, and leaves the operational gap exactly
where it is.

## Also proposed, independent of that answer

Whatever is chosen, **the two operations should be named separately** rather than sharing one
endpoint with an overriding parameter: attaching to something the container offers is not the same
act as starting a new program inside it, and they have different failure modes, different audit
meaning, and — after ADR-0260 — different targets. Naming them apart is what makes the model
teachable.

## Consequences

Option (b) is the only one that makes a minimal image and a debuggable container both true at
once; it is also the only one with real cost. Option (a) is coherent but should then be *stated*,
so nobody expects otherwise mid-incident. Option (c) resolves the confusion without resolving the
gap.

Deliberately not decided here, because the trade-off is the owner's: it is a choice about how this
platform expects to be operated, not an engineering detail.
