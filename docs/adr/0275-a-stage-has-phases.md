# ADR-0275: A stage has phases

## Status

Accepted

Issue [#388](https://git.home.arpa/itdlabs/cix/issues/388). Extends
[ADR-0256](0256-the-pipeline-is-the-model.md)'s model and
[ADR-0269](0269-one-pipeline-model-for-four-kinds.md)'s stage vocabulary
one level down. Does not change either.

## Context

Building a package runs one fixed command, defined once in `pkg.c` and
identical for every package this platform has ever built:

```c
#define PKG_BUILD_CMD \
	"set -e; . /build/recipe.sh; cd /build/src; pkg_build; pkg_install; " \
	". /build/finalize.sh"
```

That string is a pipeline. It has ordered steps, the steps can fail
independently, and which step failed is the first thing anyone asks. It
appears **zero times** in `docs/api/openapi.yaml` — no surface reports
it, so every package build is one opaque atom: `build/ok` or
`build/failed`, plus a log truncated at ~4000 characters.

Two consequences we already live with.

**A word already means two things.** `pkg_install` runs *inside* the
build container, inside the pipeline stage called `build`. The pipeline
also has a stage called `install`, which merges an artifact into an
image. They are different operations at different levels wearing the
same name, and nothing in the model says so.

**A decision was made on a premise that is about to expire.** ADR-0269
rejected a `verify` stage for packages and gave the reason plainly:

> **It cannot be derived.** A recipe's self-tests run *inside*
> `pkg_build()` … so both failures are one event to the daemon:
> `pkg_build()` exited non-zero.

CPDL declares a `check` block. Once CBS runs a recipe's phases, "it
compiled" and "its tests failed" stop being one event, and the reason
ADR-0269 gave stops being true.

## Decision

**A stage may have phases. A phase is a step inside one stage.**

`phase` is the word, taken from CBS's own vocabulary rather than
invented here: `CBS_NODE_PHASE`, `cbs_build_plan()`, `CbsBuildPlan`. It
appears zero times in `daemon/src/pipeline.c` today, so nothing has to
move to make room for it.

**Phases carry the same status axis as stages** — `ok`, `blocked`,
`failed`, `cancelled`, `not-implemented` (ADR-0256's second axis). A
recipe that declares no `check` reports that phase as
`not-implemented`, which is the same answer the pipeline already gives
for a stage a thing does not walk, and the same device the dashboard
already draws as a dotted cell. No new vocabulary at either level.

**A phase is never reported without its stage.** This is the mitigation
for a real hazard rather than a style rule: CPDL's phase names include
`build` and `install`, so `phase: install` alone is ambiguous with the
stage of the same name, while `stage: build, phase: install` is not.
The two fields travel together or the phase is omitted.

**Only `build` has phases today**, and if another stage later gains
interior steps it uses this same mechanism rather than a private one —
which is ADR-0269's own rule about kinds, applied one level down.

### Where a phase lives

Two different questions, two existing homes, no new principle:

**Which phase failed** goes beside `stage` and `status` on the package
entry and on the run record (ADR-0272). It follows the rule already
written there: null when nothing is wrong, because "absence of a failure
is not a position in the pipeline, and a stage name here would invite a
reader to believe the package is sitting at it."

**Which phase is running now** is a live cursor, valid only while a
build is in flight. This is not a stored position and does not weaken
ADR-0256's read-time join: the durable state a position is derived from
does not contain it, and cannot. It is the same shape as
`last_output_seconds_ago`, which the entry already carries and which is
already documented as "null unless a build is in flight" — so the
precedent exists and this needs no new category.

### How the daemon learns the phase

The build child emits a marker line before each phase, and the daemon
recognises it in the build output it already captures (ADR-0087/0112).

This works for the shell recipes **today**, by making `PKG_BUILD_CMD`
announce the steps it already has, and it generalises unchanged to CBS
emitting the same markers for a declared plan. That ordering is
deliberate: this must not be a feature that only arrives when CBS does.

A marker in an output stream can be spoofed by a recipe that prints the
same line. That is acceptable **because a phase is never load-bearing**:
it is a diagnostic, never a gate, never an input to whether a build
succeeded. A spoofed marker misreports a diagnostic and cannot produce a
wrong build outcome. If a phase ever becomes a gate, this mechanism is
no longer sufficient and must be revisited before that happens.

## Alternatives considered

**Add a `verify` stage for packages, now that it can be derived.**
Rejected, and ADR-0269's decision therefore stands on a new basis rather
than being reversed. Two reasons. A stage is per-kind structure: adding
one perturbs `KIND_STAGES`, the page's column layout, and
`test_pipeline`'s count, for every kind — a large change to express one
package-local fact. And it is less precise than what phases give:
`stage: build, phase: check, status: failed` says which of a recipe's
*declared* phases failed, where a `verify` stage could only ever say
"something after compiling". The recorded reason in ADR-0269 does expire
when CBS lands; the conclusion does not.

**Model phases as nested pipelines.** Rejected. A pipeline in this model
is a *thing's journey* — one row per package, image, deployment or host.
A recipe's phases are not a thing's journey, they are one stage's
interior. Making them a pipeline would mean a package has two positions
at once, and "where is this package" would have two answers.

**Store the phase as durable state.** Rejected: it is exactly the stored
position ADR-0256 was written against. What phase a *finished* build
failed in is history and belongs on the run record; what phase a
*running* build is in is an observation with no meaning once the process
exits.

**Wait for CBS.** Rejected. The shell oneshot has three visible steps
right now and reports none of them; the CBS tracker asserts a
readiness the repository does not have (`itdlabs/cix-build-system` #117),
and its own TCC migration draft describes itself as "not yet a release
replacement for the shell recipe". Tying an observability fix to that
cut-over would leave the current pipeline opaque for as long as the
migration takes.

## Consequences

An operator gets which phase failed, rather than a truncated log to read
for it. A slow build says which phase is slow — a question this project
hit directly while bootstrapping cmake, where "building" for many
minutes carried no indication of what it was doing.

`stage: build, phase: check` becomes expressible, which is the thing
ADR-0269 said could not be derived. It arrives without a new stage and
without touching any kind's stage walk.

The fixed shell oneshot becomes visible as what it is: a three-step
plan, identical for every package, that nothing could see. Making it
visible is also what makes its replacement legible — a CPDL recipe
declaring `prepare/configure/build/check/install` reports more phases
than a shell recipe can, and the difference shows up in the same field
rather than in a migration note.

`phase` is additive. A client written against ADR-0256 or ADR-0269 keeps
working, in the same way ADR-0269's new keys did.

**What this does not do.** It does not make a phase a gate, it does not
give phases to any stage other than `build`, and it does not change what
`GET /v1/pipeline` derives — a thing's position is still exactly what it
was.
