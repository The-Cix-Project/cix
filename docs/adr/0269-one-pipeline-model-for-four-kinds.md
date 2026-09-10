# 0269 — One pipeline model for packages, images, deployments and the host

## Status

Accepted

Issue [#371](https://git.home.arpa/itdlabs/cix/issues/371). Generalises
[ADR-0256](0256-the-pipeline-is-the-model.md), whose model was correct and
package-only.

## Context

ADR-0256 built the right machine: one stage vocabulary, status as a second axis,
a read-time join that stores nothing. It covered packages, because that is what
the drift incident (#217) was about.

But a package is not the only thing this platform delivers. An image is composed
of packages. A deployment runs an image. The host boots a root built from all of
it. Each can be stuck, each fails in its own way, and **none of them had a
vocabulary at all** — which is precisely the state ADR-0256 found packages in and
fixed.

The pressure to generalise badly is real and immediate: the easy move is a stage
list for images and another for deployments. That reintroduces the exact failure
ADR-0256 exists to end — several vocabularies, each true, none able to see the
others.

## Decision

**One stage enum, one status axis, four kinds that each walk a SUBSET.**

```
package     discover resolve authenticate author fetch unpack build install publish roll
image       author resolve acquire install publish roll
deployment  author resolve acquire deploy verify
host        build assemble stage deploy
```

`author` means the same thing for a package recipe and an image recipe. An
operator learns the word once; a renderer draws it once. The subsets overlap
heavily and that is the point, not duplication.

**Four stages are added, and each is a position something can genuinely be stuck
at that no existing stage describes:**

- **`acquire`** — can the thing this depends on be had? *This is where forks
  live.* Acquiring may mean taking a cached artifact, or starting the pipeline
  that produces it; while that runs, this pipeline is `blocked` here with
  `blocked_on` naming what it waits for.
- **`assemble`** — host only. `GET /system/assembly` has its own generation
  counter precisely because assembling a control-plane root fails on its own.
- **`stage`** — host only. `POST /system/update` answers
  `{"status":"staged","slot":...}`, a distinct outcome from both assembling a
  root and booting it.
- **`verify`** — deployment only. A container that `execve()`'d successfully and
  then died on its first file I/O reports running for a moment and ready never.
  That was the dnsmasq pidfile crash-loop's entire disguise, and `deploy`-vs-`ok`
  cannot express it.

**`deploy` moves out of the package pipeline** into the host's. ADR-0256 already
reported it *once* beside the package list rather than per package; this makes
that structural rather than a special case.

**Two candidate stages were rejected after measurement**, and recording why is the
point of an ADR:

- **`compose`, for images.** Composing an image *is* installing its packages.
  `install` already says that, and a second name for one thing is the
  two-vocabularies problem in miniature.
- **`verify`, for packages** — separating "it compiled" from "its own tests
  passed". **It cannot be derived.** A recipe's self-tests run *inside*
  `pkg_build()` (tar's round-trip gate, cix's own #296 proof), so both failures
  are one event to the daemon: `pkg_build()` exited non-zero. Distinguishing them
  needs a separate `pkg_verify()` recipe hook, which is a different and larger
  change. The related claim that elfcheck failures report as `build` was also
  false: `elfcheck_undefined_builtin()` is called from the merge path and returns
  `merge_fail()`, so it already reports as `install`, correctly.

### Edges are structural

An edge exists because a deployment **names** an image and an image manifest
**names** packages. It is present when everything is healthy. Status colours an
edge and `blocked_on` annotates one; **neither creates one.**

A first design drew edges only where something was blocked. That gives a working
host a picture of disconnected boxes — the opposite of what a graph is for, and
it would have shipped if it had not been questioned.

`follow-rolling` is a distinguished relation rather than a flag, because it means
something permanent: this deployment chases its image's `current_version`
forever. It is emitted **only where it is live** — the flag is stored but inert on
a `restart:"no"` def, and drawing an arrow for a rule that never fires is a lie in
the most load-bearing place on the page.

## Alternatives considered

**A stage list per kind, unrelated to the package one.** Simplest to write.
Rejected: it is the four-vocabularies problem, recreated deliberately, one ADR
after fixing it.

**One flat list that is the union, with every kind walking all of it.** No
subsets, no per-kind table. Rejected: a package would report `assemble` as
`not-implemented` forever, and a stage that can never apply is noise that trains
operators to ignore the column.

**Store the graph.** Faster to render. Rejected for the reason ADR-0256 gave and
ADR-0155 proved: derived state with no invalidation event goes stale silently,
and a stale copy of "what is broken" is worse than none.

## Consequences

- The stage enum grows from 11 to 15. `test_pipeline` asserted the count and
  **failed when this landed**, which is ADR-0256's "a deliberate act with an ADR,
  not a quiet enum append" working exactly as designed. Updating it is the
  deliberate act; this document is the record.
- `GET /v1/pipeline` gains `images`, `deployments`, `edges` and `kinds`.
  `packages`, `stages` and `deploy` are **byte-identical** — a generalisation that
  broke its predecessor's contract would be a regression wearing a feature's
  clothes.
- Each kind's stage list is published in `kinds`, so a renderer draws lanes from
  the daemon's answer rather than a copy of the table that will drift.
- Nothing here forks anything yet. `acquire` exists and reports `blocked`;
  making a deployment actually start an image build is separate, riskier, and
  gets its own ADR.
