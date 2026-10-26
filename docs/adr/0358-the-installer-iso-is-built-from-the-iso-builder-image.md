# 0358 — The installer ISO is built from the iso-builder image

## Status

Accepted, 2026-10-26, for #649. The owner chose it ("(A) Build ISO from iso-builder") over keeping the isotools package and widening cbs's stage roots. Supersedes the part of [ADR-0064](0064-rest-driven-iso-assembly.md) that made `isotools` a hostbuild aggregator and the root `mkinstalleriso` reads its tools from.

## Context

ADR-0064 made `isotools` a hostbuild that builds nothing: it copied the installed `grub`, `sbsigntools`, `xorriso`, `mtools`, `mokutil`, `shim` and `dosfstools` files, and their libraries, out of its composed build environment into one artifact. `POST /v1/system/iso` passed `<artifacts>/isotools` to `mkinstalleriso` as its isotools root, and `mkinstalleriso` read `bin/grub-mkrescue`, `sbin/mkfs.fat`, `lib/grub/x86_64-efi`, `share/grub` and `shim/` beneath it.

That recipe cannot build any more. Measured on 192.168.15.95 on 2026-10-26, under cbs v0.1.107, `pkg hostbuild isotools` failed at its first install operation:

```
/build/recipe.cbs:124:9: error[CPDL-E4004]: runtime: filesystem operation failed: `/usr/bin/grub-mkrescue`; errno=1 (Operation not permitted)
  124 |         copy "/usr/bin/grub-mkrescue" to "${dest}/bin/grub-mkrescue"
```

cbs confines `copy` to the build's own roots by design. The contract route out of the build image is `stage file` and `stage tree` (CPDL 1.0 section 4), which search only the approved command roots (`/usr/bin:/bin` by default) and library roots (`/usr/lib:/lib:/usr/lib64:/lib64`). isotools also needs `/usr/sbin/mkfs.fat` and `/usr/share/grub`, which are under neither. The LAN cache has no isotools artifact (`HEAD /isotools-2.14-18-x86_64.cixpkg` answered 404), and .95 has no isotools hostbuild entry.

Meanwhile the `iso-builder` image already holds every one of those packages. Its recipe (1.4.0, `recipe: follow`) calls itself "the ISO toolchain's one home" (cix#571). On .95 its file lists put each tool at its package path: `usr/bin/{grub-mkrescue,sbsign,xorriso,mokutil,mcopy,mformat}`, `usr/sbin/mkfs.fat`, `usr/lib/grub/x86_64-efi`, `usr/share/grub`, `usr/lib/shim/{shimx64,mmx64}.efi.signed`. So isotools was a second copy of an image that exists to be that toolchain.

## Decision

1. **`POST /v1/system/iso` gives `mkinstalleriso` the iso-builder image's rootfs.** cixd resolves the image's current version and its rootfs, as the assembly resolves `cix-hosttools` for mkbootroot (ADR-0078). An image that is missing, or has no current version, fails the POST with a 400 naming the image.
2. **`mkinstalleriso` reads every tool at its package path** beneath the root it is given: the paths above, and libraries in `CIX_LIB_DIRS_SEARCH`. The argument keeps its name, `isotools-root`, and now means "a root with the ISO toolchain's packages installed". On a host that is the iso-builder image. In a composed build environment it is `/`, which is what `test_installer` passes, as it already passes `/` for mkbootroot's host-tools root.
3. **The tools run as they did.** They are exec'd off the control-plane root with `PATH` set to the root's `usr/bin` and `LD_LIBRARY_PATH` to its library directories (ADR-0154), not in a chroot or a container. The artifact was used the same way, and carried the same image's libraries.
4. **The isotools package is retired.** Nothing reads its artifact. Removing its recipes from cix-recipes is a commit to `main`, which is a publish, so it is done separately.

## Alternatives

- **Keep isotools on `stage file` and `stage tree`.** That needs cbs to reach `/usr/share` (a data root, or resolution by the `from "PACKAGE"` the operation already records), and cixd to pass `--command-path` with `/usr/sbin`. It keeps a package whose content is a copy of other packages' files. The owner did not choose it.
- **Build an isotools-shaped fixture in `test_installer` from the composed environment.** The test would pass while no host could make the artifact the product reads. That is a second route for the same bytes.
- **Run the tools inside the image, chrooted or as a container.** That changes how every tool is run, for a property the artifact never had.

## Consequences

- An ISO is built from whatever versions the iso-builder image holds when the build starts. Under `recipe: follow` those roll. The ISO build logs the image version it used, which is what the artifact's version used to say.
- A version of the image is read for the length of a build, while retention (ADR-0343) keeps the current version and the newest three. Collecting it mid-build would take four new versions of the image during that build. This is read from the code, and it is the same exposure mkbootroot has with `cix-hosttools`.
- A host builds an ISO only once its iso-builder image exists. On a fresh host that is ordinary package installs, as it was for isotools, without the hostbuild.
- `test_installer` composes the ISO packages as declared tools, as the probes that build an ISO do. It needs no input directory beside the kernel.
