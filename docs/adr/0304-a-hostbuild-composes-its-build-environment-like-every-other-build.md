# 0304 — A hostbuild composes its build environment like every other build

## Status

Accepted. Answers [issue #482](https://git.home.arpa/itdlabs/cix/issues/482). Extends [ADR-0199](0199-recipes-declare-their-build-tools.md) to the one path that was exempt from it, supersedes [ADR-0056](0056-hostbuild-artifact-mechanism.md)'s lowerdir-selection decision, and retires the `pkg_build_image` field [ADR-0230](0230-the-five-lifecycle-domains.md) introduced for issue #182.

## Context

ADR-0199 decided that a build container is composed from exactly what a recipe declares in `pkg_build_depends` and nothing else, and said why in terms that matter here: falling back to something fuller "would mean the build quietly ran against something fuller than it declared ... and it would succeed, teaching everyone that the declaration is decorative."

A hostbuild was exempt, and not by decision — by ordering. `pkg_build_container_spec()`'s if-chain opened with `if (g_chains[chain_idx].is_hostbuild)`, which rooted the build container on `--build-image`'s own rootfs and returned. The composition arm sat two branches further down, unreachable. So for the two recipes this platform depends on most — `cix` and `kernel`, the control plane and the thing it boots — `pkg_build_depends` was read by nothing at all.

**The declaration was not merely unused; it was known to be unused and worked around.** The `kernel` recipe's own changelog records the decision, in its 7.2.3-8 entry: *"wireless-regdb is added to kernel-builder 1.5.0 rather than to pkg_build_depends, because a host build sandbox is the build image rootfs and build_depends composes nothing there (ADR-0199)."* A correct diagnosis of the defect, written into a workaround for it. Meanwhile 6.18.40-24 had added the declaration precisely so the kernel *could* be built through the ordinary install path — so the recipe carried a 23-tool list that one path used and the other ignored.

Measured on 192.168.15.95, 2026-09-17: every tool `cix` (16) and `kernel` (23) declare is installed in the image each was built in, so the declarations were *true*. They were also incomplete, and nothing could tell, because `kernel-builder` additionally carried `perl`, `libxcrypt`, `linux-headers` and `wireless-regdb` that the recipe declared nowhere.

`pkg_build_image` had the same disease one level up. It was read in exactly one place, `pkg_hostbuild_start()`, so for the twenty-odd ordinary package recipes that declare it, the field did nothing whatsoever.

## Decision

**There is no hostbuild arm.** `pkg_build_container_spec()`'s chain now begins with the cache-hit case and falls through to ADR-0199's composition for anything that actually builds — for a hostbuild exactly as for an ordinary install. One answer to "what does a build run inside".

**`pkg_build_image` is retired, not repaired.** Composition makes it meaningless: the environment is derived from the declaration, so there is no image for a recipe or an operator to name. Gone with it: the `build_image` field on `POST /v1/pkg/hostbuild` and `POST /v1/system/kmod-build`, `cixctl`'s `--build-image=` on both commands, the dashboard's required "Build image" input, and three error codes that existed only to police the field — `PKG_ERR_WRONG_BUILD_IMAGE` (#182), `PKG_ERR_NO_BUILD_IMAGE`, `PKG_ERR_NO_SUCH_BUILD_IMAGE` (#317).

Making the field *enforced* rather than retired was the other candidate and is the weaker one: verifying that every declared tool is installed in the named image passes today for both recipes (measured), and would have left `perl` and `wireless-regdb` silently available exactly as before. Only composition makes an incomplete declaration fail.

**One addition was needed.** A cache-hit job takes the #144 no-op arm, which roots its container on the chain's *target* image — `__hostbuild` for a hostbuild — and wants a real rootfs path even though the container is a deliberate no-op. The build image used to supply that. `pkg_hostbuild_start()` now creates the sentinel image if absent (`IMAGE_ERR_DUPLICATE` is success, as `image_produce_new_version()` already treats it). The case is narrow and specifically the one that matters: a fresh host deploying `cix` straight from a checksum-verified artifact, which is the documented recovery route for a box whose resolver is broken (#138) and therefore cannot build from source.

## How the declarations were completed, which is the whole point

The owner's instruction was "everything needs declares, no exceptions", and the kernel's declaration was worked on **before** this shipped rather than after, because a hostbuild mechanism that cannot build the kernel is a box that cannot rebuild its own kernel. An ordinary `pkg install kernel --image=<throwaway>` composes the identical environment a composed hostbuild will, so it is a faithful proxy and needs no daemon change.

Two build inputs were missing, each found by a real build failure rather than by reading:

1. **`wireless-regdb`** — 7.2.3-13 failed in 13 seconds at the recipe's own precondition check: `/lib/firmware/regulatory.db is absent`. `CONFIG_EXTRA_FIRMWARE` embeds the database at build time, so it is a build input like `bison`. This is the one the recipe's changelog had deliberately moved into the image.
2. **`perl`** — 7.2.3-14 then reached the real compile, ran for twenty minutes, and died at `lib/Makefile:295` with `perl: command not found`; `lib/oid_registry_data.c` is generated by a perl script.

`libxcrypt` is deliberately *not* declared: `perl`'s own installed entry declares it (`pkg_depends="libxcrypt"`) and composition walks an installed package's own depends, so it arrives transitively. Declaring another package's dependency would be claiming something the kernel recipe does not itself need — and it is worth noting that this transitive resolution runs through `pkg_depends`, the field [ADR-0303](0303-a-hostbuild-carries-pkg-depends-it-does-not-resolve-it.md) had to make declarable at all.

**The declaration is complete; the kernel is not yet proven to build in a composed environment.** Those are different claims and only the first is established. 7.2.3-15, carrying both additions, compiled every object and linked every module, then failed at `scripts/Makefile.vmlinux:72` with `Error 1` and no diagnostic at all — and every post-link step in `scripts/link-vmlinux.sh` prints a message before exiting, so the failure is inside the link itself or `mksysmap`. **The cause is not established** (#484); nothing in the build log records which toolchain ran.

What is measured is a consequence of this decision that nothing forced anyone to think about while `pkg_build_image` existed. Composition resolved 25 declared tools to the 27 names `kernel-builder` holds, but `buildenv_add_tool()` resolves a bare name to *the newest installed copy*, and five of those are not the versions in the only image that has ever built this kernel: `binutils` 2.42-10 against 2.42-8, `glibc` 2.44-16 against 2.44-12, `elfutils` 0.192-12 against 0.192-9, `bison` 3.8.2-7 against 3.8.2-2, `make` 4.4.1-6 against 4.4.1-4. `gcc` matches.

So **retiring the field trades a hand-pinned image for an environment that floats to whatever is newest.** For a rolling-release platform that is the correct default and is why the decision stands as written — a build environment that silently stays years behind the platform it builds is the worse failure, and it is exactly what an image pinned by hand becomes. But it does mean a recipe that genuinely needs a specific linker must now say so, and #127's `name@version` is how. That pin is deliberately *not* applied to the kernel here: it would very likely turn this build green, and applying it before the cause is known would be a stop-gap that hides a real fact — that this kernel does not link against the platform's current `binutils`.

## Consequences

- **`cix-builder`, `kernel-builder` and `iso-builder` stop being special.** They remain ordinary images; nothing resolves a build through them any more. ADR-0208's taxonomy described which image had which job, and the job is now the recipe's declaration.
- **An already-published recipe's `pkg_build_image=` line is inert.** Published revisions are immutable, so the line stays in ~26 recipe files and is read by nothing; it disappears as each recipe next bumps. A sweep of 26 revision bumps would queue a rolling rebuild for every image tracking each package, which is a real cost for a line that does nothing.
- **The empty-declaration fallback to the shared build sandbox is the remaining exception**, and it is narrow: 2 of 149 recipes declare nothing (`lldap`, and `probe-hb-depends/1`, now superseded by `/2`). `lldap` cannot be fixed by declaring, because it needs a Rust toolchain staged from the build host and this platform has no `rust`/`cargo` package — so it is filed as [#483](https://git.home.arpa/itdlabs/cix/issues/483) rather than papered over.
- **`test_pkg`, `test_pkg_cache`, `test_kmod_build` and `test_kernelrecipe` all changed**, and two of those changes are the interesting ones: `test_kmod_build`'s fixture built with `gcc` supplied by a build image and now declares `tcc` (gcc is not in the ADR-0209 floor and cannot be composed), and `test_kernelrecipe` asserted that a generated kernel recipe keeps its `pkg_build_image` and now asserts it keeps its `pkg_build_depends` — the line whose loss would leave a recipe that composes nothing.
