# 0330 — A followed image holds its recipe, and nothing else

## Status

Accepted, 2026-10-05, the owner's decision on cix#571. Asked "when a package is removed from an image's recipe in git, should the box uninstall it from that image automatically?", the owner answered *"Yes"*. It amends [ADR-0320](0320-an-image-policy-decides-how-its-three-copies-agree.md), which decided how an image's recipe, manifest and installed set agree, but only ever gave `recipe: follow` and `apply: converge` the power to add and move packages.

## Context

ADR-0320 made git authoritative for an image under `recipe: follow`. In practice it was authoritative for half of it:

- **Applying a recipe only set entries.** `pkg_image_recipe_apply()` called `image_manifest_set()` for each entry the recipe listed. An entry the recipe dropped stayed in the manifest for good.
- **Converge only installed.** It installed, upgraded or downgraded entries to match. Nothing uninstalled a package the recipe no longer covered.

Measured on 192.168.15.95, 2026-10-05, running 0.2.57-461:

- `cix-hosttools` 2.4.1 dropped `tar`, `gzip` and `bzip2`. The apply left them in the manifest, still pinned from the old recipe, and converge reinstalled the three packages that had just been removed by hand.
- The four images that follow and converge held **46 packages** their recipes did not declare: `cix-builder` 23, `cix-hosttools` 21, `jumpbox` 3, `kernel-builder` 1.
- Some of those were load-bearing. The cix release recipe's own build tools (`cbs`, `curl`, `libarchive`, `minisign`, `quickjs`) sat in `cix-builder` undeclared. Nine build tools of the ISO toolchain lived **only** in `cix-hosttools`: grub, shim, sbsigntools, mokutil, efivar, dosfstools, freetype, unifont and e2fsprogs. They were installed there by hand because the `iso-builder` image, whose recipe declares them, had never been created on the box.

So the recipe in git described part of each image, and the box held the rest without any record of why.

## Decision

**Under `recipe: follow` and `apply: converge`, an image holds its recipe's runtime closure, and nothing else.**

1. **Under `recipe: follow`, the manifest is the recipe.** An apply removes every manifest entry the recipe no longer lists, logs each one by name, and reports the count as `removed` in the apply result. Under `recipe: manual` the manifest stays the operator's, and an apply only adds and moves entries, as before.
2. **Under `recipe: follow` with `apply: converge`, converge uninstalls.** Once every manifest entry is satisfied, the rebuild drain uninstalls every package in the image outside the closure of its manifest. The closure is walked through the `depends` each installed package recorded when it was fetched (ADR-0302), which is what that build actually needed to run. Each uninstall is logged by name, and so is each one that fails.
3. **An image that did not reach its manifest is not trimmed.** If any entry's install could not start, the drain logs that entry and its reason, and does not uninstall anything that pass. Before this, a refused install was silent, and the drain then described the image as caught up. `iso-builder`'s first converge installed none of its 40 entries and logged nothing.
4. **One image per job.** A package an image needs, a build tool included, belongs in that image's recipe. A tool installed by hand into an image that does not declare it is an undeclared second job, and the converge will remove it.

`pkg_error_describe()` moves from `main.c` to `pkg.c`, so the drain can log a refusal with the same sentence a request would get.

## Before the code: every recipe made true

Shipping clause 2 against the recipes as they stood would have uninstalled the cix release's own build tools at the next converge. So the recipes came first (cix-recipes, 2026-10-05):

- `iso-builder` 1.4.0 is created on the box and becomes the ISO toolchain's one home, every entry rolling, with freetype and unifont added.
- `cix-builder` 6.2.1 declares the 20 build tools it held without declaring them.
- `cix-hosttools` 2.4.2 declares `cbs`, which mkbootroot copies into the root.
- `jumpbox` 2.9.6 declares bind-utils and diffutils.

After that, what the converge removes is only what nothing uses:

- from `cix-builder`: dnsmasq, go-bootstrap, hibr and probe-argspace;
- from `kernel-builder`: the `kernel` package;
- from `cix-hosttools`: tar, gzip and bzip2, plus the ISO toolchain, which now lives in `iso-builder`.

## Consequences

- **Removing a package from a followed image is a commit to its recipe.** `pkg rm` on such an image is undone at the next converge only if the recipe still lists the package, and an explicit `pkg install` lasts until the next apply of the recipe.
- **An image's recipe is now its complete description.** `cixctl image show` and the recipe in git can be read against each other and agree.
- **A converge can remove things the operator did not type.** That is the decision. It is limited to images whose policy says follow and converge, and every removal is logged by name.
- **Gated in the release selftest.** `test_image_recipe` checks the manifest removal, and `test_pkg` (floor) checks the uninstall end to end: a followed, converging image installs two packages from its recipe, then uninstalls the one the recipe drops and keeps the other.
