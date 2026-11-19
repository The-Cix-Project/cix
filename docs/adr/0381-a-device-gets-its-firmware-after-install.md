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
- **This kernel has no firmware user helper** (`FW_LOADER=y`, `FW_LOADER_USER_HELPER` unset). The file must be on disk when the driver probes.

Not measured, and to be measured in release 1: that a probe which failed for a missing file is not retried by the kernel, and that binding the device again retries it. That is the kernel's documented behaviour, not something this probe read.

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
   - **The module is the device's driver and that module's dependency closure**, never only the module matched to the device's alias. The declaration often sits in a chip module the bus module depends on: rtw88's `rtw8822b_fw.bin` is declared in `rtw8822b.c` (its recipe header), while the dongle's alias names a USB module. For a bound device the driver comes from its sysfs `driver` link; for an unbound one, from the module ADR-0348's policy matched.
   - **It is read at every start and on every plug,** never only when a module loads. Releases arrive by in-place switch, where no sweep runs, so a load-time read alone would leave every record empty after a switch. The read runs in the disk snapshot's helper-child style (ADR-0377): sysfs and `.ko` reads, off the loop.
   - Each name is checked against the firmware image's current tree and the root's `/lib/firmware`. The names, and those missing, go on the device record (ADR-0344). The dashboard and cixctl show the same answer: `firmware: missing N of M (amdgpu/navi10_sos.bin, ...)`.
   - **Missing is normal for working hardware.** A module declares files for every chip it supports, so a bound, working device can list missing names. The record says so; it is not a fault.
