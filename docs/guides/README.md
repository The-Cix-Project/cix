# Guides

Task-oriented "how do I do X" instructions for building, installing, operating, and extending Kanxeo. Each guide has exactly one job and links to the ADR/API doc that owns the "why"/"contract" behind it rather than restating it — see [`docs/README.md`](../README.md) for how this category relates to the rest of the documentation set.

| Guide | Job |
|---|---|
| [`quickstart.md`](quickstart.md) | The fastest real path to a first running container |
| [`building-kanxeo.md`](building-kanxeo.md) | Compiling `kanxeod`/`kanxeoctl`/`web` — on a dev machine, or self-hosted from a running Kanxeo box |
| [`installing.md`](installing.md) | Building and using the installer ISO: Secure Boot, partitioning, first boot |
| [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) | Producing a kernel and rolling it out through the A/B slot mechanism |
| [`staying-updated.md`](staying-updated.md) | Keeping an already-installed system current: control plane + packages |
| [`writing-recipes.md`](writing-recipes.md) | The complete `pkg` recipe format, with a real worked example |
| [`cli-reference.md`](cli-reference.md) | The full `kanxeoctl` command surface |
| [`web-dashboard.md`](web-dashboard.md) | A tour of the browser dashboard |

Unlike `docs/adr/`, these are living documents with no numeric prefix — edited in place as the mechanism each one documents changes, not an append-only historical sequence. See `docs/README.md`'s own "Taxonomy and naming" section for the full rule.
