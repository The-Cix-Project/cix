# 0198 — The build sandbox is an ordinary image, not a second mechanism beside images

## Status

Accepted

Applies [ADR-0107](0107-package-image-versioning.md)/[ADR-0108](0108-image-version-content-hash.md)'s image model to the one thing that had been living outside it.

## Context

Every ordinary package build runs in a container whose lowerdir is a shared toolchain sandbox. That sandbox was never a declared thing:

- It lived at a flat `<images_dir>/pkgbuild/rootfs`, not the versioned `<name>/<version>/rootfs` + manifest layout every real image uses. No history, no `current_version`, nothing to inspect.
- It was seeded once, straight to disk, bypassing the image mechanism entirely.
- And then it was grown silently by **every successful ordinary install** — a second, parallel `merge_tree()` alongside the real versioned merge into the actual target image.

So its content was a function of one box's entire install history, expressible nowhere and comparable to nothing. That is not a tidiness complaint: it made "what is `/usr/bin/gcc` in this sandbox, and how did it get there" genuinely unanswerable, and it is why a recipe that built once could stop building with no visible change anywhere — which is exactly what happened to `gcc/4.7.4` while working [issue #55](https://git.home.arpa/itdlabs/cix/issues/55), blocking a fix that was otherwise ready.

## Decision

**The sandbox is the `cix-builder` image, resolved exactly the way a hostbuild's own `--build-image=` already is.** One mechanism, not two: `image_current_version()` + `image_version_rootfs_path()`, the same two calls the hostbuild path three lines above it was already making.

Reusing `cix-builder` rather than introducing a new name was the operator's decision, made deliberately over the alternative. It has a real consequence, stated here rather than discovered later: `cix-builder` is now grown by **every** ordinary install, not only by an explicit `pkg install --image=cix-builder`, so a hostbuild's own inputs move when unrelated things are installed. The concern that motivated the alternative was exactly that — and the answer is that it is now *visible*: the image has real version history to see the movement in, and to roll back to.

Seeding follows: both bootstrap paths produce a real first version through `image_produce_new_version()` instead of writing to a flat directory, and the accretion merge becomes a real new version too.

### The trap this design walks into, and the way out

An image's version identity is a hash of its **manifest** (ADR-0108). Folding a package into the sandbox adds no manifest entry — so the naive implementation computes a byte-identical hash, concludes the version already exists, discards the staging copy, and reports success while losing the merge entirely. That is [ADR-0155](0155-baseline-reseed-manifest-hash-dedup-gap.md)'s same-manifest trap arrived at from the other direction, and it fails silently: every subsequent build runs against stale content with nothing anywhere reporting a problem.

`image_produce_new_version()` therefore takes an optional extra identity component, used by this one caller and NULL for every other. The sandbox's identity folds in the **previous version** as well as the change being applied, making it a real chain — `v(n+1) = H(manifest, v(n), pkg@version)` — so two different histories that happen to merge the same package last still land on different versions.

The regression test asserts the version actually advances across an install, not merely that the install succeeded. Verified by removing the extra identity and watching `test_pkg` fail — and it does not fail gently: packages get stuck in `building`, because later recipes no longer find what earlier ones installed.

### Migration, because a refactor must not smuggle in a content change

Simply resolving the image instead would silently discard everything that had accreted into the flat directory. That would turn a structural change into an unannounced content change — the kind of thing that should never ride along inside one.

So an existing flat `pkgbuild/rootfs` is folded in as a real version of the image, once, at startup, and then **set aside rather than deleted** — renamed with the version it produced, so the previous state is still on disk if the migration proves wrong. What the sandbox *contains* is unchanged by this; only how it is tracked changes.

**And if the migration fails, builds keep using the flat directory.** That safety net is not defensive padding — it is there because the first version of this change did not have it, and the consequence on the real box was immediate and total: the migration failed, builds silently switched to the image's own declared content, and the very next install died on `sed: command not found`. `sed` is one of the many packages that had only ever accreted into the flat sandbox and was never in the image's manifest — which is precisely what this whole issue is about, biting the migration first. The box was rolled back within minutes and the fallback added.

The lesson generalises past this change: the image's declared content is **not** a substitute for the accreted sandbox, and anything that assumes it is will fail on whichever undeclared package a given recipe happens to need. Until the declared-package-list half of #40 lands, the accreted content is load-bearing.

## Consequences

What is in the build sandbox is now inspectable through `GET /v1/images/cix-builder` like anything else, has history, and can be rolled back. A recipe that stops building can be compared against the version it last built under.

**What this does not yet do** is make the content *declared*. The image's manifest still lists only what an operator installed into it directly; the accreted union is real content without a corresponding manifest entry. Closing that is the remaining half of issue #40 — publishing a real `image_packages=` recipe for it — and it is now possible precisely because the versioning exists to hang it on.

The failure to fold a package into the sandbox stays non-fatal, as it was before: the authoritative install already succeeded, and a future recipe losing build-time visibility of one package is a smaller problem than unwinding a good install over it. It is now *reported* rather than silently ignored, which the previous best-effort call was not.
