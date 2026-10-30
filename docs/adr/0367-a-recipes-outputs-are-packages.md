# 0367 — A recipe's outputs are packages

## Status

Accepted by the owner on 2026-10-30, for #378: the go-ahead and the price, and decision 4's approval shape. It builds on cix-build-system ADR-0038 (cbs v0.1.119), which leaves name resolution and approvals to the repository (its section 3).

## Context

cbs v0.1.119 lets a recipe declare repeatable `output "NAME" { files { ... } requires { ... } }` blocks. One source, pinned once, is built once and emits several `.cixpkg` artifacts:

- the main package, named by the `package` declaration, takes every staged path no output claimed;
- each output is its own package, sharing the recipe's version and release;
- all of them record the same `build_fingerprint`;
- `cbs explain --json` reports an `outputs` array, main package first;
- `--output FILE` is refused for a multi-output recipe, and `--output-dir DIR` writes each artifact under its canonical name, `name-version-release-arch.cixpkg`.

cixd assumes one recipe yields one package of the same name. Measured in `daemon/src/pkg.c` on 2026-10-30:

- the build command passes `--output /build/artifact.cixpkg` (`pkg_cbs_build_command()`);
- the harvest reads that one path;
- name resolution, dependency resolution, the cache name, the push and the approval (`metadata "artifact_sha256"`, read through `cbs_explain_metadata()` and written back by the ADR-0324 write-back) all key on the recipe's name.

cbs v0.1.107 has no `--output-dir`. It arrived in v0.1.119.

## Decision

1. **An output is a package.** It has its own name, the recipe's version and release, its own files, its own runtime requirements and its own artifact. Installing, requiring, pinning in an image manifest, caching and pushing all treat it as an ordinary package.
2. **A package name is offered by exactly one recipe**, as its main package or as one of its outputs. ADR-0324's "one package, one source" extends to outputs. A publish whose outputs name a package another recipe offers is refused, naming both. cixd resolves a name to `(recipe, output)` through one index, built from `explain` at publish and sync, and swept once for the existing store. Nothing else maps a name to a recipe.
3. **Every build writes with `--output-dir`, for every recipe.** One build action has one artifact directory: the requested package is installed from it, and every other output's artifact goes into the local cache beside it and is pushed like any other. A single-output recipe yields one artifact, whose name is the one cixd uses today. There is one build path, never a single-output case and a multi-output case.
4. **One approval line per artifact** (the owner's choice). `metadata "artifact_sha256"` stays the main package's. Each output's is `metadata "artifact_sha256.<output>"`. Each line names one byte sequence, as ADR-0307 clause 5 requires. The write-back writes every artifact the build produced, and each is checked against its own line when a cache hit installs it. A single-output recipe is unchanged.
5. **A hostbuild keeps every output.** The control-plane root takes `/bin/cixctl` from the cix hostbuild today. Once `cix` declares `output "cixctl"`, assembly takes it from the `cixctl` artifact of the same hostbuild. It never falls back to a second build.
6. **The order is forced, and each step ships alone.**
    1. cbs v0.1.120 reaches cix-builder, cix-hosttools, the host root and the ADR-0209 test floor.
    2. cixd builds with `--output-dir`. Every recipe is still single-output, so nothing visible changes.
    3. The index, resolution, per-output cache and approvals.
    4. Assembly takes cixctl from the `cixctl` output.
    5. The `cix` recipe declares `output "cixctl"`, built by a daemon from step 4. Never in the same release as the daemon that first understands outputs, which could not harvest what it built.
    6. The agent image and container (ADR-0366).

## Alternatives considered

- **Keep `--output FILE` for single-output recipes, `--output-dir` for the rest.** Two build paths, the shape One Source of Truth forbids. ADR-0038 also calls `--output` the legacy form.
- **One approval naming the fingerprint, plus N digests** (ADR-0038 section 3's other shape). It states outright that the artifacts share one build, at the cost of an approval shape every reader would have to learn. The owner chose one line per artifact.
- **Name outputs `recipe/output`.** It would keep the index trivial, and give every consumer a second kind of package name. An output is a package and is named like one.

## Consequences

- An ordinary recipe sees no change: one artifact, one approval line, the same names.
- The test floor's cbs moves with step 1, through the six-way coupling CLAUDE.md records, because floor tests build through cixd.
- `cixctl pkg ls` and the dashboard list an output like any package. Which recipe built it is the index's answer, shown where a package's source is shown.
