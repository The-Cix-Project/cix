# 0371 — The control-plane root is one image

## Status

Accepted by the owner on 2026-11-03, for #350, with both decisions below taken: wait for ADR-0367 outputs, and `cix` is followed like any entry. Not built yet; the phases are in order. This is [ADR-0291](0291-the-control-plane-root-is-an-image.md)'s proposal restated against the root as it is assembled today. ADR-0291 described a root that no longer exists: build-host tools copied by `shelled_bins[]`, a library table, an elfcheck tension. [ADR-0337](0337-the-root-is-derived-from-one-list-with-no-build-host-fallback.md) and [ADR-0303](0303-a-hostbuild-carries-pkg-depends-it-does-not-resolve-it.md) removed all three. ADR-0291 stays as written, because an ADR is append-only, and is Superseded by this one.

## Context

### What the root is made from today

Read from `spawn_cix_bootroot_assembly()` (`daemon/src/main.c:10505`) and `mkbootroot`'s `main()` (`image/src/mkbootroot.c:998`) on 2026-11-02. Everything comes from a Cix host. Nothing comes from a build host except one read, noted below. But the content is defined in six places:

| Part of the root | Comes from | Defined by |
|---|---|---|
| `/bin/cixd`, `/bin/cix-init`, `/web` | the `cix` hostbuild's harvest, `ARTIFACTS_DIR/cix` | the `cix` recipe's `install` block |
| `/bin/cixctl` | the `cixctl` output of the same build, out of the local cache (ADR-0367) | the `cix` recipe's `output "cixctl"` |
| 11 programs (`include/controlplane_programs.h`), and **every** `.so` in six library directories, the C.UTF-8 locale, `openssl.cnf` and the CA bundle | the `cix-hosttools` image | `cix-hosttools@2.4.6` and `controlplane_programs.h` |
| `/lib/firmware` | the `cix-firmware` image | `cix-firmware@1.0.2` |
| `/lib/modules` | the `kernel` hostbuild's harvest, `ARTIFACTS_DIR/kernel` | the `kernel` recipe |
| `modprobe` and the other kmod tools | the `cix-kmod` image's `usr/bin`, copied flat | `cix-kmod@1.0.1` |

`mkbootroot` adds the parts that are not content: the mount points, an empty `/etc/resolv.conf` and a generated `/etc/os-release`. It then seals the tree through `verify_platform_libs_intact()`, `verify_controlplane_programs()`, `verify_root_closure()` and `require_dlopened_libs()`, and squashes it.

### What is still wrong with that

1. **Four inputs are positional arguments that a caller may leave empty.** These are `argv[6..9]` (`main.c:10733-10736`). That is how ADR-0029's firmware staging had no caller and how #347 left every host without modules. Both are fixed. The slots that made them possible are still there.
2. **The C library still starts as the build host's.** `test_image_fixture_build()` copies `libc.so.6` and the loader from `/lib/x86_64-linux-gnu` of the machine running `mkbootroot` (`mkbootroot.c:1067`). On an installed host, that machine is the running root. The `cix-hosttools` sweep then overwrites them (`:1281-1360`), and `verify_platform_libs_intact()` (`:1671`) checks that it did. The root has one glibc because of the copy order, and a check guards that order. Nothing in the structure guarantees it.
3. **No API answers "what is in the control plane".** `GET /v1/system/assembly` reports a path and the kernel releases. The answer is spread across three image manifests, two hostbuild records and a C header.
4. **Three images exist only to feed this.** `cix-hosttools`, `cix-firmware` and `cix-kmod` have no consumer other than `spawn_cix_bootroot_assembly()`. Each is a partial description of the root, and none of them is the root.

### What a whole-package root would carry (measured)

