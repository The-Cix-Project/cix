# Cix documentation

This directory holds every durable, non-code artifact describing Cix: why it exists, what's shipped, why each hard-to-reverse decision was made, the API contract, and a picture of the system's current shape. Nothing lives loose in `docs/` itself — every document sits under the one subdirectory that owns its kind of content, and this page only points at each; it doesn't repeat what's inside them (One Source of Truth).

| Directory | Job | Start here |
|---|---|---|
| [`mission/`](mission/) | *Why* Cix exists at all — the original charter, verbatim, frozen | [`mission/MISSION.md`](mission/MISSION.md) |
| [`roadmap/`](roadmap/) | *What* has shipped, phase by phase, and how each phase was verified | [`roadmap/ROADMAP.md`](roadmap/ROADMAP.md) |
| [`adr/`](adr/) | *Why* a specific, significant, hard-to-reverse engineering decision was made the way it was — reasoning and alternatives, not implementation detail | [`adr/README.md`](adr/README.md) |
| [`api/`](api/) | The REST API contract, both as data (OpenAPI) and as a human-readable walkthrough | [`api/openapi.yaml`](api/openapi.yaml) (authoritative), [`api/README.md`](api/README.md) (narrative) |
| [`architecture/`](architecture/) | A visual map of the system's components and how they connect, as of the most recently reflected phase | [`architecture/architecture.svg`](architecture/architecture.svg) |
| [`retrospective/`](retrospective/) | *Why an entire episode was harder than the work inside it* — a root-cause analysis spanning several bugs at once, written when the pattern matters more than any individual fix | [`retrospective/0001-why-bringing-up-dns-was-painful.md`](retrospective/0001-why-bringing-up-dns-was-painful.md) |
| [`guides/`](guides/) | Task-oriented operator/user instructions — how to build, install, update, administer, network, secure, or write a recipe for Cix | [`guides/README.md`](guides/README.md) |
| [`brand/`](brand/) | The Cix brand system — the guidelines transcription, the owner's logo reference sheet, and brand assets. Content authority stays with the owner: files here are faithful copies, replaced only by new versions from them, never edited ad hoc | [`brand/README.md`](brand/README.md) |

Two more project documents live outside `docs/` entirely, at the repository root, because they're read before anything under `docs/` is: [`CLAUDE.md`](../CLAUDE.md) (living instructions for working in this repository — rules, conventions, environment facts) and [`CHANGELOG.md`](../CHANGELOG.md) (the chronological record of every change, grouped by roadmap phase). The root [`README.md`](../README.md) is the project's own front door — what Cix is, a quickstart pointer — and links back into every directory listed above rather than repeating their content (in particular, it does not carry its own phase-status table — that's `roadmap/ROADMAP.md`'s job alone). A third file, [`../recipes/README.md`](../recipes/README.md), sits next to the recipe catalog itself rather than under `docs/`, since a recipe author is already looking at that directory — it does no more than point at [`guides/writing-recipes.md`](guides/writing-recipes.md).

## Which document answers which question

- **"Why does this project exist, and what's it ultimately trying to become?"** → [`mission/MISSION.md`](mission/MISSION.md).
- **"Has feature X shipped yet? How was it verified?"** → [`roadmap/ROADMAP.md`](roadmap/ROADMAP.md).
- **"Why is it built *this* way and not some other way?"** → [`adr/`](adr/) — check the index for the specific decision first; if none exists, the choice was either not yet significant enough to record or genuinely undecided.
- **"What does the API actually accept and return?"** → [`api/openapi.yaml`](api/openapi.yaml) is the ground truth; [`api/README.md`](api/README.md) is the same information organized for reading start to finish.
- **"What talks to what, at a glance?"** → [`architecture/architecture.svg`](architecture/architecture.svg).
- **"How do I actually build/install/update/administer Cix, or write a recipe?"** → [`guides/`](guides/) — check the index for the specific task first.
- **"What are the official colours, the logo, the typography?"** → [`brand/`](brand/) — the owner's own brand documents, transcribed rather than reinterpreted.
- **"What changed recently, and why?"** → [`../CHANGELOG.md`](../CHANGELOG.md), newest entries first.
- **"How do I work in this repository — rules, conventions, known environment quirks?"** → [`../CLAUDE.md`](../CLAUDE.md).

## Taxonomy and naming

Every document lives under the subdirectory matching its *kind* (mission, roadmap, decision record, API contract, diagram, how-to guide), not its topic or the phase that introduced it — a networking decision and a PKI decision both live in `adr/`, not in directories of their own; a guide for installing Cix and a guide for writing a recipe both live in `guides/`, not split by subsystem.

Two distinct naming conventions apply, deliberately different, because the two kinds of document have opposite lifecycles:

- **`adr/` is an append-only historical sequence** — files are named `NNNN-kebab-case-title.md`, numbered sequentially with no gaps and no reused numbers, oldest first. A superseded ADR keeps its original number and file, its `Status` line updated to point at the ADR that replaced it (append-only, never edited to reverse its own decision — see [`adr/0000-adr-process.md`](adr/0000-adr-process.md)); a plain factual correction, as opposed to a reversed decision, may still be fixed in place.
- **`guides/` is a set of living documents** — files are named `kebab-case-topic.md`, **no numeric prefix**. A guide describes how to do something *today*; it gets edited in place as the underlying mechanism changes, the same way `roadmap/ROADMAP.md` and `api/README.md` are living documents. Numbering it like an ADR would incorrectly imply a fixed historical order that doesn't exist.

This same underlying rule — one directory per document kind, index files that point rather than repeat — is the pattern to extend if a genuinely new *kind* of document is ever needed; it is not a reason to add a new subdirectory for a new *topic*.
