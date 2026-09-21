# Cix documentation

This directory holds every durable, non-code artifact describing Cix: why it exists, what's shipped, why each hard-to-reverse decision was made, the API contract, and a picture of the system's current shape. Nothing lives loose in `docs/` itself — every document sits under the one subdirectory that owns its kind of content, and this page only points at each; it doesn't repeat what's inside them (One Source of Truth).

| Directory | Job | Start here |
|---|---|---|
| [`mission/`](mission/) | *Why* Cix exists at all — the original charter, verbatim, frozen | [`mission/MISSION.md`](mission/MISSION.md) |
| [`roadmap/`](roadmap/) | *What* has shipped, phase by phase, and how each phase was verified | [`roadmap/ROADMAP.md`](roadmap/ROADMAP.md) |
| [`adr/`](adr/) | *Why* a specific, significant, hard-to-reverse engineering decision was made the way it was — reasoning and alternatives, not implementation detail | [`adr/README.md`](adr/README.md) |
| [`api/`](api/) | The REST API contract, both as data (OpenAPI) and as a human-readable walkthrough | [`api/openapi.yaml`](api/openapi.yaml) (authoritative), [`api/README.md`](api/README.md) (narrative) |
| [`architecture/`](architecture/) | A visual map of the system's components and how they connect — a picture of what exists now, updated whenever a change adds, removes or rewires a box or arrow it shows | [`architecture/architecture.svg`](architecture/architecture.svg) |
| [`retrospective/`](retrospective/) | *Why an entire episode was harder than the work inside it* — a root-cause analysis spanning several bugs at once, written when the pattern matters more than any individual fix | [`retrospective/README.md`](retrospective/README.md) |
| [`guides/`](guides/) | Task-oriented operator/user instructions — how to build, install, update, administer, network, secure, or write a recipe for Cix | [`guides/README.md`](guides/README.md) |
| [`keys/`](keys/) | The public halves of the keys Cix signs with, published so an outsider can verify what this project ships — never any private key | [`keys/README.md`](keys/README.md) |
| [`brand/`](brand/) | The Cix brand system — the guidelines transcription, the owner's logo reference sheet, and brand assets. Content authority stays with the owner: files here are faithful copies, replaced only by new versions from them, never edited ad hoc | [`brand/README.md`](brand/README.md) |