An image's content is whole packages and nothing else ([ADR-0333](0333-an-images-content-comes-only-from-packages.md)). Today's root is a filter: a list of programs, plus every library in an image. So the question is what the whole packages would add. File lists were read from `GET /v1/pkg/NAME@cix-hosttools` on 192.168.15.95 on 2026-11-02. Runtime requirements were read from the newest recipe of each package in cix-recipes the same day.

- **The tool packages are already narrow.** squashfs-tools, util-linux, e2fsprogs, btrfs-progs and dosfstools ship 13 programs, and the root stages 9 of them. The extra four are `sqfscat`, `sqfstar`, `fatlabel` and `mkfs.fat`. kmod ships the 7 tools the root already stages, and cbs ships 1.
- **No runtime closure reaches a shell.** Their declared closures, and cixd's, reach xz, libarchive, zstd, zlib, ncurses, libblkid, libuuid, pkgconf, openssl and curl. None of them reaches bash, perl or coreutils, so the root keeps the property of having no shell.
- **The library packages are not narrow.**
  - curl ships 12 headers and `usr/bin/curl`.
  - openssl ships 135 headers and `usr/bin/openssl`.
  - ncurses ships 11 programs (`tic`, `reset`, `tput`, ...), 11 headers and 3,836 files under `usr/share`.
  - kernel ships `bzImage` beside its `lib/modules`.
  - A whole-package root would carry all of these. Splitting them is what [ADR-0367](0367-a-recipes-outputs-are-packages.md)'s outputs are for, and `cixctl` is the first output to exist.
- **The `cix` package is laid out for a harvest, not for a root.** It installs `cixd` and `cix-init` at the top level, beside five build-side tools: `mkbootroot`, `cix-install`, `cix-recover`, `mkinstalleriso` and `cix-xorriso`. It also carries `cix-boot.efi`, and none of these six belong in a root. Its `cixctl` output installs `usr/bin/cixctl`, while the root and `cixd --init-mode` use `/bin/cixctl` (`controlplane_programs.h:52`, `main.c:37590`).
- **A kernel installs into an ordinary image already.** `pkg ls` shows `kernel 7.2.3-18` in `base`, so installing one into an image is not new ground.

## Proposal

**The root is the current version of one image, `cix-root`, plus the files ADR-0333 says are not image content.** The name `cix-boot` is taken by the UEFI boot manager ([ADR-0362](0362-cix-boot-is-built-by-a-tcc-win64-cross-compiler.md)).

1. **One recipe defines the root's content.** `cix-root`'s image recipe in cix-recipes declares:
   - a `cixd` output of `cix` (`bin/cixd`, `bin/cix-init`, `web/`);
   - the `cixctl` output, at whichever single path the root and the console agree on;
   - a modules output of `kernel`;
   - `kmod`, the firmware packages, the seven tool packages and `ca-certificates`;
   - `glibc`, explicitly, since no runtime closure names it.
   
   The image's runtime closure brings the libraries. "What is in the control plane" becomes `GET /v1/images/cix-root`.
