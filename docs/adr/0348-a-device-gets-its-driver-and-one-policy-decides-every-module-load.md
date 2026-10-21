# 0348 — A device gets its driver, and one policy decides every module load

## Status

Proposed, 2026-10-21, for #143. The owner decided the defaults on 2026-10-21: drivers load automatically at boot and on plug, and a blacklisted module that is already loaded is marked and warned about. The precedence rule, that the blacklist beats everything, is the owner's decision of 2026-08-27, recorded on #143.

## Context

Read from the code at `0de4d66f` unless an item says it was measured.

**A device that appears gets no driver unless someone named it in advance.** Modules load from three places today, and none of them is the hardware:
- `load_boot_modules()` loads the fixed NIC list `CIX_NIC_MODULES` at start;
- `load_configured_modules()` loads every `kmod-config` entry with `autoload: true`;
- `POST /v1/system/kmod/{name}` loads one module on request.

The uevent socket (`NETLINK_KOBJECT_UEVENT`, ADR-0161 Phase C) is already in the event loop. It reads `ACTION=` and `SUBSYSTEM=` to reconcile device grants, and ignores `MODALIAS=`. Nothing walks `/sys` for the `modalias` of hardware present at boot. Measured on .95, 2026-10-21: `rtw88_8822bu` is loaded because a `kmod-config` entry names it, not because the dongle appeared.

**There is no way to refuse a module.** A module that hangs a machine or fights another driver can only be avoided by never asking for it, and an automatic path would ask.

**`modprobe` runs on the event loop.** `kmod_load()` forks `/usr/bin/modprobe` and waits for it (`test_blocking_waits` counts it as "modprobe, bounded"). Each request holds the loop for one `modprobe` run. A hub's worth of hotplug events, or a sweep over every device at boot, would hold it for hundreds.

**The kernel has its own route.** `kernel.modprobe` reads `/sbin/modprobe` on .95 (measured, `GET /v1/system/sysctl/kernel.modprobe`, 2026-10-21). That is the path `request_module()` runs for kernel-initiated loads: a filesystem type or a protocol family asked for by name. Whether `/sbin/modprobe` exists in the control-plane root is not measured. Read from `mkbootroot`, the root creates `bin`, `usr/bin` and `usr/sbin` and no `sbin`, so those loads probably fail today, without a word.

## Decision

**Every module load, whoever asks for it, is decided by one function over one policy. Hardware that appears gets its driver by default, and a blacklisted module never loads.**

1. **One policy**, `GET`/`PUT /v1/system/kmod-policy`, persisted in the state directory beside `kmod-config`:
   - `autoload`: `off`, `coldplug`, `hotplug` or `both`. A host that has never saved a policy uses `both`, and a saved policy is never replaced by the default, as ADR-0315 rules for sources. A test data directory starts with the policy saved as `off`, so a daemon a test forks does not sweep a build container's `/sys`.
   - `blacklist`: module names that never load.
   - `allowlist`: when not empty, the only modules an automatic path may load.

   Names compare with `-` and `_` as the same character, which is `kmod_is_loaded()`'s rule. Per-module options stay in `kmod-config`, and are not repeated here.
2. **One verdict**, `kmod_policy_verdict(name, origin)`. Every load path asks it before `modprobe` runs, and the answer says why:
   - a blacklisted name is refused from every origin: an explicit `POST /v1/system/kmod/{name}` (409, naming the blacklist), a `kmod-config` autoload entry, a coldplug match and a hotplug match;
   - the allowlist applies only to the automatic origins, coldplug and hotplug. An operator naming a module in a request or in `kmod-config` has allowed it.
   - **A dependency counts.** `modprobe` loads what a module depends on. The verdict therefore covers the module's dependency set (`modprobe --show-depends`), and a blacklisted dependency refuses the load, naming that dependency.
