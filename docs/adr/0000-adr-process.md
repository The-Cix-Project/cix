# 0000 — Recording architecture decisions

## Status

Accepted

## Context

This project accumulates significant, hard-to-reverse engineering decisions as it grows (toolchain constraints, syscall-level mechanics, storage layout, API contract shape). `docs/roadmap/ROADMAP.md` records *what* was built and how it was verified, phase by phase, but it isn't the right place for the *why* — the reasoning, alternatives considered, and trade-offs behind a specific decision. Without a dedicated place for that, the reasoning either gets lost or gets re-litigated by a future session that doesn't know it was already settled — a violation of One Source of Truth.

## Decision

We keep Architecture Decision Records (ADRs) in `docs/adr/`, one file per significant decision, numbered sequentially (`0001-`, `0002-`, ...). Each ADR:

- Is short: Status, Context, Decision, Consequences. No implementation detail that belongs in `docs/roadmap/ROADMAP.md` instead — link to the relevant phase there rather than repeating it.
- Is **append-only**. An ADR is never edited to reverse its own decision. If a later decision supersedes an earlier one, the earlier ADR's Status changes to `Superseded by ADR-00NN` and a new ADR is written — the history of *why we changed our mind* is itself worth keeping, consistent with No Regressions (we can always see what guarantee we're moving away from and why).
- Only exists for decisions with real, durable consequences — a choice that would be expensive to reverse, that constrains later work, or that isn't obvious from reading the code. Routine implementation choices don't get one; that would be its own kind of clutter, against Zen.

### File and header schema

Every ADR is `docs/adr/NNNN-kebab-case-title.md`, four digits, zero-padded, one number per file and no gaps. The first line is the H1, in exactly this form, and the Status heading follows it:

```markdown
# NNNN — Title in sentence case

## Status

Accepted

Issue [#123](https://git.home.arpa/itdlabs/cix/issues/123). Supersedes [ADR-0099](0099-something.md).

## Context
```

The prose line under the status is optional and is where an issue reference, a supersession, a relationship to another ADR, or a decision date belongs. Both the H1 form and the Status heading are mechanically checked (`test/test_docindex.c`), because this corpus has drifted twice: seventeen files once used `# ADR-NNNN: Title`, and seven of those additionally carried Status, Date and Issue as a bullet list instead of a heading. Both forms were readable and neither was wrong — they were simply a second way to say the same thing, which is what One Source of Truth forbids of a document set as much as of a registry.

### Status values

The **first word** is the status and comes from this set: `Proposed`, `Accepted`, `Superseded by ADR-00NN`, `Deprecated`.

It may be followed by qualifying prose, on the same line or the next, and often should be. `Accepted; phased.` ([ADR-0179](0179-user-namespaces-by-default-subordinate-id-allocation.md), [ADR-0207](0207-btrfs-storage-substrate-userns-by-default.md)) and `Accepted, with two decisions in it superseded` ([ADR-0184](0184-dashboard-navigation-one-vocabulary.md)) both say something a bare token cannot, and a rule that forbade them would be a rule asking documents to be less accurate. What is fixed is the first word; what follows is prose.

## Consequences

- Anyone (human or a future session of this assistant) asking "why is it built this way" has one place to look before proposing a change, rather than rediscovering the reasoning from scratch or accidentally re-introducing something already tried and rejected.
- Discipline required: when a genuinely significant decision is made, write the ADR *then*, not "later" — a promised-but-missing ADR is exactly the kind of stop-gap this project's rules forbid.
