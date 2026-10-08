# 0333 — An image's content comes only from packages

## Status

Accepted, 2026-10-08, the owner's direction. Supersedes [ADR-0332](0332-an-image-version-identifies-its-baseline-too.md), which was Proposed and never shipped. Amends [ADR-0023](0023-per-image-runtime-seeding.md), [ADR-0041](0041-container-image-baseline-fhs-layout.md), [ADR-0296](0296-the-platform-keeps-nsswitch-correct.md) and [ADR-0331](0331-the-image-baseline-and-the-device-policy-read-one-list.md) — each of which put something into an image by a route other than a package.

Stated by the owner on 2026-10-08, in these words: *"I want the baseline to be a package, 100% no other way to orchestrate an image except through the packages (for content), and json config of the image"*.

## Context

An image had **four** writers of its content: its installed packages, and three things `pkg_seed_image_baseline()` created directly — device nodes, a written `/etc/nsswitch.conf` and the directory layout around it, and the host's CA trust bundle. Only the first was versioned, declared, or visible anywhere.

**The project had already started down this road and stopped half way.** Until [ADR-0216](0216-the-glibc-floor-is-closed.md) the baseline also copied the dynamic loader, libc, libm and the files-NSS backend off the build host — [ADR-0209](0209-derived-images-and-one-container-mechanism.md) named them "the only files in any image that this project did not build". `glibc` became an ordinary recipe, and the function's own comment records the result: *"Nothing in any image is copied off the build host any more. What this function still does below — device nodes, directories, a written nsswitch.conf — it CREATES; it does not borrow."* A freshly created image has no C library, and `POST /v1/containers` refuses it by name rather than letting it surface as exit 127. That is the precedent this ADR generalises.

**What the remainder cost.** #577 added `tty` (5:0) to the baseline and shipped in 0.2.57-468. A freshly created image got the node; `jumpbox` did not — its device nodes were dated `Sep 21 17:29`, weeks older than the fix — so `sudo`, `su` and `ssh -t` still failed on the one image whose purpose is humans logging in, while #577 read as closed. #578 added a sixth node the next day and would have landed identically.

ADR-0332 proposed fixing that by giving the baseline a generation and folding it into the image-version hash. It was implemented and passed its gates. **It solves the wrong problem**, and the owner's question — why is the baseline not a package? — is what exposed it: it versions a fourth writer of image content instead of removing it. Every mechanism it needs exists only because the baseline is not a package.

## Decision

**An image's content comes only from its installed packages. Its intent comes only from its JSON. Nothing else writes content into an image.**

1. **Content is packages.** Every file in an image is there because a package put it there — recorded in the manifest, with a version that can be upgraded, downgraded and audited, and an origin traceable to a recipe and an artifact approval.

2. **Intent is the image's JSON** — its manifest and its recipe, reconciled by its policy ([ADR-0320](0320-an-image-policy-decides-how-its-three-copies-agree.md)). There is no third kind of input.

3. **`pkg_seed_image_baseline()` ceases to exist, and *nothing it did becomes a package*.** That is the finding that decided the shape of this ADR, and it is the opposite of what "make the baseline a package" first suggests: measured against the code, every one of its four jobs is **per-container state**, and two of them are already done at container creation today — redundantly.

   | What the baseline did | Where it belongs | Evidence |
   |---|---|---|
   | `/run` and `/dev` directories | Already created at every container start | `mountns.c:239` mkdir + a **fresh tmpfs on every start**; `mountns.c:309` `mkdir("/dev")`. The image copies are dead weight. |
   | 6 char device nodes, `/dev/ptmx` | Staged host-side into the container's upperdir by the daemon | `stage_container_file(upperdir, …)` is how `/etc/passwd` already arrives. The daemon runs as real root and holds `CAP_MKNOD`. |
   | `/etc/nsswitch.conf` | Same, and it already has that path for one variant | `main.c:15291` stages the `ldap_client` variant per container today. |
   | CA trust bundle | Same — it is host-generated, not content | `pki_write_trust_bundle_file()` writes *that host's* own root and intermediate PEM. |