3. **Hotplug.** A uevent with `ACTION=add` and a `MODALIAS=` is resolved to module names with `modprobe -R` and each name is decided and loaded. Read on the loop, run off it (rule 5).
4. **Coldplug.** A sweep walks `/sys/devices` to a bounded depth, collects each distinct `modalias`, and puts each one through the same path as a hotplug event. It runs once after daemon start and on `POST /v1/system/kmod-scan`. It is idempotent: a module already loaded is reported as such and not loaded again.
5. **Off the loop, one queue.** Every load except the NIC list goes through one queue served by a helper child, one `modprobe` at a time; the loop only reads the result. At start the NIC list stays synchronous, because `bootstrap_management_network()` needs it. The queue then takes the sweep and, after it, the `kmod-config` autoload entries, in that order.
   - An explicit `POST /v1/system/kmod/{name}` joins the same queue and parks, as ADR-0340's credential check does: one new kind of `struct park`, never a second park. The verdict's dependency check makes it two `modprobe` runs, which is two too many for the loop (#619 and #624 are both loop holds).
6. **Every outcome is said, once, where it belongs.** A load from an automatic origin logs one info line: the alias, the device path and the module. A refusal and a failure each log one line with the reason, the failure in modprobe's words. An alias no module claims, and a module already loaded, are not logged: a sweep meets hundreds of ACPI, platform and input aliases with no driver. `GET /v1/system/kmod-scan` returns the last sweep's report with every alias in it: its device, the module it resolved to (or none), and loaded, already loaded, unclaimed, refused (with the reason) or failed (with modprobe's words). Nothing a sweep meets is left out of the report.
   - **Announced** (ADR-0339). A load or unload changes `listKmod`, and a sweep changes the `kmod-scan` report; the daemon makes both changes itself, so each goes through a setter that announces. A `PUT` of the policy invalidates the policy read and `listKmod`, because the `blacklisted` mark lives there.
7. **A blacklisted module that is loaded stays loaded, and shows.** `PUT` never unloads anything. `GET /v1/system/kmod` marks each such module `blacklisted: true`, and cixctl and the dashboard show the mark. A `PUT` that blacklists a loaded module logs one warning naming it.
8. **The kernel's route obeys the same blacklist.** The daemon renders the blacklist into a `modprobe.d` file under `/run/modprobe.d`, rewritten at start and on every `PUT`. Any `modprobe` that some other path runs, the kernel's `request_module()` included, then refuses those names too. The file is a rendering of the policy, never edited and never read back. The verdict stays the one decision and the one source of reasons. Before this is built, whether `/sbin/modprobe` exists in the root is measured. If it does not, the rendering covers a path that is closed today and stays harmless if it opens.

## Verification

1. **In the gate.** A `test_kmodpolicy` in `SELFTESTS` checks the verdict: the blacklist from every origin, the allowlist on the automatic origins only, a blacklisted dependency, the `-`/`_` rule, and that the rendered `modprobe.d` file refuses exactly the blacklist. Nothing in a build container can load a module, so the loads themselves are not gated.
2. **Live, on .95.** With `kmod-config`'s `rtw88_8822bu` entry removed and the module unloaded, a re-plug of `ar-1`'s dongle loads `rtw88_8822bu` by hotplug, logged once. After a reboot, the coldplug report lists the dongle's alias as loaded. A blacklisted `rtw88_8822bu` is refused at plug with one line, `POST /v1/system/kmod/rtw88_8822bu` answers 409, and while the module is loaded `GET /v1/system/kmod` marks it. That needs the owner for the re-plug, as ADR-0347's verification did.

## Consequences

- Hardware works when it is plugged in, as on a general-purpose distribution, and the operator can refuse any driver for good with one entry.
- Every automatic load is traceable to the event that caused it, so a driver that wedges hardware can be traced to the plug or sweep that loaded it.
- `kmod-config` autoload entries become unnecessary for hardware that is present. They stay valid for a module that matches no device, such as a protocol or a filesystem.
- The boot path gains a sweep. It runs in the helper, so a slow `modprobe` delays a driver and never the loop. `kmod-config` autoload entries now load after the sweep, through the same queue, where before they loaded synchronously at start.
- An explicit load request waits on the queue behind any automatic load in progress, which is the price of one `modprobe` at a time.
- A module whose dependency is blacklisted is refused even when the operator names it. That is what "never load this" means.

## Alternatives considered

- **Let the kernel's `request_module()` and a udev-style rule file do it.** Rejected: it would be a second load path with its own rules, decided outside the API, and the API-First mandate puts every decision behind an endpoint.
- **Allowlist over explicit loads too.** Rejected for now. An operator who names a module has made the decision the allowlist exists to require. A locked-down host that wants explicit loads refused as well needs a separate setting, and none is decided here.
- **Blacklist as `modprobe.d` alone**, letting `modprobe` refuse and the daemon report its exit status. Rejected: `modprobe` reports a refused `install` rule as a bare exit 1, so the API could not say why, and the dependency rule could not be explained.
- **Unload on blacklist.** Rejected by the owner: editing a policy must not pull a driver from under running hardware.
