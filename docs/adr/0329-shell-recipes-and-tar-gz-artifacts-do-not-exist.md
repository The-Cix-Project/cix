# 0329 — Shell recipes and `.tar.gz` artifacts do not exist

## Status

Accepted, 2026-10-03, the owner's direction on cix#569: *"What are shell recepies, those should not exist, please deprecate them."* It supersedes [ADR-0309](0309-shell-recipes-are-history-the-shell-path-retires-with-its-last-dependent.md) clause 1 (published shell recipes stay, in the repository and on hosts) and the shell half of [ADR-0307](0307-a-packages-artifact-format-is-the-one-its-recipe-declares.md) clause 1 (a shell recipe publishes `.tar.gz` by construction). It finishes what [ADR-0328](0328-cix-produces-no-tarball.md) left open under "Not decided here". The One Build System Mandate in CLAUDE.md already said a recipe is CPDL and an artifact is `.cixpkg`; this is the change that makes the daemon agree.

## Context

ADR-0309 kept published shell recipes as history and retired only the shell *build* path, which went in `v2.57.344`. What remained in cixd was everything needed to *hold* a shell recipe and *consume* what one produced:

- a line-scanning parser for `pkg_name=`/`pkg_version=`/… headers, used at publish, at sync and whenever a stored version was read;
- a `format` field on `POST /v1/pkg/recipes`, choosing between `shell` and `cbs`;
- an approval-only republish, the one in-place edit a shell recipe allowed, adding `pkg_artifact_sha256=`;
- the `tar.gz` artifact format: its fetch suffix, its cache entries, and the extraction that installed one;
- two-segment backup keys (`<name>/<version>`) that meant "a shell recipe".

The reasons ADR-0309 gave for keeping shell recipes no longer apply. Measured on 192.168.15.95 and the LAN cache, 2026-10-03:

- **Installed versions.** ADR-0309 counted 67 installed versions built from a shell recipe. Today there is one, `gcc@16.2.0-13`, in `cix-builder` and `kernel-builder`. `gcc-16.2.0-18`, built from CPDL, is in the LAN cache as a `.cixpkg`.
- **Image pins.** The latest image recipes in cix-recipes pinned 55 versions that resolved to shell recipes. 53 moved to same-upstream CPDL revisions in cix-recipes `20139f2`. The other two are the `gcc` pins in `cix-builder` and `kernel-builder`.
- **Release record.** The `cix` release recipes already retired with their artifacts (ADR-0313). Every shell recipe that ever existed stays in cix-recipes' git history, which is the record. A stored copy on a host is not a second one.
- **Approval trust.** A shell recipe's approval vouched for a `.tar.gz`, which nothing will fetch any more (below).

The store on .95 held 991 shell recipe versions against 805 CPDL ones. Nothing installed or pinned needs those 991 once the `gcc` pins move.

## Decision

**A recipe is CPDL and an artifact is `.cixpkg`. cixd holds and consumes nothing else.**

