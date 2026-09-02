# Guides

Task-oriented "how do I do X" instructions for building, installing, operating, and extending Cix. Each guide has exactly one job and links to the ADR/API doc that owns the "why"/"contract" behind it rather than restating it — see [`docs/README.md`](../README.md) for how this category relates to the rest of the documentation set.

**Getting started** — first contact with a new box:

| Guide | Job |
|---|---|
| [`quickstart.md`](quickstart.md) | The fastest real path to a first running container |
| [`installing.md`](installing.md) | Building and using the installer ISO: Secure Boot, partitioning, first boot |
| [`building-cix.md`](building-cix.md) | Compiling `cixd`/`cixctl`/`web` — on a dev machine, or self-hosted from a running Cix box |

**Operating** — running and administering an installed box day to day:

| Guide | Job |
|---|---|
| [`cli-reference.md`](cli-reference.md) | The full `cixctl` command surface |
| [`web-dashboard.md`](web-dashboard.md) | A tour of the browser dashboard |
| [`staying-updated.md`](staying-updated.md) | Keeping an already-installed system current: control plane + packages |
| [`administration.md`](administration.md) | Monitoring, backup/restore, and disk management day to day |
| [`networking.md`](networking.md) | Networks, physical/VLAN interface attachment, and routing |
| [`security.md`](security.md) | PKI, HTTPS, and LDAP-backed Unix/SSH accounts |

**Extending** — building on top of Cix:

| Guide | Job |
|---|---|
| [`storage.md`](storage.md) | Disks, the six roles and what each is for, btrfs vs ext4, snapshots and quotas |
| [`writing-recipes.md`](writing-recipes.md) | The complete `pkg` recipe format, with a real worked example |
| [`remote-development.md`](remote-development.md) | Pushing local (or server-compiled) changes onto a real box with no SSH, and proving they landed |
| [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) | Producing a kernel and rolling it out through the A/B slot mechanism |

Unlike `docs/adr/`, these are living documents with no numeric prefix — edited in place as the mechanism each one documents changes, not an append-only historical sequence. See `docs/README.md`'s own "Taxonomy and naming" section for the full rule.
