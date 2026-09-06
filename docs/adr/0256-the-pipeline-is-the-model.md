# 0256 — The pipeline is the model

## Status

Accepted

Replaces `enum pkg_failure_kind` (issue #101) with a two-axis (stage, status) model. Builds on [ADR-0255](0255-a-recipe-is-a-rule-not-a-version.md), whose Resolve stage produced the first four stages, and on [ADR-0177](0177-pkg-build-resume.md)'s kept-on-failure build, which is what makes a failed stage recoverable rather than merely reported.

## Context

Getting a package from an upstream release onto a running host takes eleven distinct steps. This platform performs all eleven, and has never had a name for the sequence.

Instead it has four unrelated vocabularies, each true, none of which can see the others:

| Where | What it knows | What it calls a failure |
|---|---|---|
| `srcresolve` (ADR-0255) | Did upstream publish something we have no recipe for? | `pinned` / `current` / `missing` / `unresolved` |
| `g_packages[]` job records | Did the install work? | `enum pkg_failure_kind`: `RECIPE` / `FETCH` / `BUILD` / `INSTALL` / `CANCELLED` |
| Image rebuild queue | Did the images tracking this package rebuild? | a build-log line |
| Boot slots | Did the new root boot? | `cix-<slot>+<tries>.conf`, a filename |

An operator asking the only question that matters — *"where is this package, and what is stopping it?"* — has to ask four things and join them by hand. Nobody does that, which is how 25 of 70 installed packages drifted behind unnoticed until `/pkg/drift` was written to count them (issue #217), and how the source catalogue had to be written to answer the same question one stage earlier.

**The vocabularies are also wrong in a specific, load-bearing way.** `PKG_FAILURE_BUILD` covers unpacking the tarball *and* compiling it. So a source archive that downloaded intact and is not a valid archive reports as a build failure, and an operator goes and reads a compile log for a problem that happened before any compiler ran. `stage_main_source()` and the compile are separate code paths, separately detectable, reported as one thing.

Issue #101 already established the right instinct — it made the failure kind a *required* parameter precisely so a failure would say what happened, and ADR-0177's `CANCELLED` was split out of `BUILD` on the grounds that "the build was killed" and "the build did not work" call for opposite responses. This ADR is that instinct carried to its conclusion: if a failure must say what happened, then the list of things that can happen is the model, and it should be one list.

## Decision

**The pipeline is a first-class concept with one vocabulary, shared verbatim by the API, the CLI and the web dashboard.**

### The eleven stages

| Stage | The question it answers | Failure reads as |
|---|---|---|
| `discover` | What has upstream published? | could not check for versions |
| `resolve` | Which of those does policy want? | could not resolve a version |
| `authenticate` | Are these really upstream's bytes? | could not verify the signature |
| `author` | Is there a recipe for it? | no recipe for the resolved release |
| `fetch` | Can we download it? | could not download |
| `unpack` | Is the archive usable? | could not unpack |
| `build` | Does it compile? | could not compile |
| `install` | Does the output merge into the image? | could not install |
| `publish` | Does the artifact reach the cache? | could not publish |
| `roll` | Do the images that use it rebuild? | could not rebuild dependents |
| `deploy` | Does the new root boot and serve? | could not deploy |

The list is ordered and total: every failure this platform can have between "upstream released something" and "the host is running it" lands on exactly one of these. Adding a twelfth is a deliberate act with an ADR, not a quiet enum append.

### Status is a second axis, not more stages

```
status = ok | blocked | failed | cancelled | not-implemented
```

`cancelled` is what proves these are two axes rather than one longer list. A cancelled build (issue #213) is not a different *place* in the pipeline — it is the build stage with a different *outcome*, and the two need opposite operator responses from the same position. The same is true of `blocked` (a stage that cannot start because an earlier one has not finished) and `not-implemented` (a stage this platform performs by hand today — `authenticate` and `author` are both honestly this, and saying so is better than a green tick that means "we did not check").

`enum pkg_failure_kind` is **replaced**, not wrapped in a translation shim. Two vocabularies for one fact is exactly what this ADR exists to end, and a shim would preserve both indefinitely. The mapping is total: `RECIPE`→`author`, `FETCH`→`fetch`, `BUILD`→`build`, `INSTALL`→`install`, `CANCELLED`→ the `build` stage with `cancelled` status. `unpack` is genuinely new and is the one behaviour change: the `stage source` prep step reports as `unpack` rather than as `build`.

### A reason is a past-tense sentence naming the stage

Every non-`ok` position carries a reason, and the reason names what was attempted, not merely that something went wrong:

```
could not unpack linux-7.2.3.tar.xz -- not a recognised archive format
kernel.org release data has never been fetched on this host
depth asks for release 1 back within line 7.2, which publishes only 1 release(s)
```

This is the shape ADR-0255's catalogue already established. It is extended, not replaced by a second style.

### `GET /v1/pipeline` is a read-time join, not a store

Three owners already hold the truth: the source catalogue (stages 1–4), the package job records (5–9), the boot slots (11). The pipeline endpoint joins them at read time and stores nothing, for the reason ADR-0255 gave for the catalogue and ADR-0155 proved the hard way — derived state with no invalidation event goes stale silently, and a stale copy of "what is broken" is worse than no copy.

**Row grain: one row per package.** The catalogue is per-package because recipes are; builds are per-`(package, image)`. A row therefore reports the *worst* status across that package's images, with the per-image detail available underneath. This is decided here rather than per surface, because the CLI table and the web view both inherit it and they must not disagree.

### `deploy` confirms on reachability, not on binding

The `deploy` stage is only honestly `ok` when the host is actually reachable at the address an operator uses. Today `confirm_boot()` runs when cixd is "about to serve traffic" (`main.c`), which a box with no uplink NIC satisfies: issue #133 correctly stopped a missing NIC from killing PID 1, the management bridge and its address exist, cixd binds — and the boot is confirmed. The result is a host that is up, permanently confirmed, and unreachable, having spent its A/B fallback on a slot that cannot serve anyone.

So confirmation requires reachability on the configured management address, and a boot that cannot achieve it within a bounded time **reboots instead of confirming**, spending a try so the bootloader eventually falls back to the other slot. One guard is essential and is the whole safety of it: this applies **only while the boot is unconfirmed** (the entry still carries `+tries`). A confirmed slot whose NIC fails at runtime must never reboot itself — that would turn a cable fault into a reboot loop.

## Consequences

**The UI reorients around the pipeline.** Catalogue, Packages, Recipes and Build Pipeline are not four subjects; they are windows onto stages 1–4, 5–9 and 11 of one subject. The web view built on this endpoint is the first place that is visible.

**A contract changes.** `GET /pkg/{name}` and the package list stop carrying `failure_kind` and start carrying `stage` and `status`. Clients reading the old field see it disappear rather than silently misread it — the deliberate choice here, consistent with this project's no-backward-compat posture: a field that changes meaning is worse than a field that is gone.

**Two stages report `not-implemented` on day one.** `authenticate` and `author` are performed by hand. That is the honest state and it is the point: the pipeline view makes the manual steps visible as *positions in a sequence* rather than as absences, which is what turns "somebody should automate this" into a specific, locatable gap.

**What this does not do.** It does not schedule anything, does not open an update window, and does not know which images consume a package. Those act *on* this model and are deliberately after it — a scheduler over four unjoined vocabularies would have automated the confusion.

## Alternatives considered

**Keep the four vocabularies, add a joining view.** Rejected: the join would have to translate between them, which makes the translation a fifth source of truth and leaves the `BUILD`-means-unpack-or-compile ambiguity intact underneath. The vocabularies do not need a bridge; they need to be one vocabulary.

**One enum instead of two.** Rejected on the evidence of `CANCELLED`, which is already in the failure enum and is already not a failure kind. Folding status into stage would produce `build`, `build-cancelled`, `build-blocked` — the cross product, spelled out, which is how an enum stops being a list and starts being a table.

**Persist the pipeline state.** Rejected for ADR-0155's reason. A stored row would be authoritative-looking and wrong the moment a recipe was published.

**Confirm the boot on a successful external request instead of on reachability.** Rejected: a healthy box that nobody happens to call during the window would fail to confirm and fall back, which converts idleness into a rollback.
