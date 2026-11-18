# 0381 — A device gets its firmware after install, from packages, by itself

## Status

Accepted by the owner on 2026-11-18, for #685, through four answers:
- *"do we need to have the amd firmware on the iso? can it not be downloaded after the initial install?"*;
- **acquisition:** *"Automatic only"*;
- **rtw88:** *"Move it"*;
- **provenance:** *"Approve"*, on `rtw88-firmware`'s terms.

It supersedes [ADR-0029](0029-gpu-kernel-driver-firmware-and-kfd.md)'s firmware half. That half staged a hand-fetched `amdgpu/` tree into the root and was retired with `mkbootroot`'s firmware argument by [ADR-0371](0371-the-control-plane-root-is-one-image.md).

## Context

A driver asks for firmware with `request_firmware()` when it probes a device. Since ADR-0371 the only firmware the host has is what `cix-root`'s recipe names, today `rtw88-firmware` (ar-1's radio) and `wireless-regdb`. AMD's `amdgpu/` set is in no package (#685).

`cix-root` is one recipe shared by every host. Putting firmware there puts every host's firmware in every root, in both slots and on the installer ISO. A firmware update would also cost a root switch (ADR-0379). The owner rejected that: firmware is fetched after install.

**Measured on 192.168.15.95, 2026-11-18,** by `probe-fwpath@1-1` reading kernel 7.3.5's own source (the booted kernel; `kernel@7.3.5-1.cbs` declares that tarball and sha256):

- **The loader's search list starts with one configurable directory** (`drivers/base/firmware_loader/main.c:471-485`). It is `fw_path_para[256]` before `/lib/firmware/updates/<release>`, `/lib/firmware/updates`, `/lib/firmware/<release>` and `/lib/firmware`, set by `module_param_string(path, ..., 0644)`. So `/sys/module/firmware_class/parameters/path` is writable at runtime and takes up to 255 bytes.
- **It is resolved in pid 1's filesystem view.** The loader calls `kernel_read_file_from_path_initns()` (`main.c:549`), which opens the path under `scoped_with_init_fs()` (`fs/kernel_read_file.c:147-166`). On a Cix host pid 1 is cixd. So the path names cixd's own mounts and moves with them across an in-place root switch (ADR-0375).
- **This kernel has no firmware user helper** (`FW_LOADER=y`, `FW_LOADER_USER_HELPER` unset). The file must be on disk when the driver probes. A probe that failed for a missing file is not retried by the kernel; it is retried by binding the device again.

Also from cixd's code: the data disks are mounted (`diskformat_remount_present_role_disks()`) before the boot's module sweep (`kmodscan_boot_sweep`). So an image's tree exists when drivers first probe.

**Which package provides a file is already answered by CBS, with no new syntax.** `cbs explain` reports each output's `files` selectors in typed JSON, `{"pattern": ..., "glob": ...}` (CBS ADR-0038, `test_cbsrecipe.c`). Upstream firmware is one signed tarball for every driver: `linux-firmware-20261019.tar.xz`, 642 MB, with a `.tar.sign`, on kernel.org (listed 2026-11-18). So one `linux-firmware` recipe with an output per driver family is the index: each output claims its files, for example `glob "lib/firmware/amdgpu/**"`. A metadata key would be the workaround ADR-0322 refused; outputs are the contract.

## Decision

**Firmware is not root content. A host keeps its firmware in its own image, `firmware`. cixd points the kernel's firmware path at that image's current tree, installs into it by itself the output that provides a file a loaded driver declares and the host lacks, and probes the device again.**

1. **The `firmware` image.**
   - cixd creates it at start when it is absent, as it creates `base`. Its manifest is per host; nothing but cixd's acquisition, an explicit install or an uninstall changes it. Its entries roll.
   - **This is not a root feeder** (ADR-0371 retired `cix-firmware`). Nothing assembles it into a root, and a root never contains it. The kernel reads it in place, as containers read their image versions.
2. **The path.**
   - The image's current version is reached through one stable path, a symlink at `REBUILDABLE_DIR/firmware` naming the version's `lib/firmware`. A version change replaces the link atomically (rename over it); the kernel path never changes.
   - At every start, before the boot's module sweep, cixd writes that path to `/sys/module/firmware_class/parameters/path`. The write is idempotent, so the step runs at every start, a handover included. It is not once-per-kernel: storage placement can move `REBUILDABLE_DIR`, and the same write follows it.
   - One setter does both, `firmware_path_apply()`, and nothing else writes the parameter. A path over 255 bytes is refused and said, never truncated.