1. **Publishing is CPDL only.** `POST /v1/pkg/recipes` has no `format` field. Content must satisfy `cbs explain --json`, and is stored as `<name>/<version>/build.cbs`. `parse_recipe()` reads only a `build.cbs`; the shell header parser is deleted. A sync reads only `recipes/package/*.cbs`.
2. **An approval is never a republish.** The shell approval-only republish is removed. A CPDL approval arrives through `approve_cbs_artifact()` after this host builds and pushes (#492), or through a git sync refresh (ADR-0324). The sync status's `approved` counter only ever counted the removed path, so it is removed too. A git-carried approval was already counted as `refreshed`.
3. **The only artifact format is `cixpkg`.** A recipe declaring anything else is refused at publish, as a CPDL `format "tar.gz"` already was. The fetch, the local cache, the cache hit, the push and the ISO seed all name `.cixpkg`, and each refuses an unknown format rather than defaulting to one. The `.tar.gz` extraction path is deleted.
4. **What a host still holds is removed at startup.** `retire_shell_recipes()`, in `pkg_init()`, deletes every stored `build.sh`, and each version and name directory that leaves empty. `retire_targz_artifacts()`, in `pkg_cache_init()`, deletes every `*.tar.gz` and `*.tar.gz.minisig` in the local artifact cache. Only regular files with exactly those names are removed, and `rmdir()` refuses a directory that still holds anything, so no CPDL recipe is touched. One log line reports how many of each were removed, and how many removals failed.
5. **A backup keys every recipe `<name>/<version>/build.cbs`.** A restore skips a two-segment key, a shell recipe from an older backup, and logs that it did.
6. **The #494 artifact-name collision check is removed.** It refused a version whose artifact name another version already owned, which needed a version with no release (`1.25.0` beside `1.25.0-1`). CPDL always joins `version` and `release`, so with no shell recipe in the store that collision cannot occur.
7. **cixd stages no source into the build container.** It used to unpack `source[0]` into `/build/src` and copy the other sources to `/build/extra/<basename>`, where a shell recipe's `pkg_build()` read them (ADR-0036). A CPDL build never reads either: cixd hands every source to cbs as a cache entry named by its sha256 (ADR-0305), and cbs unpacks it inside the container. The unpack was also the one place cixd could exec an external decompressor: five recipes have a `.bz2` main source (elfutils, gcc, ipset, libmnl, shim), and the libarchive cixd links is built `--without-bz2lib`, so it ran the root's `bzip2`. `/build/extra` now holds only the `kmod-extra.config` cixd writes for a kmod build.
8. **The control-plane root carries no `tar`, `gzip`, `xz` or `bzip2`.** After clauses 3 and 7, the only archive cixd opens is a recipe source's git archive, a `.tar.gz` that libarchive reads with zlib in-process. cbs reads a `.cixpkg` itself, and none of cbs's sources executes any of the four. They leave the program list (`include/controlplane_programs.h`) and mkbootroot's staging, together with the libraries staged only for them: libacl, libselinux, libpcre2-8 and libattr for `tar`, and libbz2 for `bzip2`. No Cix package builds any of those five.
9. **mkbootroot checks the assembled root's library closure before sealing it.** `shelled_bin_libs[]` is a hand-kept list, and nothing compared it to what the staged binaries need, which is how those five libraries outlived their consumers. `verify_root_closure()` runs `elfcheck_undeclared_links()` over the root and refuses to seal when an ELF object needs a soname no file in the root provides, naming both. Removing a library is therefore checked by the assembly, not discovered at boot.

## Ordering

Clause 4 deletes `gcc@16.2.0-13`'s recipe, and `cix-builder` and `kernel-builder` still held 16.2.0-13 when this was decided. The plan was to move them to the cached `16.2.0-18` first. That did not survive measurement, and does not need to:

- **16.2.0-18 cannot install anywhere.** Its `cc1` links `libzstd.so.1`, which its recipe never declares, so cix#389's gate refuses it (192.168.15.95, 2026-10-04). Configure had detected zstd in the composed build environment, although with `--disable-lto` it has no use. `gcc@16.2.0-19` configures `--without-zstd`.
- **Nothing depends on 16.2.0-13's recipe.** The images keep the files they hold, and a build composes the newest installed copy of a tool from any image, which is already 16.2.0-18 (`probe-gcc`, `probe-verify`). So 0.2.57-460 ships without waiting for gcc.
- **The images move on their own.** The three build images now roll every entry (cix-recipes `8cde290`). A 0.2.57-459 daemon cannot apply them, because its sync reads only `.sh` image recipes (fixed in this release). Once 460 runs, they converge onto 16.2.0-19 once it is built. The last `.tar.gz` in the LAN cache, `gcc-16.2.0-13`, is deleted after that.

## Consequences

- **The LAN cache's 452 `.tar.gz` artifacts are deleted**, since no host will request any of them; `gcc-16.2.0-13` goes last, once the `gcc` pins have moved. The public cache is synced from it by the owner, not by this project.
- **An older backup restores without its shell recipes.** They are skipped by name with a log line, not failed on. A backup is restored onto a host whose daemon would delete them at its next start anyway.
- **A client still sending `format` sends a field the daemon does not read.** `cixctl` and the dashboard no longer send it.
- **A root that needs something it lacks fails its assembly**, naming the file and the soname, where it would once have sealed and failed at boot. The first assembly with clause 9 is also its first measurement of the real root.
- **Image recipes were already JSON-only** (ADR-0311 clause 4, completed in cix#569 before this change). With this, no `.sh` recipe of any kind is read anywhere in cixd.
