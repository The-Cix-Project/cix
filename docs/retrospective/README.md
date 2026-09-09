# Retrospectives

One document per *episode* that was harder than the work inside it.

A retrospective is written when the **pattern** across several bugs matters more than any one of them — when the interesting finding is the shape of the difficulty rather than the defect. It is deliberately a different kind of document from the two it sits between:

| | answers | granularity |
|---|---|---|
| [`../adr/`](../adr/) | why one decision was made | one decision |
| [`../roadmap/ROADMAP.md`](../roadmap/ROADMAP.md) | what shipped and how it was verified | one phase |
| **`retrospective/`** | why a whole episode was harder than the work in it | one episode |

Each is a dated record of one episode and is **append-only**: never rewritten afterwards, because a retrospective that gets tidied up as understanding improves stops being evidence of what it was actually like at the time. Files are named `NNNN-kebab-case-title.md`, numbered sequentially, oldest first — the same convention `adr/` uses and for the same reason.

| # | Retrospective | The finding |
|---|---|---|
| [0001](0001-why-bringing-up-dns-was-painful.md) | Why bringing up DNS on a fresh install was painful | Small work — install a package, run two containers, register them — took hours, produced eight issues (#134–#141), and needed a developer machine and a hand-built HTTP server to finish. The gap between "the platform can do this" and "an operator can do this" was the finding, not any single bug. |
| [0002](0002-three-ownership-bugs-hiding-behind-each-other.md) | Three ownership bugs hiding behind each other | Four deploys, three distinct defects, each **invisible until the one in front of it was fixed**. The structure was the finding: a stack of bugs where only the topmost is observable makes progress look like regression. |
| [0003](0003-a-cut-over-invalidates-what-it-did-not-change.md) | A cut-over invalidates assumptions held by code it did not change | ADR-0260 landed correctly — right design, a gate that had already refused four revisions for real faults, a deploy verified live across eleven containers — and the next thing asked of the platform failed, and had been failing since the release shipped. A release that changes the build path is built by the daemon it replaces, so the first real exercise of new build code is the build *after* the one that ships it. |

## When to write one

Not for a hard bug. A hard bug gets a good commit message, a changelog entry, and an ADR if it changed a decision.

Write one when, looking back at a session, the honest summary is *"the work was small and getting to it was not"* — and when the reason generalises beyond the specific code. All three above share that shape, and each named something that has since changed how the platform is built or verified.
