# 0309 — shell recipes are history; the shell build path retires with its last dependent

## Status

Accepted. Builds on [ADR-0305](0305-a-recipes-format-is-its-filename.md) (two recipe languages), [ADR-0307](0307-a-packages-artifact-format-is-the-one-its-recipe-declares.md) (whose retirement query, "no image manifest resolving any package to a shell revision", this ADR applies to the build path) and [ADR-0308](0308-recipes-are-their-own-repository-flat.md) (the corpus is its own repository).

## Context

ADR-0305 let a recipe be a shell script or a CPDL document, and the conversion to CPDL (cix#487) has run since. Having two recipe languages is a parallel implementation, and "no parallel implementations" is one of this project's maxims. The owner asked whether the `.sh` recipes can therefore be removed. That question covers three different things, and the maxim gives a different answer for each.

Measured on 2026-09-23:

| | count |
|---|---|
| recipe files in cix-recipes | 1,502 `.sh`, 220 `.cbs` |
| packages whose **latest** revision is shell | 7: `gcc`, `gitea`, `glibc`, `go`, `kernel`, `node`, `tcc`, plus the throwaway `probe-*` recipes |
| installed package versions on 192.168.15.95 whose recipe is shell | 67 of 179, among them `glibc`, `gcc`, `kernel`, `bash` and `coreutils` |
| test files in this repository that write shell fixture recipes | 12 |
| image recipes | 59, all `.sh` by extension |

Three mechanisms decide what removal would do:

- **Sync only adds recipes.** `pkg sync` merges; `test_pkg_sync` asserts that a second sync skips a package recipe already present, and the only files the sync code unlinks are its own temporary tarballs. So deleting a recipe from the repository removes nothing from a host. It only makes the host hold recipes the repository no longer has.
- **An artifact's approval lives in its recipe**, and nowhere else: `pkg_artifact_sha256=` for shell, the `metadata` block for CPDL. It is the only thing that makes a cached artifact trustworthy (`recipe_adds_only_artifact_sha256()`, ADR-0122). A deleted recipe takes the approval for every artifact it built with it.
- **cixd has no CPDL executor of its own.** It runs `/usr/bin/cbs`, `explain` on the host and `build` inside the composed build environment. So building cbs from source needs a cbs. The lineage in the corpus shows the chain: `cbs@v0.1.23-1.sh` through `cbs@v0.1.25-1.sh` are shell, and every revision after is CPDL, built by the previous engine. The shell recipe is the only route that starts from nothing. That is the same shape as the compiler, for which the owner ruled that there is no ambient or external seed, ever (#36-#40).

## Decision

**1. Published shell recipes stay, in the repository and on hosts.** They are history, and history carries weight here:
- a published revision is immutable (ADR-0107);
- its approval is what makes its cached artifact trustworthy;
- 67 of the box's installed versions are built from one;
- `cix@v1.4.0.sh` through `cix@v2.57.244.sh` are the platform's own release record.

Deleting them would not remove the parallel. It would break reproducibility and artifact trust, and it would split the source of truth between the repository and every host.

**2. New revisions are CPDL.** A package's next revision is written in CPDL. The seven packages whose latest revision is still shell convert when they next change. For `tcc`, `glibc`, `gcc` and `kernel`, that conversion is a rebuild of the compiler, the C library or the kernel, so under this project's cost rule each needs the owner's go-ahead. `go` is blocked separately: its install is refused for an undeclared test fixture.

**3. The shell build path in cixd is the parallel, and it retires when its last dependent does.** The dependents are:
- the seven latest-shell packages above;
- the installed shell revisions: ADR-0307's query, no image manifest resolving any package to a shell revision (67 of 179 installed versions on 192.168.15.95 today);
- the 12 test files that write shell fixtures;
- the `probe-*` convention, which needs a CPDL template proven on a host first: a `.cbs` with a deliberately wrong hash must still make the daemon log `computed=`;
- the cbs bootstrap in point 5.

When those are gone, the shell path is removed in one change, not left dormant.

**4. Once probes have a CPDL template, publishing a new shell revision is refused.** That stops the parallel growing while it retires, and it costs nothing then.

**5. The cbs bootstrap is decided explicitly, not by default.** Removing the shell path leaves cbs installable from source only through a previously built cbs, whose root is `cbs@v0.1.25-1.sh`. The same question applies to the toolchain. The options, for the owner:
- **the seed is the previous Cix root.** This is already true today. `mkbootroot` refuses to assemble a control-plane root without `/usr/bin/cbs` (`image/src/mkbootroot.c`, ADR-0307 clause 6), so every Cix host has a Cix-built cbs, and building cbs from source is an ordinary upgrade using the engine the host already runs. The seed is Cix-built, not external, so the Build Provenance Mandate holds. **Recommended.**
- keep one shell recipe as the documented bootstrap root, the one exception to point 3;
- give cixd a minimal built-in CPDL executor sufficient to build cbs. That is a second CPDL implementation, a parallel of its own;
- accept seeding from an approved cached artifact that no Cix root carried, which the no-external-seed rule forbids.

**Considered and rejected: linking libcbs into cixd.** cbs builds a library, `libcbs.a`, so linking it looks like it removes the dependency on a separate binary. It does not, for two reasons:
- **It does not solve the bootstrap.** `libcbs.a` is built by the cbs recipe, which needs a cbs. A cixd linking it would declare it as a build tool, the same shape as `libarchive` and `curl` in `cix@v2.57.244-6.cbs`, and that is the same recursion. The only way out is vendoring cbs source into this repository, which is a second copy of a project maintained separately.
- **It does not give cixd a builder.** `cbs build` runs inside the composed build container, not in cixd. Linking would move `explain` in-process and leave the build exactly where it is.

It is also the wrong dependency for the binary that boots the machine. [ADR-0305](0305-a-recipes-format-is-its-filename.md) §3 rejected it because cixd is PID 1, so a crash in linked code is a kernel panic; because a cbs upgrade would become rebuild cixd, deploy and reboot; and because of the blast radius. Those grounds now have measurements:
- **Engine defects:** cix-build-system #216, #217, #218, #221, #229 and #230 are engine defects found in the last two weeks. Each was a failed build because cbs runs as a separate process. Linked, each was a candidate kernel panic.
- **Upgrade coupling:** the box stays on cbs v0.1.34 while #229 is open, and cixd does not care. Linked, shipping cixd would have meant choosing a cbs to ship with it.

**Image recipes are out of scope.** They are `image_packages=` declarations, line-scanned and never executed. They are `.sh` by extension only, and CPDL 0.1 has no image form.

## How cbs is installed today

- **On the host:** `/usr/bin/cbs` is staged into the control-plane root from the `cix-hosttools` image by `mkbootroot`. cixd runs it for `cbs explain` when a recipe is published.
- **In builds:** cixd adds `cbs` implicitly to every CPDL build's composed environment, from whichever image has it installed.
- **Installing or upgrading it:** `cixctl pkg install --name=cbs --image=<image> [--version=…] --upgrade`, the same as any package.
- **Today:** v0.1.34-1 is installed in `cix-builder` and `cix-hosttools`. Upgrades past it are blocked on [cix-build-system#229](https://git.home.arpa/itdlabs/cix-build-system/issues/229). In v0.1.50, `stage library` copies a symlink instead of the library it points to, and the engine fails its own `make test` on `cix-builder` at `fs-test.c:364`. v0.1.45 failed the same fixture's earlier form. v0.1.46 to v0.1.49 were never built here.

## Consequences

- The corpus keeps all 1,502 shell files. Its size is the cost of immutable history, not a parallel implementation.
- Both languages remain in cixd until the dependents in point 3 are gone. [#516](https://git.home.arpa/itdlabs/cix/issues/516) tracks them; the conversions carry the cost gates.
- A reader who wants to know which language a package uses reads its latest revision's extension. Nothing sniffs content (ADR-0305).
