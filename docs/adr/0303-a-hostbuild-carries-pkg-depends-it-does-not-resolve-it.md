# 0303 — A hostbuild carries `pkg_depends`, it does not resolve it

## Status

Accepted. Answers [issue #465](https://git.home.arpa/itdlabs/cix/issues/465). Supersedes the "a hostbuild recipe must have empty `pkg_depends`" decision in [ADR-0056](0056-hostbuild-artifact-mechanism.md); resolves the first of the two shapes named as an open question in [ADR-0291](0291-the-control-plane-root-is-an-image.md).

## Context

`pkg_hostbuild_start()` refused outright — 400, before the job was queued — any recipe whose `pkg_depends` was non-empty. ADR-0056 stated the reason: dependency resolution targets "merge into an image," which is meaningless for a one-shot artifact harvest, and every prerequisite must already be baked into `build_image`'s own rootfs.

Both halves of that sentence are true. Neither is about `pkg_depends`.

**What composes a hostbuild's build environment is `build_image`, and nothing else.** `pkg_build_container_spec()` opens with `if (g_chains[chain_idx].is_hostbuild)` — the *first* arm of its if-chain — and roots the build container on that image's current version. The arm that composes an environment from a recipe's declared build tools (`buildenv_image_for(recipe.build_depends, ...)`, ADR-0199) is two branches further down and is never reached for a hostbuild. So a hostbuild consults neither depends field when deciding what it builds inside: `pkg_build_depends` is ignored, and `pkg_depends` was refused. The refusal's stated justification was a correct statement about the field it was not checking.

**What `pkg_depends` actually describes is what the finished artifact needs in order to run** — and the only machinery that reads it is the ordinary-install path. `declared_provided_sonames()` walks the declared closure to feed the #389 undeclared-link gate, and that call sits inside the non-hostbuild arm of `pkg_build_completed()`. A hostbuild harvests into `ARTIFACTS_DIR/<name>/` and never reaches it.

This left `cix`'s own recipe unsatisfiable in both directions at once, which is how the contradiction surfaced. `cixd` links `-lssl -lcrypto -larchive -lcurl` (`Makefile:418`), so an ordinary `pkg install cix` into an image needs `pkg_depends="openssl libarchive curl"` or the #389 gate refuses it — while declaring exactly that made `pkg hostbuild cix`, the only way this platform builds itself, reject the recipe as invalid. ADR-0291's `cix-boot` design runs straight into this, since a manifest-declared `cix` is installed by the ordinary path.

## Decision

**A hostbuild accepts `pkg_depends` and carries it; it does not resolve it.** The refusal is deleted. Nothing else changed, because nothing else had to:

- `pkg_build_completed()` already records the declaration onto the entry, from the chain's captured copy ([ADR-0302](0302-a-jobs-version-is-decided-once-not-re-derived.md)), **before** the hostbuild/install split, and `save_state()` already persists it. So the recording needed no change; only its *visibility* did.
- The hostbuild path already builds a single-entry `dep_queue` with no `resolve_chain()` call, so there was no resolution machinery to disable.

**One thing did have to change, and it was found by checking rather than by reasoning.** `PkgEntry` had no `depends` key at all — measured on 192.168.15.95, 2026-09-17, against the deployed v2.57.204 that carries the refusal removal: `GET /v1/pkg/openssl` and `GET /v1/pkg/hostbuild/cix` both returned entries with no such field. The `jw_key(&w, "depends")` that looked like the REST serialiser is inside `save_state()`, the on-disk persistence writer. A declaration that is recorded, persisted, read by an internal gate and visible to no client is indistinguishable from one that was discarded — which is exactly the "accepted and silently ignored" trap this ADR argues against. `depends` is therefore added to `PkgEntry`, carrying what the entry declared when it was built. No CLI table column: that listing is fixed-width and already wide, and `--json` surfaces the field.

The distinction this draws, and the reason it is a decision rather than a relaxation: **resolution is skipped because resolving means "install the closure into an image" and a hostbuild has no image to merge into — not because the declaration is meaningless.** A field that is accepted and silently ignored is a trap; a field whose meaning is stated per mode is a contract. In hostbuild mode `pkg_depends` is metadata about the artifact, recorded and carried forward. In install mode it is additionally the input to dependency resolution and to the linkage gate.

Deleting the refusal also removes a real ambiguity at the API boundary. `pkg_hostbuild_start()` had exactly two `PKG_ERR_INVALID_RECIPE` returns — a genuine parse failure and this refusal — which a caller received identically as a bare 400. There is now one, so the error means what it says.

## Alternatives considered

- **Give the refusal its own error code and keep refusing.** This preserves the contradiction it exists to create. The field would still be un-declarable on the one recipe that most needs it.
- **Keep hostbuild's gate and give `cix-boot`'s manifest install a path that consumes the verified artifact without resolving** (ADR-0291's second shape). Still a reasonable thing to build for its own reasons, but it does not fix this: it routes *around* the refusal rather than correcting it, and it leaves an elfcheck gate that never runs against `cix`, which is not a gate.
- **Also stop ignoring `pkg_build_depends` for a hostbuild** — i.e. compose a hostbuild's environment from its declared tools the way ADR-0199 does for an ordinary build, instead of trusting `build_image`. That is a second, genuine inconsistency, found while reading this one, and it is deliberately **not** fixed here: a hostbuild's whole point is that it builds inside a named, operator-prepared image, and changing what composes that environment is a change to how the kernel and the control plane are built. Filed as [#482](https://git.home.arpa/itdlabs/cix/issues/482) rather than smuggled into this one, with the three shapes weighed and the measurement each would need.

## Consequences

- `cix`'s recipe can declare what `cixd` links, truthfully, for the first time — which is the prerequisite ADR-0291 phase 2 named.
- A hostbuild recipe's `pkg_depends` is now *documentation that travels with the artifact* and is read back by anything installing that package the ordinary way. It is not a promise that the hostbuild installed anything.
- `test_pkg.c` step 17's fixture inverted: it asserted the 400, and now asserts acceptance. The dependency it declares is deliberately a name no recipe in the fixture set has, so three things are proven by one build — reaching `installed` proves nothing resolved it (resolution could only have failed), the recorded `depends` field proves it was not discarded, and a 404 for the name proves nothing installed it.
- `test_pkg` is in none of the `SELFTESTS` lists (#224), so that fixture is not a release gate.
- **The change lands over two steps, in this order, and it cannot be done in one.** A recipe declaring `pkg_depends` cannot be *published* until a daemon that accepts it is *running*: publishing one against the daemon being replaced gets the very 400 this removes. So the deploy that carries the fix uses a recipe with `pkg_depends=""`, and `cix`'s declaration is published against the daemon it deployed. Expect the same shape for any future change that widens what a recipe may say.