2. **`mkbootroot` takes one tree.** It takes the image version's rootfs, an output path and a staging directory. It adds the mount points, `/etc/resolv.conf` and `/etc/os-release`, then seals and squashes. These are deleted: `argv[3..9]`, the library sweep, the locale and CA staging, the fixture's C-library read and `verify_platform_libs_intact()`. One package owns a path, so the root has one glibc by construction.
3. **The seal stays.** `verify_controlplane_programs()`, `verify_root_closure()` and `require_dlopened_libs()` check the result, because #589 is why they exist. `controlplane_programs.h` stays as the daemon's declared executable paths. It is a gate over the image and no longer a source of content, so the root is still defined once.
4. **The assembly reports its source.** `GET /v1/system/assembly` names the `cix-root` version it squashed. Kernel release pairing (cix#568) is checked against the modules in that version.
5. **`cix-hosttools`, `cix-firmware` and `cix-kmod` retire.** Nothing else reads them (`daemon/src/main.c`, 2026-11-02).

## Decisions for the owner

Both taken on 2026-11-03: **A is "wait for outputs"** and **B is "followed"**. So `cix` rolls in `cix-root`, and an assembly runs when `cix-root` gains a version, not when a cix hostbuild finishes. The options are kept below as they were put. **A was refined the same day**, once its cost was measured: 25 recipes declare `tool "openssl"`, 10 `tool "curl"` and 12 `tool "ncurses"` to get headers into their builds, so moving headers into an output would break them. The owner chose to split the libraries as well and to add a composition rule so that a `tool` brings its package's outputs with it. That rule is [ADR-0372](0372-a-build-tool-brings-its-whole-build.md). The other choices offered were splitting the kernel only, or also revising the 47 recipes.

- **A. Wait for outputs, or accept the growth.** With ADR-0367 outputs for curl, openssl, ncurses and kernel, the root stays the size it is today: 13,275,136 bytes as squashed for 0.2.57-654. Without them, it also carries the headers, the terminfo database, a second bzImage and four extra command-line tools. cixd installs a named output into an image already (0.2.57-635; `cixctl` is installed that way in `agent`), so waiting costs recipe work, not daemon work.
- **B. How `cix-root` gets the cix that was just built.**
  - **Pinned at hostbuild:** the assembly first installs `cix@X` into `cix-root`. That moves the pin (ADR-0320), so the recipe has to leave cix unpinned or follow the install. The timing is today's: a hostbuild produces a root.
  - **Followed:** `cix` rolls in `cix-root` like any other entry, and an assembly runs when the image gains a version. That covers the host roll (ADR-0327) too, but it moves the trigger from "a cix build finished" to "the image changed".

### Decisions of 2026-11-10, after phase 2

Phase 2 created `cix-root@1.0.0` on 192.168.15.95 and compared it with the root inside `cix-installer-0.2.57-669` (ROADMAP Part 362, #350). Every file in both trees was byte-identical. The root alone held mount points, `os-release`, `resolv.conf`, `/bin/cixctl` and three libraries from its sweep. The image alone held 120 paths. The owner was asked three questions on that evidence and answered all three on 2026-11-10:

- **C. `cixctl` lives at `/usr/bin/cixctl`,** the path its output installs. `cixd --init-mode`, the console and `controlplane_programs.h` move to it. No package changes, and agent-1 already has it there.
- **D. glibc gets a `-dev` output,** as openssl, curl and ncurses did. That output holds the `crt*.o` objects, the static archives, `/usr/include` and the build-time tools. Build images take it through ADR-0372's rule. The root carries runtime glibc only. This is one glibc rebuild, priced and asked before it runs, as CLAUDE.md requires for glibc.
- **E. The extra command-line tools are split out of the library packages too.** That covers the curl, openssl and pkgconf CLIs, xz's programs and helper scripts, and ncurses' programs and terminfo. Each library package's main output keeps only its runtime libraries, so the root carries no tool it does not name. This **reverses** the 2026-11-03 rule above that "CLIs and terminfo stay in the main package". An image that uses one of those tools at runtime, such as `agent` with the curl binary or an interactive image with terminfo, declares the tool output in its recipe. A recipe that composes the tool at build time gets it through ADR-0372. Each split is one ordinary rebuild of its package. **The tool packages are split as well** (the owner, the same day, asked after phase 2 showed the root carrying `sqfscat`, `sqfstar`, `mkfs.fat` and `fatlabel`): squashfs-tools and dosfstools each get an output for the programs `controlplane_programs.h` does not name, so the root carries exactly the programs it runs. `mkfs.fat` is not one of them.

## Alternatives

- **Keep the filter and report it.** `GET /v1/system/assembly` would list each input's image version and package set, and the four slots would become required. That is one release. It answers the operator's question, but it keeps six definitions and the copy-order glibc.
- **The whole `cix-hosttools` tree as the root.** Rejected: it carries bash, perl and coreutils, and the root has no shell.

## Consequences and cost

Priced in builds and releases, in order. Each phase is verified on 192.168.15.95: chroot-run the assembled root before any boot, then boot it.

1. **Outputs:**
   - a `cixd` output in the `cix` recipe (a routine hostbuild);
   - a modules output in `kernel`, with `bzImage` staying in the main package so the hostbuild harvest and cix#568's pairing are untouched (**one kernel rebuild, run alone**);
   - `-dev` outputs for curl, openssl and ncurses (three ordinary rebuilds).
   
   Only build-time parts move: `usr/include`, pkg-config files and `*-config` scripts. CLIs and terminfo stay in the main package, because images use them at runtime (`agent` carries curl for the binary, and interactive images need terminfo), so the root carries them. The bytes that adds are not measured. ADR-0372's rule must ship in a cixd release **before** any split reaches cix-recipes, or the 47 builds that compose these tools lose their headers.
2. **`cix-root` image recipe**, created on .95. A probe compares its tree with the current assembled root, path by path and hash by hash. No cixd code changes.
3. **One release:** `mkbootroot` takes one tree, `spawn_cix_bootroot_assembly()` passes the image version, and the deletions above are made. The tests that build a root pass a fixture tree.
4. **Retire the three images** on .95, and update the documents that name them.

## Phase 3a, as built (0.2.57-681)

Step 3 above is two releases, because the daemon that assembles release K is release K-1:

- **K (phase 3a).** `mkbootroot <tree> <out.squashfs> <staging>` seals one tree. It copies it, adds the mount points, an empty `/etc/resolv.conf` and `/etc/os-release`, and keeps the seals: `verify_controlplane_files()` (new: `cixd`, `cix-init`, the dashboard, `openssl.cnf`, the CA bundle, the C.UTF-8 locale), `verify_controlplane_programs()`, `require_dlopened_libs()` and `verify_root_closure()`. The tree's own `mksquashfs` seals it. `spawn_cix_bootroot_assembly()` passes `cix-root`'s current version. The staging that read `cix-hosttools`, `cix-firmware`, `cix-kmod` and the two hostbuilds' outputs is gone from the daemon. K's `mkbootroot` still takes K-1's ten arguments: it builds a tree from the host-tools root with `test_root_tree_build()`, the old staging moved whole into `test/test_root_tree.c`, and seals it the new way. So **K boots on a legacy-shape root carrying K's cixd; the first one-image root is K+1's.** The boot tests build their fixture tree with that function and call the three-argument form, as production does. `test_console_pki_bootstrap` keeps the ten-argument form, so the path K-1's daemon takes is booted under QEMU before a roll depends on it.
- **K+1 (phase 3b).** The ten-argument form, `mkbootroot`'s link to `test/test_root_tree.c` and `test_mkbootroot_firmware` go.

**How `cix-root` gets the build is a bridge, not decision B.** The owner chose to have `cix` followed like any entry. In K the daemon instead installs the hostbuild's `cixd` and `cixctl`, and the kernel hostbuild's `kernel-modules`, into `cix-root` as an explicit install. That moves their pins (ADR-0320); `cix-root`'s policy is `manual`/`declare`, so the moved pins do not fight git. An assembly is asked for, not started: `bootroot_want()` records it, and `bootroot_drive()` runs at the end of every package job and assembly. It installs the first build `cix-root` lacks, or, once the image holds every build at its owner's installed hostbuild version and no job is in flight for it (ADR-0374's settled rule), starts `mkbootroot`. `root_image_behind()` is the guard. A root sealed from a `cix-root` still holding the previous `cixd` would boot, be found not to be running the installed `cix`, and roll again every night. `GET /v1/system/assembly` reports `waiting_for` and `root_image_version`. Following needs the rolling drain to take the hostbuild's cached outputs rather than build `cix` in a build container; when it does, the explicit install is deleted and `bootroot_drive()` only waits.
