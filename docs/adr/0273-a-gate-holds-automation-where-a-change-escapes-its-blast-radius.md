# 0273 — A gate holds automation where a change escapes its blast radius

## Status

Accepted

Issue [#371](https://git.home.arpa/itdlabs/cix/issues/371) (Deploy 4 of four, the last).
Builds on [ADR-0256](0256-a-package-has-a-position-in-a-pipeline.md) (the stage/status
vocabulary it reuses rather than extends), [ADR-0269](0269-one-pipeline-model-for-four-kinds.md)
(`blocked_on`), [ADR-0271](0271-an-action-is-attributable-to-a-person.md) (who approved),
and [ADR-0272](0272-a-pipeline-run-is-a-log-entry-not-join-state.md) (the store this
shares a file with).

## Context

Three things on this platform happen automatically, and each one is a point where a
change stops being local:

- `pkg_artifact_push_enqueue()` uploads a freshly built package to the shared cache.
  Every host that installs it inherits those bytes, checksum-verified and trusted.
- `pkg_try_start_queued_rebuild()` moves an image to a package version **nobody asked
  for** — a recipe was published, and the drain converges every image tracking it.
- `handle_system_update()` writes a control-plane root and a kernel onto a boot slot.
  The next reboot runs it.

Each is correct and each is deliberate. What none of them has is a place for a person
to stand. A recipe published at 02:00 rolls out to every image tracking it, and the
first opportunity to disagree is after it has happened.

The word for that pause is a gate, and the temptation is to build it as a workflow
engine. This ADR is mostly about what it is not.

## Decision

**A gate holds automation at the point where a change escapes its blast radius, and
nowhere else.**

Three gates, named for what they hold: `publish`, `roll`, `deploy`. They are booleans
in `pipeline-config`, **default false**, and a host with them off behaves exactly as it
did before this ADR — not approximately, exactly: the hold is a single test at the head
of each drain and a refusal in one handler.

There are three because there are three escape points, not because three is a tidy
number. A new gate needs a new escape point, which is the same test ADR-0269 applies to
a new pipeline kind.

### It reuses the status vocabulary rather than extending it

A held item is `blocked`, with `blocked_on: {kind: "approval", name: "<gate>"}`.

The first sketch of this work added `awaiting-approval` as a sixth `pipeline_status`.
That was wrong, and the reason is written on the existing enum:

```c
/*
 * Cannot proceed: waiting on something outside this stage -- an
 * earlier stage that has not finished, or a person.
 */
PIPELINE_BLOCKED,
```

`blocked` already means this, *including the person*, and `blocked_on` already exists to
say what is being waited on. A sixth status would have been a second way to express a
state the set already had — a parallel implementation inside the one closed vocabulary
ADR-0256 exists to keep closed.

### An approval is an input, not a position

ADR-0256 forbids storing a *position*, because a stored position is a second copy of
the present that drifts from the derived one. An approval is not a position. It is an
input to a decision, in the same class as `pipeline-config` itself, and storing it no
more violates that rule than storing the retention count does.

The split is kept exact:

- **What is waiting** is derived at read time from the queues and the gate flags, and
  is stored nowhere. Turn a gate off and nothing is waiting any more, with no state to
  reconcile.
- **What has been approved** is stored, and consumed when the thing it approved goes
  through.

### You cannot approve what nobody has asked for — except for deploy

`POST /v1/pipeline/approve` refuses with 409 a target that is not currently held. Pre-
approving something that has not arrived is a standing permission wearing the costume of
a decision, and it would make the audit line say a person approved a specific change
they had never seen.

**`deploy` is the exception, and the asymmetry is real rather than an oversight.**
`publish` and `roll` hold items that sit in a queue, so "currently held" is a fact that
can be read. An update is a single synchronous request with no queue behind it; there is
nothing to be in. So a deploy target — the `image_path` — is accepted as given, and the
refusal names the exact target to approve. Stating the asymmetry is better than
inventing a one-element queue to hide it.

### A held item must not starve what is behind it

`pkg_try_start_queued_rebuild()` looks at `g_rebuild_queue[0]` and pops it when the
image is satisfied. A hold implemented as "stop at the front" would mean one unapproved
image freezes every other image's convergence — a gate on one thing becoming an outage
for everything. Both drains therefore **skip** a held entry and continue the walk, and a
held entry costs one string comparison per pass rather than a rebuild attempt.

### Approving is an action with a name on it

Every approval writes an audit line naming the user (ADR-0271), the gate and the target.
That is the whole point of the pause: a record that a person, named, allowed a specific
change to leave its blast radius.

## Consequences

An operator can stop a rolling release without disabling the mechanism, look at what is
held, and let it through deliberately. With every gate off — the default, and what every
existing host gets — nothing changes at all.

**This is not four-eyes.** The same identity may request a change and approve it, which
means a gate constrains *automation*, not *a person*. That is still worth having: the
caller being held today is a token-bearing script (`pkg hostbuild --deploy`, or an
agent), and making it stop for a deliberate second call is exactly the pause being
bought. Requiring approver ≠ requester needs roles, which is
[#304](https://git.home.arpa/itdlabs/cix/issues/304)'s RBAC, and is deliberately not
built here.

**An approval cannot be revoked**
([#381](https://git.home.arpa/itdlabs/cix/issues/381)). For `roll` and `publish` this
mostly self-corrects — every path that takes an image out of the rebuild queue forgets
its grant, so approvals for abandoned work do not accumulate. `deploy` has no queue and
therefore no such path: its grant is cleared only by an update that uses it. An approval
typed against the wrong path stays, and a later update against that same path goes
straight through while the gate looks on. Recorded rather than quietly shipped as a
one-way door; the shape of the fix is a `revoke` taking the same `{gate, target}` and
writing an audit line the way granting does.

**A held item still lives in an in-memory queue.**
[#373](https://git.home.arpa/itdlabs/cix/issues/373) already records that the rebuild
queue does not survive a restart. Gates do not cause that and do not fix it; they make
it more visible, because an item held for a person is likely to sit there far longer
than one held for a build slot.

## Alternatives considered

**A gate per stage.** Eleven stages, eleven flags, and ten of them holding a change
inside its own blast radius where nobody needs to be asked. The three chosen are the
three that reach someone else.

**Approval as a workflow with states.** Requested, reviewed, approved, rejected, expired.
Every one of those is a stored position, which is what ADR-0256 spent its length
arguing against, and none of them answers a question this platform is being asked.

**Gate the recipe publish itself.** Tempting, because it is one endpoint. But publishing
a recipe is a *declaration* — it changes nothing until something converges on it — and
holding it would stop the catalogue from recording what exists. The thing worth holding
is the convergence, which is `roll`.

**Default on.** Safer sounding, and it would silently break every existing caller,
including this project's own deploy path. A gate nobody asked for that stops a release
at 02:00 is how gates get switched off permanently.