4. **Device nodes are staged by the daemon, not created by the container and not carried in a package.** Both alternatives were measured and both fail:

   - *The container cannot create them.* `mknod` is gated on `CAP_MKNOD` **against the initial user namespace, which no userns container has ever held** (`src/container.c:810`, from #321) — and userns is the default ([ADR-0179](0179-user-namespaces-by-default-subordinate-id-allocation.md), [ADR-0207](0207-btrfs-storage-substrate-userns-by-default.md)). This is the real reason the nodes were in the image, and it is not an accident.
   - *A package cannot carry them.* Measured on 192.168.15.95, 2026-10-08: **zero** of the installed packages ship any `dev/` path, `coreutils` as built here ships no `mknod` binary, and CPDL has no `mknod` operation — so nothing in the build path can create one. Whether the `.cixpkg` format could preserve one was therefore never reached, and does not need to be.

   Staging host-side satisfies both: the daemon has the capability the container lacks, and the nodes stop being image content. It also **tightens** ADR-0331 rather than relaxing it — the staging and `container_dev_bpf_attach()`'s allow set expand the same `CIX_BASELINE_DEVICES` list, in one file, at one moment, so a node the policy denies is not expressible. Today the nodes live in the image and the policy is decided per container, so the two agree only by discipline.

5. **ADR-0332's baseline generation is removed, not kept.** With no fourth writer, ADR-0108's hash already covers all of an image's content, and ADR-0155's "silently discards correct fixes" trap closes for the same reason. Keeping a mechanism whose superseding decision has landed is the stop-gap the No Stop-Gaps maxim forbids and the parallel implementation No Parallel Implementations forbids.

6. **"No other way" is the point, not a simplification.** A future need to put something into an image is a recipe, or it is not image content. A second route is what produced #577, #578 and ADR-0332's entire mechanism.

## Consequences

- **The #577/#578 class of bug cannot recur, and not because a trigger catches it.** A device the baseline gains is staged at the next container start, for every container, from the one list — there is no image tree to go stale. A platform change reaches a running workload when it is next created or restarted, which is the same contract every other per-container file already has.
- **Nothing needs to propagate to existing images at all**, so the churn ADR-0332 weighed — every image's version string changing once, one extra version directory per image — does not happen. No release-wide version churn, and no migration commits to the five followed image recipes.
- **An image becomes exactly its manifest.** `GET /v1/images/{name}` already answers what an image holds; afterwards that answer is complete rather than nearly complete.
- **Old image trees keep their now-unused `/dev` nodes and `nsswitch.conf`.** Harmless — a container's upperdir shadows them — and they age out as versions are reclaimed (#551). Nothing is written into an existing version's tree, so ADR-0107/0108 immutability is untouched.
- **ADR-0296's requirement is better served, not weakened.** It made the platform converge `nsswitch.conf` because a container given resolvers could not use a `hosts: files` file. Written at every container start from live state, it cannot be stale and needs no convergence pass.
- **The ownership detail is real work, not a footnote.** `stage_container_file()` takes an `id_offset` so a staged file lands owned by the container's mapped root; a staged device node needs the same treatment, and `nodev` must stay cleared on a userns rootfs (#173) or the nodes are present and unopenable.
- **Build containers are covered** — they go through `container_create()` like any other, so they get the staged nodes a `./configure` script needs. Measured alongside: there are **no `chroot()` callers anywhere** in the daemon, runtime library, host tools or tests, and `test_image_fixture_host.c` stages its own deliberate subset for build-image fixtures, independent of this path. So nothing reads device nodes out of an image tree without creating a container.
- **The remaining unversioned image surface is zero.** That is the property worth having: there is no "and also the platform puts some things in" clause left to forget.

## Alternatives rejected

- **Version the baseline in place — [ADR-0332](0332-an-image-version-identifies-its-baseline-too.md).** Implemented and green before this decision. It makes the identity correct and leaves a fourth, unversioned writer of image content in place, with a hand-maintained generation, a re-seed trigger on the converge path, and a gate that can only cover the device list while the rest rides on a comment. Rejected because the content model, not the hash, was what was wrong.
- **Make the baseline a literal package.** The direct reading of the owner's words, and measurement says the content is not package-shaped: the trust bundle is host-generated, `nsswitch.conf` is per-container, `/run` is a fresh tmpfs every start, and nothing in the build path can create a device node. A package would also decouple the nodes from the policy, re-opening the divergence #578 was and ADR-0331 closed. The direction is honoured by removing the content from the image, which is what it was for.
- **Add a CPDL `mknod` operation so devices can ship in a package.** A real option, and an upstream request to cix-build-system. Rejected as unnecessary once staging host-side works: it would add a build-system feature, an artifact-format question, and a new gate asserting the package's nodes against the BPF list — to reach a worse place than the daemon writing six nodes where it already writes `/etc/passwd`.
- **Keep the trust bundle in the image, regenerated on change.** Putting host-specific generated state into an immutable, checksummed, cache-published artifact is a category error, and regenerating it into existing version trees breaks the immutability ADR-0107/0108 rest on.
