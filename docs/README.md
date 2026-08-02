# Kanxeo documentation

This directory holds every durable, non-code artifact describing Kanxeo: why it exists, what's shipped, why each hard-to-reverse decision was made, the API contract, and a picture of the system's current shape. Nothing lives loose in `docs/` itself — every document sits under the one subdirectory that owns its kind of content, and this page only points at each; it doesn't repeat what's inside them (One Source of Truth).

| Directory | Job | Start here |
|---|---|---|
| [`mission/`](mission/) | *Why* Kanxeo exists at all — the original charter, verbatim, frozen | [`mission/MISSION.md`](mission/MISSION.md) |
| [`roadmap/`](roadmap/) | *What* has shipped, phase by phase, and how each phase was verified | [`roadmap/ROADMAP.md`](roadmap/ROADMAP.md) |
| [`adr/`](adr/) | *Why* a specific, significant, hard-to-reverse engineering decision was made the way it was — reasoning and alternatives, not implementation detail | [`adr/README.md`](adr/README.md) |
| [`api/`](api/) | The REST API contract, both as data (OpenAPI) and as a human-readable walkthrough | [`api/openapi.yaml`](api/openapi.yaml) (authoritative), [`api/README.md`](api/README.md) (narrative) |
| [`architecture/`](architecture/) | A visual map of the system's components and how they connect, as of the most recently reflected phase | [`architecture/architecture.svg`](architecture/architecture.svg) |

Two more project documents live outside `docs/` entirely, at the repository root, because they're read before anything under `docs/` is: [`CLAUDE.md`](../CLAUDE.md) (living instructions for working in this repository — rules, conventions, environment facts) and [`CHANGELOG.md`](../CHANGELOG.md) (the chronological record of every change, grouped by roadmap phase). The root [`README.md`](../README.md) is the project's own front door — what Kanxeo is, how to build and run it — and links back into every directory listed above.

## Which document answers which question

- **"Why does this project exist, and what's it ultimately trying to become?"** → [`mission/MISSION.md`](mission/MISSION.md).
- **"Has feature X shipped yet? How was it verified?"** → [`roadmap/ROADMAP.md`](roadmap/ROADMAP.md).
- **"Why is it built *this* way and not some other way?"** → [`adr/`](adr/) — check the index for the specific decision first; if none exists, the choice was either not yet significant enough to record or genuinely undecided.
- **"What does the API actually accept and return?"** → [`api/openapi.yaml`](api/openapi.yaml) is the ground truth; [`api/README.md`](api/README.md) is the same information organized for reading start to finish.
- **"What talks to what, at a glance?"** → [`architecture/architecture.svg`](architecture/architecture.svg).
- **"What changed recently, and why?"** → [`../CHANGELOG.md`](../CHANGELOG.md), newest entries first.
- **"How do I work in this repository — rules, conventions, known environment quirks?"** → [`../CLAUDE.md`](../CLAUDE.md).

## Taxonomy and naming

Every document lives under the subdirectory matching its *kind* (mission, roadmap, decision record, API contract, diagram), not its topic or the phase that introduced it — a networking decision and a PKI decision both live in `adr/`, not in directories of their own. ADRs are named `NNNN-kebab-case-title.md`, numbered sequentially with no gaps and no reused numbers, oldest first; a superseded ADR keeps its original number and file, its `Status` line updated to point at the ADR that replaced it (append-only, never edited to reverse its own decision — see [`adr/0000-adr-process.md`](adr/0000-adr-process.md)). This same convention — one directory per document kind, index files that point rather than repeat — is the pattern to extend if a genuinely new *kind* of document is ever needed; it is not a reason to add a new subdirectory for a new *topic*.
