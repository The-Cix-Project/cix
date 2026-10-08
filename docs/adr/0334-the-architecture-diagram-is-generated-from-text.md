# 0334 — The architecture diagram is text, and a gate keeps it current

## Status

Accepted, 2026-10-08, the owner's decision on [cix#582](https://git.home.arpa/itdlabs/cix/issues/582) — *"Mermaid it is!"*. Changes the form of the document [ADR-0005](0005-api-first-mandate.md)'s own picture lived in; supersedes nothing, because the previous diagram recorded no decision.

## Context

`docs/architecture/architecture.svg` was a hand-laid-out SVG: 102 rects, 202 text elements and 46 lines at coordinates someone computed by hand. The Documentation Map's rule for it was *"Updated whenever a change adds/removes/rewires a box or arrow it shows; stale diagrams are worse than none, so this is not optional busywork."*

**It went 34 ADRs stale, and said so.** Its own `<title>` read *"as of Part 92"* while ROADMAP was past 270. Measured on 2026-10-08 against its own text content: zero occurrences of `schedul`, `catalogue`, `discover`, `upstream`, `repositor` or `forge`, and its seven `roll` hits were all "hand-rolled" or ADR-0107's *image* rolling. Absent entirely were recipe sources as external components (ADR-0308, ADR-0315, ADR-0324), package repositories as many mirrors (ADR-0324), the scheduler (ADR-0316, ADR-0327), package rolling (ADR-0323), the nightly host roll (ADR-0327), the signed catalogue and upstream key trust (ADR-0326), and the RBAC enforcement point (ADR-0317) — most of a month's architecture.

**Two causes, and only one of them was about effort.**

First, editing it was expensive. Adding a box meant choosing coordinates that did not collide with 101 others, in a file where a diff shows numbers rather than meaning.

Second, and the part that made it unfixable rather than merely neglected: **this project's own rules forbade the only way to check such an edit.** CLAUDE.md said a change to that file *"should be verified"* by rendering it with `rsvg-convert` and explicitly rejected *"eyeballing the raw SVG source"* — while THE SANDBOX RULE says nothing but `cixctl` executes in the development sandbox, which the owner restated as "nothing, never". So the sanctioned verification was prohibited and the alternative was pre-rejected. An assistant session could not edit that diagram to the standard the file demanded, and the file contained a procedure it also forbade.

## Decision

**The architecture diagram is mermaid in `docs/architecture/architecture.md`, and a gate asserts it names what exists.**

1. **Mermaid, in a fenced block inside a Markdown document.** Not a bare `.mmd`: Gitea renders mermaid in its Markdown viewer, so the diagram is *visible* with no renderer anywhere in the loop — which was the whole requirement the SVG could not meet. A `.mmd` would render nowhere without a tool and would reproduce the problem in a new syntax.

2. **Layout is the renderer's job.** There are no coordinates, so there is nothing to verify by rendering, and the contradiction above disappears by removing its cause rather than by granting an exception.

3. **The SVG is deleted, not kept as a historical picture.** Keeping it would be a second source of truth with a nicer coat on: the Documentation Map row says this document is *"a picture of what exists now, not a decision record"*, and a frozen SVG is exactly the decision-record shape that row excludes. Git keeps every version of it.

4. **`test_docindex` asserts that every component with a daemon source file is named in the document.** `scheduler.c` requires "Scheduler", `pkgsource.c` requires "Recipe sources", and so on, as an explicit table in the test. This is a string match over text — impossible against coordinates, trivial against prose — and it is the part that matters: it would have caught this drift at ADR-0316 rather than ADR-0333, seventeen ADRs earlier.

5. **The picture carries the shape; the prose carries the detail.** The SVG crammed annotations into the drawing, which is why it held 202 text elements. The diagram now shows boxes, boundaries and arrows, and each layer gets a section below it. That plays to the medium instead of against it.

6. **`rsvg-convert` is not sanctioned, and the Environment note describing it is gone.** The exception would have restored one session's ability to maintain the SVG, and the SVG being hard to maintain is the cause of this ADR. Making the wrong thing possible is not a fix.

## Consequences

- **The hand-drawn visual design is lost** — the deliberate grouping, the line icons, the colour system. That is the real cost and it was weighed: the *information* those encoded is the boundaries they drew, and mermaid has subgraphs and class styling, so the boundaries survive and the polish does not. If the polish is wanted back, that argues for generating a styled SVG *from* this source, never for hand-maintaining one beside it.
- **The diagram can now be wrong in a way a machine notices.** Previously "is this current" was a judgement nobody made; it is now a test that fails.
- **It can still be incomplete in ways the gate misses.** The gate checks that a component is *named*, not that the arrows around it are right, and a subsystem with no daemon source file of its own is invisible to it. That is a real limit, and the alternative — asserting the shape is correct — is not mechanically checkable at all.
- **One more document renders differently in different places.** GitHub and Gitea render mermaid; a plain Markdown viewer shows the fenced source. The source is readable prose, so that degrades to something useful rather than to nothing, which was not true of an SVG shown as XML.

## Alternatives rejected

- **Sanction `rsvg-convert` for this one file.** The smallest change, and it fixes the wrong problem: one session's ability to verify one file, leaving untouched the reason the diagram went stale. It also reintroduces a standing exception to a rule the owner has restated as absolute, for the benefit of an artefact this ADR removes.
- **Keep the SVG, hand-edit it, and skip verification.** Rejected by the Documentation Map's own row: a stale diagram is worse than none, and an unverified edit to a 1400×1420 coordinate space is how one becomes stale while looking maintained.
- **Graphviz `.dot`.** Text, diffable, and a better layout engine — but nothing renders it in the forge, so seeing it needs a tool in a sandbox where no tool may run. Mermaid was chosen for where it renders, not for the quality of its layout.
- **A geometry gate on the SVG** — a test asserting rects nest or stay disjoint, so a hand-added box could be checked without rendering. Rejected for three reasons: the 16×16 icon definitions live in their own coordinate space and would be the first of several carve-outs; "disjoint or contained" does not catch the failure that matters, a box correctly placed with its text spilling out of it; and it would be building a verifier *for* hand-edited SVG, which is the practice this ADR ends.