Two more project documents live outside `docs/` entirely, at the repository root, because they're read before anything under `docs/` is: [`CLAUDE.md`](../CLAUDE.md) (living instructions for working in this repository — rules, conventions, environment facts) and [`CHANGELOG.md`](../CHANGELOG.md) (the chronological record of every change, newest first). The root [`README.md`](../README.md) is the project's own front door — what Cix is, a quickstart pointer — and links back into every directory listed above rather than repeating their content (in particular, it does not carry its own phase-status table — that's `roadmap/ROADMAP.md`'s job alone). The recipe catalog itself is no longer in this repository at all — it moved to [cix-recipes](https://git.home.arpa/itdlabs/cix-recipes) on 2026-09-21 ([ADR-0308](adr/0308-recipes-are-their-own-repository-flat.md)), carrying its own README; [`guides/writing-recipes.md`](guides/writing-recipes.md) remains the place that explains how to write one.

## Which document answers which question

- **"Why does this project exist, and what's it ultimately trying to become?"** → [`mission/MISSION.md`](mission/MISSION.md).
- **"Has feature X shipped yet? How was it verified?"** → [`roadmap/ROADMAP.md`](roadmap/ROADMAP.md).
- **"Why is it built *this* way and not some other way?"** → [`adr/`](adr/) — check the index for the specific decision first; if none exists, the choice was either not yet significant enough to record or genuinely undecided.
- **"What does the API actually accept and return?"** → [`api/openapi.yaml`](api/openapi.yaml) is the ground truth; [`api/README.md`](api/README.md) is the same information organized for reading start to finish.
- **"What talks to what, at a glance?"** → [`architecture/architecture.svg`](architecture/architecture.svg).
- **"How do I actually build/install/update/administer Cix, or write a recipe?"** → [`guides/`](guides/) — check the index for the specific task first.
- **"What are the official colours, the logo, the typography?"** → [`brand/`](brand/) — the owner's own brand documents, transcribed rather than reinterpreted.
- **"Why was that whole episode so painful, and has anything changed since?"** → [`retrospective/`](retrospective/).
- **"What changed recently, and why?"** → [`../CHANGELOG.md`](../CHANGELOG.md), newest entries first.
- **"How do I work in this repository — rules, conventions, known environment quirks?"** → [`../CLAUDE.md`](../CLAUDE.md).

## Taxonomy and naming

### Where a document goes

Every document lives under the subdirectory matching its **kind** — mission, roadmap, decision record, retrospective, API contract, diagram, how-to guide, key, brand asset — never its *topic* and never the phase that introduced it. A networking decision and a PKI decision both live in `adr/`; a guide for installing Cix and a guide for writing a recipe both live in `guides/`. Splitting by subsystem would mean asking "is this a networking thing or a storage thing?" before every write, and getting it wrong half the time.

`docs/` itself holds **only** `README.md` — this file. A document loose in the root is one that has not been assigned a kind, and "not in the map" is not a valid state for a document to be in.

### Two lifecycles

Every directory here is one of exactly two things, and the naming follows from which:

**Append-only sequences** — a numbered historical record. A document is never rewritten to say something different; it is superseded by a later one, and the older stays exactly as written because the history of changing our mind is itself the value.

**Living documents** — a description of how things are *today*, edited in place as reality changes. A number would falsely imply a fixed order.

| Directory | Lifecycle | Naming | Index |
|---|---|---|---|
| [`adr/`](adr/) | append-only sequence | `NNNN-kebab-case-title.md`, sequential, no gaps, no reuse | [`adr/README.md`](adr/README.md) |
| [`retrospective/`](retrospective/) | append-only sequence | `NNNN-kebab-case-title.md`, same convention and same reason | [`retrospective/README.md`](retrospective/README.md) |
| [`guides/`](guides/) | living | `kebab-case-topic.md`, **no numeric prefix** | [`guides/README.md`](guides/README.md) |
| [`api/`](api/) | living | `openapi.yaml` (authoritative) + `README.md` (narrative) | its own `README.md` is both index and narrative |
| [`keys/`](keys/) | append-only in practice | the key's own published filename | [`keys/README.md`](keys/README.md) |
| [`brand/`](brand/) | replaced, never edited | the owner's own filenames, kept verbatim | [`brand/README.md`](brand/README.md) |
| [`mission/`](mission/) | frozen | `MISSION.md` | — single document |
| [`roadmap/`](roadmap/) | living | `ROADMAP.md` | — single document |
| [`architecture/`](architecture/) | living | `architecture.svg` | — single document |

**A single-document directory has no index**, and deliberately so: a `README.md` beside one file could only repeat that file's own opening or restate the row above, which is duplication wearing an index's clothes. The directory *is* the document. Its file takes the directory's own name in the SHOUTING form the ecosystem already uses for a canonical top-level document (`README`, `CHANGELOG`, `LICENSE`) — hence `MISSION.md`, `ROADMAP.md` — except `architecture/`, whose payload is an image and takes the ordinary lowercase asset name.

**A directory with more than one document has a `README.md` that points and never repeats.** `test/test_docindex.c` enforces the mechanical half of this: every subdirectory has a row in this file and in `CLAUDE.md`'s Documentation Map, every ADR and every guide has a row in its own index, and every one of those rows resolves to a file that exists. What a row *says* is review; that it exists and resolves is a build failure.

### Document headers

`adr/` additionally fixes its H1 and status block, because that corpus drifted into two formats once and a reader should never have to work out which they are looking at. The schema, and what is checked, is in [`adr/0000-adr-process.md`](adr/0000-adr-process.md).

### Adding a new kind

One directory per document *kind*, an index that points rather than repeats, and one of the two lifecycles above. That is the pattern to extend if a genuinely new kind is ever needed — a new *topic* is not a new kind, and does not get a directory.
