# 0000 — Recording architecture decisions

## Status

Accepted

## Context

This project accumulates significant, hard-to-reverse engineering decisions as it grows (toolchain constraints, syscall-level mechanics, storage layout, API contract shape). `docs/ROADMAP.md` records *what* was built and how it was verified, phase by phase, but it isn't the right place for the *why* — the reasoning, alternatives considered, and trade-offs behind a specific decision. Without a dedicated place for that, the reasoning either gets lost or gets re-litigated by a future session that doesn't know it was already settled — a violation of One Source of Truth.

## Decision

We keep Architecture Decision Records (ADRs) in `docs/adr/`, one file per significant decision, numbered sequentially (`0001-`, `0002-`, ...). Each ADR:

- Is short: Status, Context, Decision, Consequences. No implementation detail that belongs in `docs/ROADMAP.md` instead — link to the relevant phase there rather than repeating it.
- Is **append-only**. An ADR is never edited to reverse its own decision. If a later decision supersedes an earlier one, the earlier ADR's Status changes to `Superseded by ADR-00NN` and a new ADR is written — the history of *why we changed our mind* is itself worth keeping, consistent with No Regressions (we can always see what guarantee we're moving away from and why).
- Only exists for decisions with real, durable consequences — a choice that would be expensive to reverse, that constrains later work, or that isn't obvious from reading the code. Routine implementation choices don't get one; that would be its own kind of clutter, against Zen.

Status values: `Proposed`, `Accepted`, `Superseded by ADR-00NN`, `Deprecated`.

## Consequences

- Anyone (human or a future session of this assistant) asking "why is it built this way" has one place to look before proposing a change, rather than rediscovering the reasoning from scratch or accidentally re-introducing something already tried and rejected.
- Discipline required: when a genuinely significant decision is made, write the ADR *then*, not "later" — a promised-but-missing ADR is exactly the kind of stop-gap this project's rules forbid.
