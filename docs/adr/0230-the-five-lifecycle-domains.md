# 0230 — The platform has five lifecycle domains, named in the CLI and the docs, not in URLs

## Status

Accepted

Issue [#182](https://git.home.arpa/itdlabs/cix/issues/182). Decided 2026-09-02.

## Context

Cix has a complete delivery pipeline: it builds its own packages from
recipes, publishes them to a shared cache, composes images, assembles
its own control plane, compiles its own kernel, upgrades itself across
A/B slots, and cuts its own installer media on a real host.

What it does not have is a **shape**. All of it is reachable through two
flat namespaces, `/v1/pkg/*` and `/v1/system/*`, which group by
implementation rather than by what an operator is doing. `/system/iso`
(make media), `/system/update` (stage a control plane) and
`/system/reboot` are siblings; `/pkg/hostbuild` (build software) and
`/pkg/cache` (distribute software) are siblings. Each pair belongs to
different concerns.

## Decision

**Five domains are named, and they are expressed in the CLI, the
dashboard and the documentation — not as URL prefixes.**

| domain | answers | covers |
|---|---|---|
| **Catalogue** | what software exists | recipes, packages, images, versions, available vs installed |
| **CI** | how software gets built | builds, build images, build environments, build logs, produced artifacts |
| **CD** | how software gets delivered | the artifact cache, publish and sync, repo config, update policy |
| **Host lifecycle** | how this machine changes | control-plane assembly, A/B staging, kernel pinning, deploy, reboot, rollback |
| **Media** | how new machines are made | installer ISO assembly, signing keys, what goes on the media |

### Why not URL prefixes

The issue asks this explicitly and calls a prefix "clearer". It is
clearer to a reader of the URL list, and that reader is the person this
change helps least.

An operator reaches this platform through `cixctl` and the dashboard.
Those are where the grouping is felt, and both can express it fully
without a single path changing. Against that, prefixes are a breaking
change to **275 operations**, every generated helper in `web/api.js`,
every generated route in `apiroute.h`, the entire CLI command tree, and
every code sample in the guides — for a gain confined to the least
operator-visible surface there is.

There is also a quieter argument. `/v1/pkg/...` and `/v1/system/...` are
resource namespaces, and REST paths name resources. A lifecycle domain
is a *view* over resources — assembly touches packages, artifacts, the
ESP and the slots — so several endpoints would have to belong to two
domains at once, and the prefix would then be a lie in the URL rather
than an ambiguity in the docs.

**What would change this answer:** a second consumer that is not the CLI
or the dashboard, written by someone who reads the contract before the
documentation. Then the URL is the vocabulary, and the trade flips.

### Where status lives

Each domain gets **one honest "what is happening now" surface**, and
this is the part the issue correctly calls the weakest today.

The rule: **status belongs to the thing that is happening, never to a
neighbour that happens to know about it.** Control-plane assembly is the
worked example and the first fix — it had no endpoint at all, and its
progress was two generation counters on `GET /system/boot`, an endpoint
whose subject is *what booted*, reporting on *what is being built*. An
operator watching a deploy had to know that assembly is a side effect of
a hostbuild named `cix` and is observed through a counter somewhere else.

## Consequences

- No path changes and nothing breaks. The taxonomy arrives as
  vocabulary: CLI grouping, dashboard navigation, documentation
  structure.
- Each domain gains a status surface as it is worked, rather than in one
  sweep. `GET /v1/system/assembly` is the first (issue #182's own
  sharpest symptom); the fields move off `/system/boot`, which is a
  clean cut-over with its one consumer updated in the same change, per
  this project's no-compatibility-shim posture.
- The taxonomy is deliberately recorded before the work rather than
  after, because the shape outlives every implementation detail beneath
  it — which is what #182 asked for.
- The other symptoms #182 lists stay open and are now placeable:
  "which image can build what is convention, not contract" is a **CI**
  question; the ISO's three input sources are a **Media** question;
  kernel compilation being "a package that happens to be called kernel"
  is a **CI** question with a **host lifecycle** consumer.
