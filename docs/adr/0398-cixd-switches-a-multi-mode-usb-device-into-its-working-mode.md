# 0398 — cixd switches a multi-mode USB device into its working mode

## Status

Accepted, 2026-11-27, by the owner ("yes"). The owner asked for a mode switcher that is *"original ... not hacky at all, and should work seamlessly"*, so that *"any device plugged in, can switch seamlessly"*, naming Wi-Fi and 4G/5G dongles. Not built yet.

## Context

Many USB Wi-Fi and cellular dongles first appear as a fake disk or CD holding a Windows driver. They become the real device only after the host sends them a vendor-defined message, which on other distributions is `usb_modeswitch` run by a udev rule.

Measured on a Cix host, 2026-11-27:

- The dongle on `usb:0bda:1a2b@0000:04:00.4-usb2-1` describes itself as "Realtek DISK". Its record's `driver` is `usb`, the generic device driver; no interface driver is bound.
- The kernel log for that port (`usb 3-1`, device 2) reads `usb-storage 3-1:1.0: USB Mass Storage device detected` and then `usb-storage 3-1:1.0: device ignored`. Kernel 7.3.5 carries an ignore rule for this id, so nothing in the kernel switches it, and the fake disk never appears either. Which ids the kernel ignores, and whether it switches any itself, is to be read from `drivers/usb/storage/unusual_devs.h` in a source-read probe on the box before release A. The rule's reason upstream, that the kernel leaves the switch to userspace, is recalled rather than read.
- A second Realtek dongle at `usb:0bda:c811@0000:04:00.3-usb2-1.1` is already in its Wi-Fi mode, with `rtw88_8821cu` bound and the factory serial `123456`.
- That host's device mappings name its radios by permanent MAC, not by USB serial. So two dongles sharing `123456` are not ambiguous to any mapping in use today.
- The kernel configuration carries no modem driver: no `CONFIG_USB_SERIAL_OPTION`, `CONFIG_USB_NET_QMI_WWAN`, `CONFIG_USB_NET_CDC_MBIM` or `CONFIG_USB_NET_CDC_NCM` in `image/kernel/*.config`. `CONFIG_USB_STORAGE` and `CONFIG_USB_UAS` are `=y`.

Cix already reacts to a USB add. `handle_uevent_event()` queues the device's driver through `kmod_hotplug()` (ADR-0348), asks for missing firmware through `fwscan_want()` (ADR-0381), and reconciles waiting grants (ADR-0161, ADR-0345). A device in its working mode is therefore handled end to end. A device that is still in its storage mode is not.

## Decision

1. **The switch is cixd's, in one module, `modeswitch.c`.** It is original C over usbfs (`/dev/bus/usb/BBB/DDD`): claim an interface, send bulk or control transfers, release. It uses no libusb, starts no second daemon and adds no udev. A switch runs only in a helper child, never on the event loop, with a timeout on every transfer (ADR-0340's rule for blocking work).

2. **What to send is data, never code.** The rules come from upstream's `usb-modeswitch-data`, built by CBS as a package in `cix-root` (CPDL, sha256-pinned). cixd parses that format; it does not ship upstream's binary. That data is what makes "any device" true: one rule per known device, each naming the target ids it becomes.
   - **Before release A:** read the data on the box (its licence, how its source authenticates, and a count of rules per method), and size the engine from that count.
   - **Release A implements the generic methods:** a message sequence (`MessageContent`, `MessageContent2`, `MessageContent3`, with response reads), the standard SCSI eject (`StandardEject`) and configuration selection.
   - **Vendor-specific modes** (Huawei, Sierra, Option, Sequans and the like) are added in later releases, each one measured on a real device, in order of how many rules use it.

3. **One pure verdict decides, `modeswitch_verdict()`.** It switches only when all of these hold:
   - a rule matches the device's exact vendor and product, and the device's interface class is the one the rule expects before the switch;
   - the device is not held or reserved (ADR-0344);
   - no block device sits beneath it;
   - no disk role or adopted volume names it (ADR-0390).

   **Nothing is ever switched by class or heuristic.** An operator's data disk is often USB mass storage. A "looks like a CD-ROM" rule is exactly how a switcher would send an eject to it.

