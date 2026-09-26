# 0305 — A recipe's format is its filename, and CPDL is parsed by exactly one parser

## Status

Accepted. The first step of the owner-directed flip to CBS. Extends [ADR-0107](0107-version-keyed-recipes.md)'s version-keyed layout to a second recipe language, and [ADR-0199](0199-recipes-declare-their-build-tools.md)/[ADR-0304](0304-a-hostbuild-composes-its-build-environment-like-every-other-build.md)'s composition to a recipe that declares its tools in CPDL. Supersedes nothing. Three upstream gaps shaped what it could do at the time of writing and were filed rather than assumed: [cix-build-system#159](https://git.home.arpa/itdlabs/cix-build-system/issues/159) (no finalize policy from the CLI, which blocked CIXPKG adoption), [#161](https://git.home.arpa/itdlabs/cix-build-system/issues/161) (no home for `pkg_artifact_sha256`/`pkg_changelog`) and [#162](https://git.home.arpa/itdlabs/cix-build-system/issues/162) (a capability count instead of names). **All three closed upstream on 2026-09-18 and are verified running on 192.168.15.95** via `cbs v0.1.25-6`, built from cix-build-system `main` at `170dc744d`: #162 and #161 are read by the daemon (capability NAMES into `build_caps`, and `metadata { }` into `artifact_sha256`/`changelog`), and #159's `--finalize-command` is proven by upstream's own `cli-finalizer-test.sh` passing in a Cix build container. One half remains: cixd READS a CBS artifact approval and cannot WRITE one, tracked as [#492](https://git.home.arpa/itdlabs/cix/issues/492).

## Context

Cix recipes are shell scripts. A recipe is a `key="value"` header that `parse_recipe()`
reads with `extract_line_value()`, plus two functions (`pkg_build`, `pkg_install`) that
the daemon runs inside a build container as `. /build/recipe.sh; pkg_build; pkg_install`.
That format has carried this platform's entire package set — 149 packages, some with
dozens of revisions — and its limits are structural rather than cosmetic: a recipe is
executable shell, so what a recipe *declares* and what it *does* are the same text, and
the only way to know what a build will attempt is to run it.

`cix-build-system` (CBS) is the owner's successor. Its recipes are CPDL 0.1 documents —
declarative, validated before anything runs, with the build expressed as five ordered
phases (`prepare`, `configure`, `build`, `check`, `install`) rather than as a shell
function. The owner's word for the script format is **CBS**; the files carry the `.cbs`
extension, which CBS itself requires (`has_cbs_extension()` refuses any other path, in
both `explain` and `build`).

The owner's instruction is **full adoption, in coherent steps**, and their ordering:
support the mechanism, then build CBS itself with it, then flip every other package.
This ADR is the first step's decision record. It deliberately decides less than full
adoption needs, because two of the four stages depend on upstream work that is filed and
not yet answered (see Consequences).

## Decision

**1. A recipe's format is its filename.** `<recipes>/<name>/<version>/build.sh` is a
shell recipe; `<recipes>/<name>/<version>/build.cbs` is a CBS recipe. Nothing sniffs
content, and no recipe carries a field declaring what it is.

The alternative was a `format` field inside the recipe, or inferring the format from the
text (a CPDL document has no `pkg_name=` line; a shell recipe has no `package "…" {`).
Both were rejected for the same reason: they make it possible for a recipe to *claim* one
format and *be* another, and the failure would surface as a parse error deep inside
whichever parser was chosen by the claim. A filename cannot disagree with itself. It also
happens to be what CBS already insists on, so the extension is load-bearing regardless of
what we would have preferred.

**Both files present for one version is refused at publish**, not resolved by precedence.
One Source of Truth: a version that could be read two ways has no single answer to "what
will this build do", and picking one silently is how the `pkg_build_image` defect
(ADR-0304) lasted as long as it did.

**The REST body needs a discriminator, because a JSON object has no filename.**
`POST /v1/pkg/recipes` takes `{name, content}`; it gains an optional
`format: "shell" | "cbs"`, defaulting to `"shell"` so every existing client is unchanged.
This is not a retreat from the decision above — it is a routing hint that is *verified
immediately*, by the parse that follows it. `"cbs"` means the content must satisfy `cbs
explain --json`; `"shell"` means it must satisfy `parse_recipe()`. A body whose `format`
disagrees with its content is refused at publish, so the claim never survives to become a
stored file. What is stored is the filename, and from then on nothing asks again.

**2. cixd never parses CPDL.** Identity and declarations come from `cbs explain --json`.
The daemon maps that onto its own fields and knows nothing about CPDL's grammar.

This is the whole point of adopting someone else's build language rather than growing our
own. A second CPDL parser in `pkg.c` would be a parallel implementation of the thing we
are adopting, and the two would diverge on exactly the documents that matter — the ones
where the grammar is subtle.

**3. cixd execs the `cbs` binary; it does not link `libcbs`.**

Linking is the technically richer option and is rejected. `CbsFinalizePolicy` is a
callback, so only a linked embedder can supply ADR-0251's normalisation — which is
precisely what blocks stage 3 (cix-build-system#159). Against that:

- **cixd is PID 1 on an installed host.** A segfault in linked third-party code is not a
  failed build, it is `Attempted to kill init!` — a kernel panic this project has two
  recorded serial captures of. A separate process makes a CBS bug a failed build.
- **A `cbs` upgrade must stay `pkg install cbs`.** Linked, CBS becomes an ABI dependency
  of the control plane: a new CBS means rebuilding cixd, deploying, and rebooting. CPDL
  is 0.1 and we expect it to move.
- **Blast radius against benefit.** Linking vendors a build system into the binary that
  boots the machine, to gain one hook that is one small upstream feature away.

The choice is cheap to revisit because almost none of the code depends on it: the
discriminator, the identity mapping and the source handoff are identical either way.
Only the call differs, and it lives in one function.

**4. Identity is derived once, at publish, and persisted beside the recipe.**
`pkg_recipe_add()` runs `cbs explain --json`, which both validates the document and
yields its identity, and writes the result as `explain.json` in the same version
directory. Every later read — the recipe listing, dependency resolution, update
candidates — reads that file and forks nothing.

Without this the listing endpoint would fork one `cbs explain` per recipe file. There are
~1400 recipe files in this repo today, and #236 already measured the event loop blocked
for 10981 ms doing *shell* parses on a comparable walk, which needed the host reset by
hand. Twice.

Derived state is normally a second source of truth and normally forbidden. It is
legitimate here for one specific reason: ADR-0107 makes a published `(name, version)`
**immutable**, so the document `explain.json` describes cannot change under it.

What the file does **not** carry is which `cbs` produced it — it is that command's
output verbatim, and `explain --json` emits no version of its own. So a CBS release that
changed what those fields *mean* would go unnoticed here until something behaved oddly.
That is accepted rather than solved, on the grounds that such a change would be a
breaking change on CBS's side rather than a drift on ours, and that wrapping the output
to add provenance would cost the one property worth having: what is stored is exactly
what the engine said. A system restore does not copy this file at all — it re-derives it
(`pkg_recipe_rederive_identity()`), which is the case where a stale reading would
otherwise be resurrected months later.

**5. The engine is implicit in the composed build environment.** A CBS recipe does not
declare `cbs` among its build tools; `buildenv_resolve_tools()` adds it, exactly as it
already adds `PKG_BASE_LIBC` and for the same reason — without it the build cannot start
at all, and the resulting failure (`cbs: not found`) names the least useful cause. A
recipe that needs a specific engine revision may still declare `cbs@v0.1.24-4` and win
the slot, because `buildenv_add_tool()` already dedups by name and lets a recipe's own
pin occupy it.

This is the one place this ADR does not follow "everything needs declares" literally, and
the distinction it draws is between a package's *dependencies* and the *driver that runs
its build*. A shell recipe does not declare `sh` either.

**6. cixd's version string is `<version>-<release>`.** CPDL requires exactly one of each;
cixd has a single fused version string and no release concept (`pkg_release` appears
nowhere in `pkg.c`). The join reproduces every existing directory name for every package
that carries a `-N` suffix — `kernel` 7.2.3 + 15 → `7.2.3-15`, `cbs` v0.1.24 + 4 →
`v0.1.24-4` — and it matches how the artifact cache already reads our names: cixd
publishes `<name>-<version>-<arch>.tar.gz` and the cache splits the trailing `-N` off as
the release, which is why `kernel-7.2.3-15-x86_64.tar.gz` is stored as version 7.2.3
release 15.

`cix` itself is the one package whose versions carry no `-N`, so flipping it would rename
`v2.57.207` to `v2.57.207-1` — visible in `GET /v1/system/boot`, since `CIX_VERSION` is
the recipe's version string. That is a separate decision, taken when `cix` is flipped and
not before.

## Consequences

- **`build.sh` is referenced in six distinct concerns, not one.** Categorised before
  editing: path resolution (`find_recipe_path()`), the recipe listing, publish, `pkg
  sync`, artifact approval, and system backup/restore. Two of those fail *silently* if
  missed, which is why they are named here rather than discovered: **`pkg_sync_merge()`**
  would never import a `.cbs` committed to git, so a CPDL recipe would simply never reach
  a box; and **`do_system_backup()`** would omit it from a backup, so a restore would
  quietly lose it. A third, `sync_walk_image_recipes()`, covers image recipes — a
  different recipe kind, deliberately out of scope here.
- **Artifact approval had no CPDL home, and now half of one.** `approve_published_artifact()`
  rewrites the recipe's own `pkg_artifact_sha256=` line, and CPDL rejected unknown package
  keys (`validate_package()`'s `default:` arm → `CPDL-E9001`), so a CBS recipe could not
  take a cache hit and rebuilt from source on every install. Filed as cix-build-system#161,
  asking for an opaque embedder-owned metadata block; **closed 2026-09-18**, and
  `metadata { "artifact_sha256" "…" }` is now read by `parse_cbs_recipe()` into the same
  field the shell line lands in. What remains is the WRITER: that function splices a line
  into shell text anchored on `pkg_sha256="`, and the CPDL equivalent is inserting a key
  into a block that may not exist, which also needs a counterpart to
  `recipe_adds_only_artifact_sha256()` — the guard that permits exactly this one edit to an
  immutable recipe. Tracked as [#492](https://git.home.arpa/itdlabs/cix/issues/492). Until
  then a CBS approval works only when hand-written. It gates 88 of 148 packages — measured,
  and none of them early: `cbs`, `zstd`, `libarchive`, `kernel`, `gcc`, `binutils` and `cix`
  itself carry no approval today.
- **`pkg_changelog` had no CPDL home either**, so a CBS recipe listed with a null changelog.
  Same ticket, same close: it is the second key the daemon reads out of `metadata { }`.
- **`test_toolchain_policy` goes blind**, and is extended in the same change: it walks
  `recipes/package/*/*/build.sh` explicitly, so a `.cbs` recipe's toolchain declaration
  would escape the ADR-0224 count. `test_recipe_hygiene` needs nothing —
  `scan_credentials()` recurses over every file regardless of extension, so the
  credential gate already covers `.cbs` (measured, not assumed).
- **Host-side `cbs` is a deploy-gated dependency.** cixd reaches a `cix-hosttools`
  binary only because `mkbootroot` stages named absolute paths into the control-plane
  root, so `cbs` needs an install into that image, an `mkbootroot` change staging it with
  its libraries, and one deploy before `explain` can run at all. It links `libarchive`
  and `libzstd` and `dlopen`s `libcurl.so.4`, so it needs ADR-0154's explicit
  `LD_LIBRARY_PATH` at the exec site — without it the loader silently binds whatever the
  host happens to have, which is the failure mode ADR-0154 exists for. This cost is not
  specific to stage 1: reading a CIXPKG at install time needs the same binary.
- **The build container needs no network and no token.** CBS fetches sources itself over
  `dlopen("libcurl.so.4")`, but honours a pre-populated cache at `<cache>/<sha256>` and
  re-verifies before use (`source.c:288`). cixd keeps fetching host-side — it holds the
  repo token and performs the `{{REPO_TOKEN}}` substitution (#405) — and hands the bytes
  over by digest. The digest in the `.cbs` *is* the cache filename, so cixd's
  verification and CBS's agree by construction rather than by convention.
- **A stage-1 recipe declares `format "cixpkg"`, and that is not a choice.** The obvious
  reading is that it should say `"tar.gz"`, since cixd still finalizes and packages and a
  declaration that is not yet true is the defect ADR-0304 was written about. CPDL even
  validates `tar.gz` as a legal value. But `cbs_build_standalone_*` refuses to run a
  recipe whose format is anything else — `strcmp(declared_format(document), "cixpkg") == 0`
  sits in its own `ok` chain — so a `tar.gz` recipe cannot be built by CBS at all.
  The field therefore declares what CBS *would* emit if asked, and in stage 1 it is not
  asked: cixd passes no `--output`, CBS writes no artifact, and cixd tars the staged tree
  as it does for every shell recipe.
- **`--staged` names a WORKSPACE, not the staged tree.** CBS creates `W/src`, `W/build`,
  `W/dest` and `W/cache` under it, and an `install` phase's `${dest}` is `W/dest`. So the
  build sets `PKG_DESTDIR` to that subdirectory rather than moving a tree afterwards —
  one environment variable instead of copying a package's entire installed content. Both
  of these were measured in CBS's own `package.c` before the first build rather than after
  it; each would otherwise have cost a full cycle on the box.
- **Nothing about provenance changes.** `cbs` is a Cix-built package, built by TCC on a
  Cix host; the build still runs in a container composed from the recipe's declaration;
  sources are still checksum-verified by cixd before the container sees them.