3. **What a driver declares.**
   - A module declares the files it may request (`MODULE_FIRMWARE`, the `.modinfo` section's `firmware=` entries).
   - When ADR-0348's queue loads a module for a device, at the boot sweep, a plug or an explicit request, its helper child also reads those names, then checks each against the firmware image's current tree and the root's `/lib/firmware`.
   - The names, and those missing, go on the device record (ADR-0344). The dashboard and cixctl show the same answer: `firmware: missing N of M (amdgpu/navi10_sos.bin, ...)`.
4. **Acquisition, by itself.**
   - For the missing names, cixd asks the output index: the `files` selectors `cbs explain` reported for every recipe it holds, matched under `lib/firmware/` with CBS's own rules (`*` within a component, `**` across components, an exact selector also claiming the tree beneath it).
   - Each output that matches at least one missing name is installed into `firmware`, as an ordinary package job (git-first recipes, cache, ABI hold and approvals all apply). One output per driver family keeps this to one install for a device.
   - **An index miss is said once and never retried by itself.** `firmware: amdgpu/xyz.bin is in no package` names the file and the module. It retries when a recipe sync changes the index.
   - **Only what the device's driver declares is fetched,** and only for modules ADR-0348's policy let load, so the blacklist holds firmware off too.
5. **Probe again.** When a `firmware` version that added a device's missing names is recorded, cixd binds that device to its driver again, in the module queue's helper child. A bind waits on the driver's probe, which can take seconds. The outcome goes on the device record and the log, once.
6. **One home.**
   - `rtw88-firmware` and every other firmware package become outputs of `linux-firmware`, and leave `cix-root`'s recipe. The old `rtw88-firmware` recipe is retired, since one name has one source (ADR-0324).
   - `wireless-regdb` stays as it is: `regulatory.db` is built into the kernel image (`CONFIG_EXTRA_FIRMWARE`), so it is not runtime firmware.
7. **Provenance.**
   - The blobs run on the device, never on the host CPU. They come only from kernel.org's signed `linux-firmware` release tarball, verified by its signature (ADR-0323's first rung), and are pinned by content.
   - Each output ships upstream's licence files for what it carries.
   - The recipe's header records the exception in `rtw88-firmware`'s words, and the owner approved it on 2026-11-18. The artifacts are published to the cache like any package.

## Consequences

- **The ISO and the root carry no firmware.** A host that never sees an AMD GPU never fetches `amdgpu/`.
- **A device whose firmware is missing comes up after its first boot's install finishes, not at that boot.** It is probed again once the firmware lands. At later boots the image already holds it.
- **The first `linux-firmware` build fetches 642 MB once per upstream release,** on whichever host builds it; every other host takes the cached outputs.
- **Limit:** a host whose uplink NIC needs firmware cannot fetch it, because fetching needs the uplink. No host in use has such a NIC. Shipping one would need the installer to carry that output, which this ADR does not do.
- **Limit:** a driver that requests a file it never declared with `MODULE_FIRMWARE` is not covered. The device record's `missing` list is the declared one; the kernel log names any other.

## Releases

1. **The index and the record.**
   - cixd reads output selectors, reads a loaded module's declared firmware, puts `firmware` on device records, creates the `firmware` image, and applies the path.
   - It installs nothing yet; the record shows what would be fetched.
2. **The recipe.**
   - `linux-firmware` with `amdgpu-firmware` and `rtw88-firmware` outputs (git first), and the old `rtw88-firmware` recipe retired.
   - Built once on .95, a fetch and a copy. Priced before it runs, since the source is 642 MB.
3. **Acquisition and re-probe.**
   - Live proof on .95 is ar-1's radio: `rtw88-firmware` arrives in `firmware`, then `cix-root`'s recipe drops it.
   - In that window the root still holds a copy, and the path searched first is the image's, so the radio never loses its firmware.

## Live checks

- **After release 1:**
  - `/sys/module/firmware_class/parameters/path` reads the stable path, read through a console;
  - ar-1's radio's device record lists `rtw88/rtw8822b_fw.bin`, found in the root.
- **After release 3:**
  - `rtw88-firmware` is installed in `firmware` by cixd, not by hand;
  - after `cix-root` drops it, ar-1 restarts and its access point comes up;
  - an AMD host, when one exists, gets `amdgpu-firmware` with no operator step.