4. **When it runs.** A whole-device USB add, and a sweep at daemon start, feed one queue. One helper drains it, one device at a time.
   - **The helper sends and returns**, and never waits for the device to come back (#689). The device leaves and re-enumerates with its target id, and that add uevent completes the switch.
   - **A switch that never completes** is marked failed by a daemon timer.
   - **The loop guard is per port path:** if the pre-switch id returns after a switch, the device is marked `failed` with that reason, said once, and not switched again until it is unplugged.

5. **After the switch, nothing new runs.** The target id's add already calls `kmod_hotplug()` and `fwscan_want()`. So the driver loads under the kmod policy, the firmware is acquired, and waiting grants attach, exactly as for a device plugged in already switched. The record id changes, because the product id is part of it (ADR-0345). A mapping or grant that should follow the device across the switch names it by identity (a permanent MAC, or a scoped serial), never by the pre-switch record.

6. **API, through the contract.**
   - **On the device record:** `modeswitch` holds `{state, rule, targets, reason}`. `state` is one of `pending`, `switching`, `switched` or `failed`, and the field is `null` when no rule applies. (Refined before release A; see the amendment below.)
   - **A host setting** `modeswitch: auto | off`, read and written through `/v1/system/modeswitch`.
   - **An explicit switch,** `POST /v1/devices/{id}/modeswitch`, which answers 202. It is refused with 409 by the same verdict.

   All four contract annotations apply (`x-cix-permission`, `x-cix-read`, `x-cix-invalidates`, `x-cix-mcp`). State changes go through an announcing setter (ADR-0339). The dashboard and cixctl show the record's field. The switch runs in init mode only, and test data directories start with the setting saved `off`, so a test daemon never switches the build host's devices.

7. **Across a handover (ADR-0375)**, the in-flight switch and the per-port guard are optional fields of the handover record.

### Amended before release A was built (2026-11-27)

Measured while writing release A, and decided on that evidence:

- **States.** `state` is `switchable`, `switching`, `switched`, `failed`, `refused` or `already`. "pending" read as "queued" while automatic switching is off, so a device a rule allows reads `switchable`. A verdict refusal is `refused` with its reason, and a device whose interface 0 already has the rule's `TargetClass` is `already`. The record also names the rule's own description of the device (`device`).
- **Decision 7, for release A.** A handover waits while a switch is in flight (`handover_quiescent()` asks `modeswitch_in_flight()`), so no switch crosses one. What remains afterwards is history: a switched device reads as its target id, and a failed one as switchable again. The log keeps both. The per-port loop guard belongs to automatic switching and arrives with it, in release B, as an optional handover field.
- **Matching.** Upstream's dispatcher (`usb_modeswitch_dispatcher.tcl`, `MatchDevice`, read from usb-modeswitch 2.6.3 by `probe-modeswitch@3-4`) matches a name's `uMa=`/`uPr=`/`uSe=` value as a substring of the device's sysfs string, also with `_` read as a space. cixd matches the same way. A matching qualified rule beats a plain one; two matching qualified rules are refused; a rule needing a SCSI attribute (`sVe=`, `sMo=`, `sRe=`) is not decided in release A, and a plain rule beside it is not used in its place.
- **StandardEject** is built from the SCSI commands REFERENCE names, for every LUN the device reports through bulk-only GET MAX LUN (upstream sends LUN 0 and 1 unconditionally). After a command wrapper, its data stage (when the wrapper declares one) and its 13-byte status are read and discarded, because a device that switches may never send them.
- **Coverage.** Of usb-modeswitch-data-20261111's 515 rules, 224 use a method release A does not carry out (a vendor mode, `Configuration`, `AltSetting`, `ResetUSB` or `DetachStorageOnly`), counted by `probe-modeswitch@3-3`. Such a rule reads `refused`, naming the key. `Interface` is carried out.
- **The data package** is `usb-modeswitch-data@20261111`, pinned by the sha256 a Cix host measured, since upstream publishes only an md5 and no signature. CBS extracts its `.tar.bz2` because the recipe declares `bzip2`: libarchive, built without the bzip2 library, runs the external program.

## Consequences

- **A dongle in storage mode works with no operator action**: plug it in, and it switches, gets its driver and firmware, and appears as a radio or modem device.
- **A cellular modem switches, but may still have no driver.** The kernel carries no modem drivers today, so making one work is a priced kernel change and its own decision. Seamless switching does not imply a working modem.
- **Releases:**
  - **A** ships the engine, the parser, the verdict and the explicit POST, with automatic switching `off`. It is proven on `0bda:1a2b`.
  - **B** turns automatic switching on, at plug and at start.
- **Gates:**
  - the parser, the verdict and the message builder are pure functions with tests in `SELFTESTS`;
  - the one usbfs call site is counted by `test_blocking_waits`;
  - the transfer wrappers and descriptors are byte arrays with explicit offsets, never packed structs (TCC ignores `packed`), and the usbfs ioctl structs are checked with `_Static_assert`.

## Alternatives considered

- **Shipping `usb_modeswitch` and a udev-style rule runner.** That is a second hotplug system beside cixd's, plus a libusb dependency, and an external program executed per plug. Rejected: one listener, one queue, one verdict.
- **Switching by heuristic** (any device with a CD-ROM-class interface). Rejected: it cannot tell a dongle from an operator's disk, and an eject sent to the wrong device is unrecoverable.
- **Hand-written rules in cixd.** Rejected: a list in code is a second, smaller copy of upstream's database that falls behind it.