4. **Acquisition, by itself.**
   - For the missing names, cixd asks the output index: the `files` selectors `cbs explain` reported for every recipe it holds, matched under `lib/firmware/` with CBS's own rules (`*` within a component, `**` across components, an exact selector also claiming the tree beneath it).
   - Each output that matches at least one missing name is installed into `firmware`, as an ordinary package job (git-first recipes, cache, ABI hold and approvals all apply). One output per driver family keeps this to one install for a device.
   - **A name two outputs claim is refused and said, never picked** (ADR-0324's rule for a name two sources offer).
   - Acquisition runs for bound devices too, so a later re-probe or reboot finds the files. Only the re-probe below is limited to unbound devices.
   - **An index miss is said once and never retried by itself.** `firmware: amdgpu/xyz.bin is in no package` names the file and the module. It retries when a recipe sync changes the index.
   - **Only what the device's driver declares is fetched,** and only for modules ADR-0348's policy let load, so the blacklist holds firmware off too.
5. **Probe again, only a device with no driver bound.**
   - When a `firmware` version that added an unbound device's missing names is recorded, cixd binds that device to its driver, in the module queue's helper child. A bind waits on the driver's probe, which can take seconds. The outcome goes on the device record and the log, once.
   - **A bound device is never re-probed by cixd.** Rebinding the uplink NIC or ar-1's radio for a firmware roll would take them down, and ADR-0347 would fail the container holding the radio. A bound device only reports; new firmware reaches it at its next probe (a re-plug or a reboot).
   - **Limit:** a driver that loads firmware asynchronously can stay bound with its device dead when the file was missing. cixd does not detect that; the record's `missing` list and the kernel log say it.
6. **Only on a real host.** The path write and the `firmware` image are init mode only, gated as the wireless country is (ADR-0354). The path is kernel-global, and selftest daemons run on .95 with `CAP_SYS_ADMIN`: a test daemon must never repoint the host's loader.
7. **One home.**
   - `rtw88-firmware` and every other firmware package become outputs of `linux-firmware`, and leave `cix-root`'s recipe. The old `rtw88-firmware` recipe is retired, since one name has one source (ADR-0324).
   - `wireless-regdb` stays as it is: `regulatory.db` is built into the kernel image (`CONFIG_EXTRA_FIRMWARE`), so it is not runtime firmware.
8. **Provenance.**
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

1. **The index and the record**, in two halves.
   - **1a:** cixd creates the `firmware` image, keeps the stable link following its current version (`firmware_link_set()`, at start and after every package job), and applies the kernel path before the boot's sweep (`firmware_path_apply()`). It installs nothing, so the link names an empty directory until the image holds a package.
   - **1b:** cixd reads output selectors and a loaded module's declared firmware, and puts `firmware` on device records. It installs nothing yet; the record shows what would be fetched.
2. **The recipe** (done 2026-11-19: `linux-firmware@20261019-1`, cix-recipes ca2638c7, with the `amdgpu-firmware` output only).
   - `linux-firmware` with the `amdgpu-firmware` output (git first). The `rtw88-firmware` output joins in release 3, with the old `rtw88-firmware` recipe retired in the same change. That name is one recipe's today (ADR-0324), and `cix-root` rolls it, so an earlier output would have moved ar-1's radio to new firmware at once (161,240 bytes in 20261019 against `rtw88-firmware@3`'s 150,984).
   - Upstream declares symlinked names in `WHENCE` (`Link:` lines), which its own install script creates. The recipe creates those under each family it ships, or a raw copy of the tree misses names a driver asks for.
   - Built once on .95, a fetch and a copy. Priced before it runs, since the source is 642 MB. If `linux-firmware` rolls, each upstream release is another 642 MB fetch and build; it is pinned unless the owner chooses otherwise.
3. **Acquisition** (code in 0.2.57-700), then re-probe, then the rtw88 switchover.
   - **Acquisition asks the firmware image, never the root.** A name the root still holds is acquired too, so dropping it from the root later leaves no gap. The decision is one pure function, `fwacquire_verdict()`, with a unit test, since no selftest daemon runs in init mode.
   - **Acquisition declares; the drain installs.** The output is declared into the `firmware` image's manifest, rolling, and the image queued. So the drain's own rules apply (one build per recipe, #681; held while a hostbuild runs, #693; the ABI hold), and nothing new starts installs. A declared entry is never declared again; a failed install is the drain's state, not retried from here.
   - **Trusted sources only** (the owner, 2026-11-19: *"Trusted sources only"*): an output whose recipe's source has no `trust_keys` is reported and installed only by an operator.
   - **Re-probe, measured first.** `probe-reprobe@1-1` read 7.3.5's `drivers/base`. Writing a device's name to `/sys/bus/<bus>/drivers_probe` (`drivers_probe_store()`, bus.c:309) calls `device_attach()` only when the device has no driver (bus.c:865), and that tries every registered driver on the bus (dd.c:1131). Nothing but `dev->p->dead` remembers a failed probe. So when the firmware link moves, a helper child writes each unbound target's name there and reports which bound. A target is a directory the last scan found with no driver while a module claimed its modalias, for a device whose modules declare firmware. Bound devices are never touched: the kernel skips them, and the child checks first. .95 has no such device to exercise it; the source is the evidence until one exists. The module must still be loaded for its driver to be registered, which is ADR-0348's queue's job.
   - cixd acquires `rtw88-firmware` into `firmware` for ar-1's radio, while the root still holds a copy. **The owner's approval gates publishing the `rtw88-firmware` output**, because the image's copy is searched first and the radio takes it at its next probe.
   - **Then a watched switchover, asked of the owner first,** because ar-1's access point is what it risks:
     1. `cix-root`'s recipe drops `rtw88-firmware`, and the root-only roll switches;
     2. ar-1 is stopped and the radio re-plugged or re-probed, so rtw88 reads its firmware again (it reads at probe, not at an access point start);
     3. ar-1 is started and its access point checked.
   - The rollback is restoring `rtw88-firmware@3.cbs` to cix-recipes from before `a5857299`, installing it into `cix-root`, and reloading the module with ar-1 stopped. There is no one-step one, because the recipe is retired.
   - This is also the first real proof that the path follows an in-place switch, which is an inference from two source reads until then.
   - **Done on 2026-11-19, on the owner's "Go ahead now", with the root's copy removed first,** which left no gap that mattered, since rtw88 reads its firmware only at probe and nothing probed the radio until the reload:
     - `rtw88-firmware` left `cix-root` first (root-only roll to `40aa432f88a4`, switched in place at 16:39:38 UTC), while the radio kept running the firmware it had loaded;
     - the `rtw88-firmware` recipe was retired (cix-recipes `a5857299`) and `linux-firmware@20261019-2` published with the output;
     - the publish asked for a scan (0.2.57-702), which declared the output by itself: *"usb:2357:012e@0000:07:1b.0-usb2-1's rtw88_8822b declares rtw88/rtw8822b_fw.bin, which the firmware image lacks -- declared rtw88-firmware (rolling, from linux-firmware@20261019-2) for the drain to install"*, and the drain installed it from the source cache;
     - ar-1 was stopped (the kernel logged its interface leaving at 16:45:43 UTC); `kmod unload` and `kmod load rtw88_8822bu`; the kernel logged `Firmware version 30.20.0, H2C version 14` at 16:45:55, where every earlier probe logged `27.2.0, H2C version 13`. The root carried no rtw88 blob after the 16:39:38 switch, so that one came through the stable link, which answers the inference above for an in-place switch;
     - ar-1 started; `hostapd: radio-ap: AP-ENABLED` at 16:46:32 UTC. No station had associated by 16:50, so a client on the new firmware is not yet observed.
   - **A cold boot with the root's copy gone works.** The order in `main()`: rebuildable storage is resolved, then `firmware_path_start()` writes the path, then the boot sweep probes, and the link persists on disk from the last start (`firmware_link_sync()` re-points it after the sweep). Measured by a reboot the owner asked for (*"I dont want to wait"*): the kernel started at 17:23:04 UTC, the radio logged `Firmware version 30.20.0, H2C version 14` in the same second, ar-1 logged `AP-ENABLED` at 17:23:05, and vpn-1 reached its dial tone at 17:23:11, with all 14 containers running.
   - `cix-root@1.3.0` records the live manifest (cix-recipes `da7461c4`).

## Live checks

- **After release 1:**
  - `/sys/module/firmware_class/parameters/path` reads the stable path, read through a console;
  - ar-1's radio's device record lists `rtw88/rtw8822b_fw.bin`, found in the root, through its chip module;
  - the records survive an in-place switch;
  - the binding-again retry is measured, on an unbound device or a probe.
- **After release 3:**
  - `rtw88-firmware` is installed in `firmware` by cixd, not by hand;
  - the watched switchover brings ar-1's access point back with the root's copy gone;
  - an AMD host, when one exists, gets `amdgpu-firmware` with no operator step.
