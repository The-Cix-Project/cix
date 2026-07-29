# Changelog

All notable changes to this project are recorded here. Format is loosely [Keep a Changelog](https://keepachangelog.com/)-style, adapted for a rolling-release OS built phase by phase rather than a semantically-versioned library: entries are grouped by roadmap phase (see `docs/ROADMAP.md`), newest first. This file is updated as part of every meaningful change, not as an afterthought — see `CLAUDE.md`'s Documentation Map.

## [Unreleased]

### Phase 12 (part 7): real network interface passthrough

A real PCI/USB NIC has no /dev node, so the existing BPF_CGROUP_DEVICE passthrough mechanism (ADR-0017) doesn't apply -- the only real kernel primitive is moving the interface's netdev into a container's own network namespace. The largest piece of the router use case. See ADR-0022.

#### Added
- `netplane/src/rtnetlink.c`/`.h`: `rtnl_link_set_netns_fd()` -- moves a link via an open netns fd instead of a pid (needed for teardown, since the owning process may already be gone by then).
- `daemon/src/device.c`: a third bus, `net:`, walking `/sys/class/net`, excluding every kernel-created software interface (anything resolving under `/sys/devices/virtual/net/` -- bridges, veths, kanxeo's own managed networks).
- `include/container.h`: `CONTAINER_MAX_INTERFACES`, `container_spec.interfaces[]`/`interface_count`; `container_handle.interfaces_netns_fd`.
- `src/container_net.c`/`include/internal.h`: `container_net_host_attach_interfaces()` (parent side, moves interfaces in, brings them up from inside the target netns via a forked helper -- required, since the kernel administratively downs a link as part of moving it to a new netns) and `container_net_teardown_interfaces()` (a forked helper does the reverse move at container removal).
- `daemon/include/registry.h`/`daemon/src/registry.c`: `interfaces[]` name mirror for teardown.
- `daemon/src/main.c`: `POST /v1/containers` gains an `"interfaces": ["wlan0"]` array, validated against `GET /v1/devices`' own `net:` entries.
- `cli/src/main.c`: repeatable `run --interface=IFNAME`.
- `docs/api/openapi.yaml`: `Device.bus` gains `net`; `ContainerCreateRequest.interfaces`; `Container.interfaces`.
- `docs/adr/0022-real-nic-passthrough-netns-move.md`.

Verified two ways: `GET /v1/devices`' new discovery logic checked directly against this dev sandbox's own real hardware; the actual netns-move mechanism proven end-to-end in `test/test_container_net.c` using a real veth pair's own two ends as a stand-in for "a named interface not yet in a container's netns" (legitimate -- `rtnl_link_set_netns_pid()`/`rtnl_link_set_netns_fd()` are confirmed fully generic, not veth-specific) -- 3 consecutive clean runs. Two real bugs found and fixed while building that verification, not before: the original design brought an interface up *before* moving it (doesn't survive the move -- corrected to bring it up after, from inside the target netns); the test's own first check of "is it visible inside" used `/sys/class/net`, whose view is captured at mount time and isn't dynamically netns-aware (corrected to a fresh-socket `SIOCGIFFLAGS` check). Zero warnings; full regression sweep (every other test in the suite) re-run clean.

### Phase 12 (part 6): explicit, operator-chosen IPs for container network attachments

A router's own interfaces typically need stable, predictable addresses, not whatever `network_alloc_ip()`'s first-free-address scan happens to pick — a gap surfaced alongside part 5, from the same router use case. See ADR-0021.

#### Added
- `daemon/src/registry.c`/`daemon/include/registry.h`: `registry_ip_available(candidate_be)`, a pure collision check extracted from `registry_alloc_ip()`'s own scan loop.
- `daemon/src/network.c`/`daemon/include/network.h`: `network_ip_available(name, ip_be)` -- subnet-membership, range, and reserved-gateway validation, delegating the final collision check to the registry; `network_error` gains `NETWORK_ERR_IP_OUT_OF_RANGE`/`NETWORK_ERR_IP_TAKEN`.
- `daemon/src/main.c`: `handle_create()`'s `"networks"` array entries may now be `{"name":..., "ip":...}` objects, not only bare strings (`parse_network_entry()`).
- `cli/src/main.c`: `run --network=NAME:IP`, parsed the same way `--route=DEST/PREFIX:VIA` already is.
- `docs/api/openapi.yaml`: `NetworkAttachmentRequest` schema; `ContainerCreateRequest.networks` items are now `oneOf: [string, NetworkAttachmentRequest]`.
- `docs/adr/0021-explicit-network-ip-override.md`.

Verified over real HTTP against a live daemon: `test/test_networks.c` extended to confirm an explicit valid IP is honored (not auto-allocated), and that the reserved gateway address, an out-of-subnet address, and an already-assigned address are each correctly rejected (400/400/409) -- 3 consecutive clean runs. Zero warnings; full regression sweep re-run clean.

### Phase 12 (part 5): `pkg install` targets an explicit image, not always "base"

`pkg install` had exactly one destination, hardcoded (`pkg_build_completed()`'s merge step) — no way to build a `router`-flavored image carrying `bash`/`iproute2`/`bird` without every container on the shared `base` image getting them too, a real gap the user's own router use case surfaced directly. See ADR-0020.

#### Added
- `daemon/include/pkg.h`/`daemon/src/pkg.c`: `pkg_install_start()`/`pkg_get_one()`/`pkg_delete()` gain an `image` parameter (`NULL`/empty defaults to `"base"`); the package registry's lookup key becomes a (name, image) compound key (`struct pkg_entry` gains `image[]`, persisted in `pkg_installed.json` with backward-compatible defaulting for pre-existing state files).
- `daemon/src/main.c`: `POST /v1/pkg/install`'s body gains an optional `"image"` field; `GET`/`DELETE /v1/pkg/{name}` gain `@`-separated compound addressing (`{name}@{image}`, bare `{name}` still means `base`).
- `cli/src/main.c`: `pkg install --image=NAME`; `pkg rm NAME[@IMAGE]`; `pkg ls`'s formatter gains an image column.
- `docs/api/openapi.yaml`: `PkgInstallRequest.image`, `PkgEntry.image` (now required), `PkgName` path parameter's pattern extended for the `{name}@{image}` form.
- `docs/adr/0020-per-image-pkg-install-compound-key.md`.

Verified end-to-end over real HTTP against a live daemon: `test/test_pkg.c` extended to install the same recipe (`greeter`) into both `base` and `router`, confirming independent, non-cross-contaminating results, correct `image` fields in `GET /v1/pkg`, correct `@`-addressed `GET`/`DELETE` routing, and that deleting `greeter@router` leaves `greeter@base` untouched — 3 consecutive clean runs. Zero warnings; full regression sweep (`test_daemon`, `test_cli`, `test_networks`, `test_dns`, `test_pki`, `test_daemon_net`, `test_daemon_devices`) re-run clean.

### Phase 12 (part 4): a C runtime for the shared "base" image, seeded at install time

`pkg_build_completed()` only ever merges a package's own build output into `base/rootfs` — confirmed directly (part 3's own final check) that a freshly pkg-installed `bash` fails outright inside a real container (`child: execve: No such file or directory`) because `/lib64/ld-linux-x86-64.so.2` doesn't exist anywhere in the image. Not specific to bash — blocks any dynamically-linked package from ever running. See ADR-0019.

#### Added
- `image/src/mkinstalleriso.c`: stages `ld-linux-x86-64.so.2`, `libc.so.6`, and `libtinfo.so.6` into a new `/payload/kanxeo-runtime/` payload subtree, following the exact convention already used for `kanxeod`'s own runtime deps (`test_image_fixture_build()`) and the installer's own `g_lib_closure[]`.
- `image/src/kanxeo-install.c`: new `KANXEO_RUNTIME_DIR_SRC` constant; the existing containers-partition block (previously format-check only) now copies those three files into `images/base/rootfs/{lib64,lib/x86_64-linux-gnu}` at real install time.
- `docs/adr/0019-runtime-libs-seeded-at-install-time.md`.

Verified two ways: `test/test_installer.c` extracts the real containers partition right after the installer's own first boot — before any `pkg install`, before the independent second-boot persistence check (part 2) — and confirms all three files are present with correct real byte sizes. Separately, a throwaway `base` image manually seeded with the same three files let a real `bash -c` genuinely execve() and run inside a real container, printing its own version string — the actual property this fix is for. Zero warnings; `test_boot`/`test_boot_ab` re-verified with no regression; `test_installer` re-verified, 3 consecutive passes.

### Phase 12 (part 3): real pkg recipes for bash, iproute2, and bird

With persistence genuinely real (part 2), the first real `.recipe` files: `pkg/recipes/{bash,iproute2,bird}.recipe`, each with a real upstream source URL and a sha256 verified against an independent authority beyond the daemon's own download.

#### Added
- `pkg/recipes/bash.recipe`, `pkg/recipes/iproute2.recipe`, `pkg/recipes/bird.recipe`.
- `daemon/src/pkg.c`: `pkg_bootstrap_build_image()` now also stages standard `/dev` nodes (`null`/`zero`/`full`/`random`/`urandom`), a writable sticky-bit `/tmp`, and a small set of targeted host paths a real build reaches for beyond the existing `/usr/{include,lib,lib64,bin,libexec}` copy — `/etc/alternatives` (Debian's own indirection for tools like `awk`), `/usr/share/bison`, `/usr/share/autoconf`, `/usr/share/perl` (autoconf is itself a perl script).

Verified by actually building and installing all three through a live daemon, not written from a template and assumed correct — each staging gap above was found by a real build failing (`./configure: cannot create /dev/null`, `awk: command not found`, `bison: .../m4sugar.m4: cannot open`, `Can't locate Class/Struct.pm`, `sysdep/autoconf.h: No such file or directory`) and fixed one at a time, not guessed at up front. BIRD's own tarball needed one more fix specific to it: no pre-generated `./configure`, requiring `autoreconf -fi` in its own recipe.

### Phase 12 (part 2): a real reboot no longer wipes every installed package, network, and image

`boot_init()` has mounted a fresh `tmpfs` at `BASE_DIR` (`/var/lib/kanxeo`) on every real boot since Phase 11 part 1 — and every piece of daemon-persisted state lives there. A full power cycle on a real installed system silently discarded it all. Found while scoping the next step (installing real software via `pkg install`, which only matters if it survives a reboot), not reported as a bug. See ADR-0018.

#### Fixed
- `daemon/src/main.c`: `boot_init()` now mounts the real, already-formatted `kanxeo-containers` partition (`/dev/vda5`) at `BASE_DIR`, falling back to `tmpfs` only when that device doesn't exist (parts 1/2's own throwaway test disks). One new `sync()` call, gated on `--init-mode`, right after state-init and before the listening socket opens — the "listening on" line is exactly the signal both this project's tests and a real operator treat as "safe to power-cycle," and QEMU's default write-back disk cache (matching real hardware's own guest-side dirty-page caching) doesn't guarantee that's true without one.

Verified with a real cross-boot proof, not just "the mount succeeded": `test/test_installer.c` extracts the real on-disk containers partition after the installed system's first full boot, confirms `kanxeod`'s own directories are genuinely there, writes a marker file directly into it (`debugfs -w`), and confirms it survives a *completely independent second boot* of the same disk, byte-for-byte. Two more real bugs found and fixed while building that proof: the new `sync()` initially landed *after* the "listening on" print, racing the test harness's own kill-on-marker behavior and defeating the fix; and a partition extracted right after a QEMU session still carries a pending ext4 journal, which a raw `debugfs` write can have silently reverted by the next real mount's journal replay unless `e2fsck -fy` forces that replay first. Zero warnings; `test_boot`/`test_boot_ab` re-verified (tmpfs-fallback path unaffected); `test_installer` re-verified, 3 consecutive passes.

### Phase 12 (part 1): PCI/USB (character/block) device passthrough to containers

The expanded charter's first concrete step, agreed directly with the user: pass a real USB device or a driver-backed PCI device (e.g. an NVMe namespace) straight to a container. Two architectural forks were confirmed before writing any code — enforcement via `BPF_CGROUP_DEVICE` (cgroup v2's only device-access mechanism; see ADR-0017 for why this doesn't reopen the networking plane's own separately-scoped "no eBPF" rule) rather than namespace-only isolation (this project's containers run as full root with no user namespace, so `mknod()` of an ungranted device would otherwise just work), and sysfs auto-discovery rather than operator-registered devices.

#### Added
- `include/linux_compat.h`: raw `bpf(2)` syscall wrapper and self-declared `union bpf_attr`/`struct bpf_insn` equivalents, following the exact pattern `struct clone_args` already established for `clone3(2)`.
- `src/container_dev.c`: hand-assembles a `BPF_PROG_TYPE_CGROUP_DEVICE` program (no libbpf, no external BPF toolchain) per container, attached to the cgroup leaf before `ns_clone3()`; `mknod()`s each granted node right after `mountns_pivot()`. `include/container.h` gained `struct device_spec`/`CONTAINER_MAX_DEVICES`; `container_handle` gained `bpf_prog_fd`.
- `daemon/src/device.c`: walks `/sys/bus/usb/devices` and `/sys/bus/pci/devices` fresh on every call (never persisted). USB nodes resolve via each device's own `dev` attribute; PCI nodes via a bounded-depth walk of each device's own sysfs subtree (handles NVMe's controller+namespace nesting and a bridge/root port's own further-enumerable downstream devices).
- `GET /v1/devices`; `POST /v1/containers`' new `devices` array (bare ids, matching `networks`' request-is-references shape); `daemon/include/registry.h`'s `registry_device_attachment` mirrors `registry_network_attachment`.
- `kanxeoctl device ls`; a repeatable `run --device=ID` flag.
- `docs/api/openapi.yaml`: `/devices`, `Device`, `ContainerDeviceAttachment`, `ContainerCreateRequest.devices`, `Container.devices`.
- `image/kernel/qemu-part1.config`: `CONFIG_BPF_SYSCALL`/`CONFIG_CGROUP_BPF` (confirmed absent before this change).
- `docs/adr/0017-ebpf-cgroup-device-filter-for-hardware-passthrough.md`.
- `include/pathutil.h`: `kx_mkdir_p()` extracted from `daemon/src/persist.c`'s `persist_mkdir_p()` so the runtime library doesn't gain a dependency on the daemon layer.

Verified with real, not mocked, syscalls: `test/test_devices.c` proves the actual security property through a real `container_create()` (a granted device opens with the correct `fstat()`-reported major:minor; a different container is denied `EPERM` on a device it wasn't granted, even though it's visibly present; a container requesting no devices is byte-for-byte unaffected) — 3 consecutive passes. `test/test_daemon_devices.c` proves the REST/registry/CLI wiring over real HTTP, adapting to whatever hardware the test host actually has. A real environment constraint was found and documented rather than worked around silently: `BPF_CGROUP_DEVICE` checks are hierarchical, and this project's own dev/build environment (a privileged, nested LXC) runs under an ancestor cgroup permitting only a standard device set — `test_devices.c` grants real device numbers for this reason, not synthetic ones (see ADR-0017's Consequences). Zero warnings; full clean rebuild and complete pre-existing test suite re-verified with no regressions.

### Phase 11 (part 7): graceful shutdown/reboot for the installed system

Asked directly, once a real install was up and reachable: "how do I start/stop the OS when it's running live?" The honest answer was there was no way to do that cleanly — `kanxeod` as PID 1 returning from `main()` (its existing `SIGTERM`/`SIGINT` handling) is exactly "init exited," which the kernel panics on unconditionally, the same failure already accepted as harmless for the one-time `kanxeo-install` run but not acceptable for a live system. See ADR-0016.

#### Added
- `daemon/src/main.c`: calls the real `reboot(2)` (`RB_POWER_OFF`/`RB_AUTOBOOT`) once the event loop stops, gated strictly behind `--init-mode` (real PID 1) — a dev/test `kanxeod` (every `test/*.c` invocation, `sudo build/kanxeod`) never reaches it, unchanged. New `POST /v1/system/shutdown` and `/v1/system/reboot` (API-first per ADR-0005); `SIGTERM`/`SIGINT` now default to a graceful poweroff instead of silently panicking under `--init-mode`.
- `cli/src/main.c`: `kanxeoctl shutdown` / `kanxeoctl reboot`.
- `docs/api/openapi.yaml`: the two new endpoints.
- `docs/adr/0016-reboot-syscall-for-kanxeod-shutdown.md`.

The core mechanism (`reboot(2)` from genuine PID 1) was verified directly: a minimal standalone init booted on this exact kernel build confirmed both `RB_POWER_OFF` ("reboot: Power down") and `RB_AUTOBOOT` ("reboot: Restarting system") complete cleanly, no panic. The *full* REST-triggered path couldn't be exercised end-to-end by this project's own QEMU test harness — SLIRP (`-netdev user`) doesn't route host-to-guest traffic to a guest's own self-configured static IP, only to its own DHCP-assigned one, which doesn't match `kanxeod`'s `--bind=<ip>` addressing. A test-harness limitation, not a real-world one (bridged networking, e.g. Proxmox, has no equivalent restriction) — see ADR-0016's own Consequences for the honest boundary. Zero warnings; full pre-existing 13-binary non-boot suite plus `test_boot`/`test_boot_ab` re-verified with no regression.

### Phase 11 (part 6): web dashboard 404'd on every installed system

Found live: `kanxeod` reachable, `kanxeoctl` working, but the browser dashboard returned "Not Found" on every request.

#### Fixed
- `image/src/mkbootroot.c`: `kanxeod`'s `DEFAULT_WEB_ROOT` (`daemon/src/main.c`) is a relative path (`"web"`), resolved against PID 1's own CWD (never `chdir()`'d, so the squashfs root itself) — but `mkbootroot` never staged the `web/` directory into that image at all, only `kanxeod` + its two dynamic-link dependencies. Every dashboard request 404'd; the REST API worked fine since it's a separate routing path (`static_serve()` only handles what doesn't match `/v1/...`). New `copy_dir_files()` (flat, not recursive — `web/`'s own three files, `index.html`/`app.js`/`style.css`, have no subdirectories, matching ADR-0010's "no framework, no build step" design) stages it alongside `kanxeod` itself. `build/mkbootroot`'s own argv grew a required `<web-dir>` argument; all four call sites (`test/test_boot.c`, `test/test_boot_ab.c`, `test/test_installer.c`, `README.md`) updated.

Confirmed directly (`unsquashfs -l`) that `web/index.html` etc. land at exactly the path `kanxeod` resolves at runtime, not just inferred from the fix compiling. Zero warnings; `test_boot`/`test_boot_ab`/`test_installer` re-verified (3 consecutive `test_installer` passes); the real, shippable `build/kanxeo-install.iso` rebuilt.

### Phase 11 (part 6): non-interactive partitioning (`--auto-partition`)

Typing the same fixed `fdisk` command sequence by hand for every VM/scripted install was pure friction, not a meaningful safety check — asked for directly after a manual reinstall proved painful.

#### Added
- `image/src/kanxeo-install.c`: new `--auto-partition` flag — scripts `sfdisk` (already staged and already used read-only for role detection) with the same fixed 5-partition GPT layout `fdisk`/`cfdisk` always produced, via a new `run_subprocess_stdin()` (mirroring `test/test_disk_image.c`'s own). Mutually exclusive with `--skip-partition`; omitting both still means interactive `fdisk`, unchanged, for anyone who needs different sizing. Confirmed byte-for-byte identical output to the existing interactive/scripted-`sfdisk` layouts via `sfdisk -d`.
- `README.md`: documents all three partitioning modes, `--auto-partition` as the recommended default for VM/scripted use.

#### Changed
- `test/test_installer.c`: the main install session now uses `--auto-partition` instead of `--skip-partition` + host-side `sfdisk` pre-partitioning — `create_target_disk()` (which duplicated the same partition script host-side) is gone, replaced by a plain `create_blank_disk()`; one source of truth for the layout (`kanxeo-install.c` itself) instead of two copies kept in sync by hand. This also means `--auto-partition` now gets exercised for real by the same 3-consecutive-pass install flow every other Secure Boot fix already goes through, not just a one-off spike.

Zero warnings; `test_installer`/`test_boot`/`test_boot_ab` re-verified; the real, shippable `build/kanxeo-install.iso` rebuilt with `--auto-partition`.

### Phase 11 (part 6): kanxeod unreachable after install; MokManager's "Continue boot" trap documented

Two more findings from the same real install, after Secure Boot itself was confirmed working end to end.

#### Fixed
- `image/src/kanxeo-install.c`: `kanxeod` has a working `--bind=ADDR` flag (defaults to `127.0.0.1`), but the generated loader entry never passed it — so a freshly-installed system always listened on loopback only, unreachable from the network, even though `apply_static_ip()` had already configured the real address on `eth0` moments earlier. `populate_esp()` now takes the install's own `--ip=` value and bakes `--bind=<ip>` into the loader entry, binding exactly the one real address this install is for. Confirmed directly (spiked, not assumed): `kanxeod listening on 192.168.77.77:7620`.

#### Documented
- `README.md` / `docs/adr/0015-shim-mok-secure-boot-signing.md`: a real firmware-level trap found live during the user's own MOK confirmation — selecting **"Continue boot"** at `MokManager`'s main menu doesn't defer the pending enrollment request, it **permanently discards it** (confirmed by booting the scenario twice: the "Enroll MOK" option is simply gone from every later boot's menu). Not something in this project's own code to fix. `README.md` now warns loudly and documents the recovery path the user actually used successfully: hash-enrolling `\EFI\BOOT\grubx64.efi` and `\kanxeo-bzImage` individually via the same menu's "Enroll hash from disk" — narrower than the cert-based path (tied to exact file hashes, doesn't survive a kernel rebuild) but works without a reinstall.

Zero warnings; `test_installer`/`test_boot`/`test_boot_ab` re-verified (3 consecutive `test_installer` passes); the `--bind=` fix confirmed via a dedicated spike, not just inferred from the existing test's own (differently-scoped) success marker.

### Phase 11 (part 6): real-world install fixes found via an actual VM install

Four more real, previously-untested bugs found by walking the `v1.2.0` fix through an actual Proxmox install, none of them caught by the automated suite because each one lived in a path the tests had always bypassed for good reasons at the time (interactive UIs that seemingly couldn't be scripted, or a QEMU-only shortcut) — every one of them now has real coverage, not just a fix.

#### Fixed
- `image/src/mkinstalleriso.c`: `grub.cfg`'s `set timeout=0` booted the menu instantly with no visible window to press `e` — defeating the whole "edit `--disk=`/`--ip=`/... at the boot menu" design the placeholder args rely on. Bumped to `timeout=10`.
- `image/src/mkinstalleriso.c` / `image/src/kanxeo-install.c`: both the installer media and the installed system's own boot now print to `console=tty0` in addition to `console=ttyS0` — Kanxeo's boot chain previously only ever wrote to serial, so an operator watching Proxmox's default display console (rather than a serial terminal) saw nothing at all past firmware handoff and reasonably assumed a hang.
- **`cfdisk` replaced with `fdisk`** for real, interactive partitioning: `cfdisk`'s `ncurses` UI needs a terminal database that was never staged into the installer image, so every real (non-`--skip-partition`) install failed outright with `Error opening terminal: linux.` — a path the test suite had always skipped specifically because `cfdisk`'s full-screen UI couldn't be scripted. `fdisk`'s line-based UI has no such dependency (verified directly: works with `TERMINFO`/`TERMINFO_DIRS` pointed at nonexistent paths) and, unlike `cfdisk`, *can* be scripted the same way `sfdisk` already is — closing the coverage gap instead of just working around it. `image/src/mkinstalleriso.c`'s lib closure shrank accordingly (`libmount`, `libncursesw`, `libselinux`, `libpcre2-8` were `cfdisk`-only).
- `README.md`: documents the target disk only working via **VirtIO Block** today (`/dev/vda`) — the kernel has no SCSI-disk driver, so SATA/IDE/VirtIO-SCSI-attached disks never appear as a device at all, regardless of path given.

#### Added
- `test/test_disk_image.c`: `qemu_boot_capture()`'s scripted-input matching now tracks a search offset instead of re-scanning the whole cumulative buffer — needed once a real prompt (`fdisk`'s own `Command (m for help): `) started repeating many times in one session, which the original "does this text exist anywhere" check couldn't distinguish between occurrences of.
- `test/test_installer.c`: two new smoke-test sessions, each proving a real, unmodified interactive UI this project depends on but had never actually driven end to end: (1) the real GRUB menu/config path (`grub-mkrescue`'s own `BOOTX64.EFI` + `grub.cfg` via a normal `-cdrom` attach, not the Secure Boot flow's `-kernel` bypass) — this is what would have caught the `timeout=0` bug; (2) `fdisk`'s real interactive partitioning path, scripted through its actual prompts (command sequence verified directly against a real `fdisk` first, byte-for-byte matched against the existing `sfdisk`-scripted layout via `sfdisk -d`) — this is what would have caught the terminfo bug. A genuine, non-obvious gotcha hit and fixed while building the `fdisk` script: `readline`'s own horizontal-scroll behavior on an 80-column serial terminal silently drops the *left* portion of long prompts (`Last sector, +/-sectors or +/-size{K,M,G,T,P}...` never appears in the byte stream at all, replaced by a `<` scroll indicator) — the matching anchor has to target text near the cursor position, not the start of a long prompt.

Zero warnings; `test_installer`/`test_boot`/`test_boot_ab` re-verified; the real, shippable `build/kanxeo-install.iso` rebuilt.

### Phase 11 (part 5): Secure Boot for the installed system — shim + MOK enrollment

A real user-reported defect in the `v1.1.0` ISO: booting it on a Secure-Boot-enabled VM failed with UEFI `BdsDxe: ... Access Denied`. Reproduced exactly, locally, then root-caused: `grub-mkrescue`'s self-built `BOOTX64.EFI` is unsigned, and — once a signed bootloader chain was tried — a *second*, deeper issue surfaced: Secure Boot also requires a signed kernel, not just a signed bootloader, and this project has no CA relationship to get one signed conventionally. See ADR-0015 for the full reasoning, including the MOK-enrollment bootstrap chicken/egg problem and why the installer media itself stays unsigned (Secure Boot must be off for that one, disposable boot) while the *installed* system becomes fully Secure-Boot-capable after one operator-confirmed key enrollment.

Two real, non-obvious facts found via direct empirical testing this part, not assumed: Secure Boot enforcement in OVMF is gated by the **vars file's own PK-enrollment state**, not the CODE build variant (isolated by testing each independently); and `-kernel` (QEMU's own direct-boot injection) bypasses firmware's normal `LoadImage`-based Secure Boot check entirely regardless of vars state — used to let the test's own installer-boot session use the exact same, already-enrolled vars file the later MOK-confirm/final-boot sessions need, with no vars-file-merging step required. shim's real MokManager UI (menu text, navigation, password prompt) was discovered by driving a live run interactively and observing the raw captured screen output, the same technique used earlier this part to diagnose the original bug.

#### Added
- `image/keys/kanxeo-signing.{key,crt,cer}`: a project-owned Secure Boot signing key, generated once and persisted deliberately (private key gitignored; public cert, PEM and DER, committed).
- `image/src/mkinstalleriso.c`: signs `systemd-boot` and the kernel with the Kanxeo key via `sbsign`, and stages Debian's pre-signed `shim`/`MokManager` plus the signed systemd-boot (deliberately named `kanxeo-grubx64.efi` — `shim` has a hardcoded `grubx64.efi` second-stage lookup, confirmed via `strings` on the real binary) and the DER cert for the target ESP. Three new required arguments (`<signing-key> <signing-cert.crt> <signing-cert.cer>`); the installer media's own GRUB boot chain is otherwise unchanged.
- `image/src/kanxeo-install.c`: `populate_esp()` now writes the full `shim` → signed-`systemd-boot` → signed-kernel chain onto the target ESP instead of an unsigned `systemd-boot` direct-boot; new `enroll_signing_key()` mounts `efivarfs` and runs `mokutil --import`, prompting once for a password reused at the installed system's next boot to confirm enrollment via firmware's own `MokManager`.
- `image/kernel/qemu-part1.config`: `CONFIG_EFIVAR_FS` — needed for `/sys/firmware/efi/efivars`, which `mokutil` reads/writes.
- `test/test_disk_image.h`/`.c`: `qemu_boot_capture()` gained `secure_boot` (switches OVMF CODE build to document intent; the vars file is what actually gates enforcement), `direct_kernel`/`direct_kernel_args` (the `-kernel` bypass above), and `struct qemu_scripted_input` — event-driven scripted stdin (matches on captured output, not sleep-timed) for driving `mokutil`'s and `MokManager`'s interactive prompts.
- `test/test_installer.c`: extended to a three-session flow — install (direct-kernel-booted, stages the MOK request into the real, already-User-Mode vars file), a MOK-confirm boot (`secure_boot=1`, scripted through shim's actual MokManager menu), then the real final boot (`secure_boot=1`), proving the installed system boots cleanly with Secure Boot enforced end to end.
- `docs/adr/0015-shim-mok-secure-boot-signing.md`.

#### Fixed
- `mokutil`'s own `ldd` closure was missing `libdl.so.2` in the first pass (`mokutil: error while loading shared libraries: libdl.so.2`) — caught by an actual run inside the installer environment, not by review; fixed in `mkinstalleriso.c`'s `g_lib_closure[]`.

Zero warnings; `test_installer`/`test_boot`/`test_boot_ab` each re-run 3 consecutive times; full pre-existing binary suite re-run once after a clean rebuild; the real, shippable `build/kanxeo-install.iso` rebuilt with the new signing-key arguments.

### Phase 11 (part 4): ISO packaging — the installer becomes a real, distributable artifact

**Phase 11 is now complete — all 4 parts done.** Part 4's job: package the installer environment as an actual `.iso` file, the concrete artifact an operator hands to their own hypervisor. Confirmed with the user before writing any code: since this dev LXC has no physical hardware, this part's own completion bar is booting the real `.iso` via QEMU's CD-ROM emulation — a genuinely different, more realistic path than the raw-disk-image testing parts 1–3 used; a real VM/hardware install is the user's own next step.

A working approach was found via direct empirical spiking, not guessed: hand-tuning `xorriso`'s raw El Torito/GPT-hybrid flags proved genuinely fiddly and kept failing even with a real FAT boot image; `grub-mkrescue` (the standard tool every major distro uses for this) worked on the first real attempt. The kernel mounts the ISO9660 media directly as root (`root=/dev/sr0 rootfstype=iso9660 ro`) — no initramfs, consistent with every other root in this phase.

#### Added
- `image/src/mkinstalleriso.c` (`build/mkinstalleriso`): stages `kanxeo-install` + its full real-tool closure (`cfdisk`/`sfdisk`/`mkfs.vfat`/`mkfs.ext4`) + the target-disk payload + a generated `/boot/grub/grub.cfg`, shells out to `grub-mkrescue`. The last CLI argument is the kernel command line tail after `init=/bin/kanxeo-install --`, so the same tool builds both the real, shippable ISO (`/dev/CHANGEME` placeholder args) and `test_installer.c`'s own verification ISO (real test values + `--skip-partition`).
- `image/kernel/qemu-part1.config`: `CONFIG_ATA`/`CONFIG_SATA_AHCI`/`CONFIG_BLK_DEV_SR`/`CONFIG_ISO9660_FS` — the ATA→SCSI→CD-ROM block-device stack plus ISO9660 filesystem support.
- `test/test_disk_image.h`/`.c`: `qemu_boot_capture()` refactored from a growing positional-parameter list to a `struct qemu_boot_opts` (adds `disk_img_is_cdrom`); all four existing call sites updated.
- `build/kanxeo-install.iso`: the real, shippable production ISO, built with placeholder `--disk=/dev/CHANGEME --ip=CHANGEME --gateway=CHANGEME` args an operator edits at the GRUB boot menu (press `e`) before booking — `kanxeo-install`'s existing `stat()` check on `--disk=` already fails safely if left unedited.

#### Changed
- `image/src/kanxeo-install.c`: `BZIMAGE_SRC` moved from `/payload/kanxeo-bzImage` to `/boot/kanxeo-bzImage` — one kernel copy serves both GRUB's own boot and the file copied onto the target disk's ESP.
- `test/test_installer.c`: simplified, not just extended — its own private `build_installer_image()`/`build_boot_esp()` are gone; session 1 now boots the real `build/mkinstalleriso` output via `-cdrom`, a strictly more faithful test than part 3's private disk-image approximation.
- `docs/ROADMAP.md`: Phase 11 marked fully done (all 4 parts).

#### Fixed
- **A self-inflicted environment bug, not a code defect**: an early exploratory spike accidentally pointed a `-drive if=pflash,...` argument directly at the shared system template `/usr/share/OVMF/OVMF_VARS_4M.fd` instead of a private copy, permanently corrupting it with a phantom "boot from DVD-ROM" NVRAM entry that silently broke every test in this project (all of them copy from that same template). Fixed with `apt-get install --reinstall ovmf`; re-verified against `test_boot`/`test_boot_ab` immediately.

Zero warnings; `test_installer` re-run 3 consecutive times; `test_boot`/`test_boot_ab` (parts 1–2) each re-run 3 consecutive times to confirm zero regression from the `qemu_boot_capture()` refactor and kernel-config additions; the full pre-existing 13-binary suite re-run once more after a clean rebuild.

### Phase 11 (part 3): the installer — cfdisk flow, real 5-partition layout, static IP

Part 3's job: build the actual installer that takes a raw disk and produces the real 5-partition layout (ESP, root A, root B, config, containers) an operator would use, culminating in a real, running `kanxeod` on a freshly-installed system.

Confirmed with the user before writing any code, four rounds: the installer is tested inside a QEMU guest against a second attached virtio disk, using ordinary `mount()`/`mkfs` (the loop-device workaround `test_disk_image.c` needed in part 1 only applies to building a disk *image file* on this dev LXC, not to a real block device inside a guest); a new standalone binary, not a `kanxeod` mode; CLI-flag input (`--disk=`, `--ip=`, `--prefix=`, `--gateway=`), not interactive prompts; the very first install is counted with a boot-attempt counter too, exactly like any future update.

#### Added
- `image/src/kanxeo-install.c` (`build/kanxeo-install`): the installer. Bootable itself (a second `init=` target, part 4 turns this into a distributable ISO) — confirms the target disk, optionally shells out to real `cfdisk` (`--skip-partition` for scripted provisioning and this part's own test, which can't drive `cfdisk`'s curses UI), reads roles back from GPT partition *names* (`kanxeo-esp`/`kanxeo-root-a`/`kanxeo-root-b`/`kanxeo-config`/`kanxeo-containers`), formats, writes the bundled root-A squashfs raw plus a counted loader entry, writes a static-IP `net.conf` to the config partition, leaves root B and containers empty for `kanxeod`'s own existing machinery to use later.
- `daemon/src/main.c`: `boot_init()` now also (non-fatally) mounts the config partition and applies `net.conf` if present — new `find_nic()` (scans `/sys/class/net` for the first real interface rather than hardcoding a name), `parse_net_conf()`, `apply_static_ip()`, reusing the exact `rtnl_*` primitives `lo`'s own bring-up already established.
- `image/kernel/qemu-part1.config`: `CONFIG_EXT4_FS` for the config/containers partitions.
- `test/test_disk_image.h`/`.c`: `qemu_boot_capture()` gained an optional second disk and an optional virtio-net device; `extract_partition()` (reverse of `write_at_offset()`, needed for `debugfs`, which has no `@@offset` addressing the way `mtools` does); `build_squashfs()`; a shared `rm_tree()` (same pattern `test_overlay.c` already established) now called on every boot test's successful exit, fixing a real hygiene gap — 16 leftover `mkdtemp()` workdirs, ~1GB, had silently accumulated across this phase's own development.
- `test/test_installer.c` (new): two QEMU sessions — installer + blank pre-partitioned target disk, then the target disk alone with a NIC attached, proving install and boot-what-was-installed are the same real code paths already proven in parts 1–2. Verification happens from the host side: ESP contents via `mtools`, root A's raw bytes compared byte-for-byte against the original squashfs, `net.conf` via `debugfs`, and the second boot's own log confirming the static IP was genuinely applied.

#### Changed
- `docs/ROADMAP.md`: Phase 11 part 3 marked done with what shipped and how it was verified.

#### Fixed
- **`kanxeo-install` needs the same minimal PID-1 boot shape `kanxeod` already has** — the first attempt failed with `/proc: No such file or directory`; neither an early proc/sysfs mount step nor the mountpoint directories existed in the installer's own image. Fixed with a new `early_mounts()` mirroring `boot_init()`, and the missing mountpoints in `test_installer.c`'s staging step.
- **The static-IP test only proved the config file was written, not applied** — the installed system's second boot never had a NIC attached, so `find_nic()` correctly (and harmlessly) found nothing. Caught by re-reading what the test actually proved; strengthened by attaching a NIC and asserting the exact applied address/gateway appear in the boot log.

Zero warnings; `test_installer` re-run 3 consecutive times; `test_boot`/`test_boot_ab` (parts 1–2) each re-run 3 consecutive times to confirm zero regression; the full pre-existing 13-binary suite re-run once more after a clean rebuild.

### Phase 11 (part 2): A/B write + systemd-boot's boot-counter + automatic rollback

Part 2's job: prove the rollback mechanism itself works — a slot that boots but never confirms is automatically abandoned in favor of the other one, no operator involved. The real payoff of ADR-0014's "own the counter with systemd-boot's native mechanism, not a bespoke one" call.

Confirmed with the user before writing any code: the failure simulated is a *valid* boot where `kanxeod` deliberately never confirms (`--simulate-unhealthy-boot`), not a corrupted/unmountable root — the real failure class this exists for. Slot identification is an explicit `--slot=a`/`--slot=b` kernel param. The test harness launches QEMU fresh per power-on attempt against the same persistent disk, inspecting the ESP between runs, rather than relying on an in-VM reboot loop.

#### Added
- `daemon/src/main.c`: new `--slot=`/`--simulate-unhealthy-boot` flags; `boot_init()` mounts the ESP (`vfat`, fixed `/dev/vda1`) at `/boot`; new `confirm_boot(slot)` renames the matching `/boot/loader/entries/kanxeo-<slot>*` file down to the bare `kanxeo-<slot>.conf`, stripping `systemd-boot`'s own Automatic Boot Assessment counter suffix — a plain `rename(2)`, not a `bootctl` call. Called right after the existing "listening" line, before the reactor loop — "about to serve traffic" is the definition of healthy this confirms. A failure here logs and continues rather than tearing down an already-healthy daemon, matching `dns_register`/`pki_issue`'s existing best-effort/skip-and-log posture.
- `image/src/mkbootroot.c`: one more empty mountpoint, `/boot`.
- `test/test_disk_image.h`/`.c` (new): every disk/partition/ESP/QEMU-boot primitive extracted from `test_boot.c` once `test_boot_ab.c` became a second real consumer — subprocess runners, `sfdisk_dump_offset()`, `write_at_offset()`, `mtools` wrappers (`esp_mkfs`/`esp_mmd`/`esp_mcopy_in`/`esp_mren`), and a generalized `qemu_boot_capture()` returning the full captured serial text to the caller. `test/test_boot.c` refactored onto it, re-verified with zero behavior change.
- `test/test_boot_ab.c` (new): a 3-partition disk (ESP + root A + root B, identical squashfs on both — only the loader entries differ); slot A's entry carries a 3-try counter and `--simulate-unhealthy-boot`, slot B has no counter (already confirmed-good). Repeated power-on attempts scrape a new `"init-mode: slot=..."` log line and list `/loader/entries` via `mtools`' `mdir -i disk.img@@<offset>` between attempts.
- `image/kernel/qemu-part1.config`: `CONFIG_FAT_FS`/`CONFIG_VFAT_FS`/`CONFIG_NLS`/`CONFIG_NLS_CODEPAGE_437`/`CONFIG_NLS_ISO8859_1` — the kernel had never needed to mount the ESP from inside the guest until now.

#### Changed
- `docs/ROADMAP.md`: Phase 11 part 2 marked done with what shipped and how it was verified.

#### Fixed
- **`default kanxeo-*` alone booted straight to slot B on the very first attempt**, not slot A — a glob only says "these are valid candidates," it expresses no priority. Fixed per the Boot Loader Specification's actual mechanism: both entries get a matching `sort-key` plus explicit `version` fields, slot A's set higher — an unambiguous, deliberate priority signal instead of relying on filename sort-order accidents. Re-verified end to end: three attempts correctly exhaust slot A's counter (`+3`→`+2-1`→`+1-2`→`+0-3`), the fourth automatically lands on slot B.
- **`/boot: No such device`** — the kernel had never been built with `vfat`/`FAT` support at all, since part 1 never mounted the ESP from inside the guest. Fixed with `CONFIG_FAT_FS`/`CONFIG_VFAT_FS`.
- **`FAT-fs (vda1): codepage cp437 not found`** — VFAT needs NLS codepage tables to interpret filenames; none were enabled. Fixed with `CONFIG_NLS`/`CONFIG_NLS_CODEPAGE_437`/`CONFIG_NLS_ISO8859_1`.

Zero warnings; `test_boot_ab` and `test_boot` each re-run 3 consecutive times with identical passing results after a clean rebuild; the full pre-existing 13-binary suite re-run once more to confirm zero regression.

### Phase 11 (part 1): bare-metal boot — kernel, bootloader, and a minimal squashfs root booting kanxeod as PID 1 in QEMU

Architecture for the whole phase confirmed with the user before any code was written, the same discipline every prior phase used: UEFI-only + `systemd-boot`; read-only A/B root as a single atomic `squashfs` image per slot, scoped to the control plane only (kernel + `kanxeod` + its direct runtime deps — never container/package workload data); no initramfs (kernel `exec`s `kanxeod` via `init=`); automatic A/B rollback via `systemd-boot`'s native boot-attempt counter; kernel built from mainline source with a real GCC toolchain, not TCC (ADR-0001's "TCC governs our own code, not unmodified upstream software" precedent); a fixed/known target hardware assumption; static IP at install time via the existing `netplane/` rtnetlink primitives; a 5-partition GPT layout (ESP, root A, root B, config, containers); a real interactive `cfdisk`-driven installer with partition roles read from GPT type/name; `kanxeod` gains an explicit `--init-mode` flag, not `getpid() == 1` auto-detection, so the boot-init path stays testable in a namespace the way `test_harness.c` already tests namespace logic. See `docs/adr/0014-squashfs-ab-root-with-native-boot-counting.md` for the reasoning behind the one call in this design that's both significant and hard to reverse once real installs exist on real disks.

Part 1's own job: prove the chain actually boots, in QEMU, before touching real hardware.

#### Added
- `image/kernel/qemu-part1.config`: a hand-curated `merge_config.sh`-format kernel config fragment (not a full generated `.config`) applied over `make allnoconfig` against kernel 6.18.40 — every option traces to a concrete requirement (cgroup v2 controllers matching `src/cgroup.c`'s actual writes, `EFI_STUB` for direct `systemd-boot` chainloading, `SQUASHFS`+`DEVTMPFS_MOUNT` for the root itself, `VIRTIO_*` for QEMU's virtual hardware — Part 1's own fixed/known "target").
- `image/src/mkbootroot.c` (`build/mkbootroot`): assembles the minimal control-plane `squashfs` root — `kanxeod` + `ld.so`/`libc.so.6` (via `test_image_fixture_build()`, reused rather than reimplemented) + empty `/proc`, `/sys`, `/dev`, `/var/lib/kanxeo` mountpoints — then shells out to the real `mksquashfs`.
- `daemon/src/main.c`: new `--init-mode` flag and `boot_init()` — mounts `proc`/`sysfs`/`cgroup2` (the same flags `mountns.c` already uses for each container's own `/proc`) and a `tmpfs` at `BASE_DIR` (Part 1's stand-in for the real config partition Part 3 mounts there instead), brings `lo` up via the existing `rtnl_link_set_up()` (`netplane/`, reused directly), then falls through into the completely unmodified existing startup sequence.
- `test/test_boot.c` (`build/test_boot`): assembles a throwaway 2-partition disk (ESP + one root slot), boots it in QEMU, and scrapes the serial console for the same `"kanxeod listening on ..."` line every other test already treats as daemon-ready.
- Dev-environment build/boot-test tooling installed and confirmed working: `qemu-system-x86`, `ovmf`, `systemd-boot-efi`, `squashfs-tools`, `mtools`, and kernel-build prerequisites `flex`/`bison`/`libelf-dev`.

#### Changed
- `docs/ROADMAP.md`: Phase 11 added to the phase table and given a full design write-up; Part 1 marked done with what shipped and how it was verified.

#### Fixed
- **`bind(127.0.0.1)` failed with `EADDRNOTAVAIL`** on the first real boot attempt — a fresh kernel leaves `lo` administratively down, and nothing had ever needed to bring it up before (every prior phase ran as a guest on an already-fully-booted host). `kanxeod` exiting as PID 1 panics the kernel unconditionally — correct kernel behavior surfacing a missing boot-init step, not a boot-chain bug. Fixed by bringing `lo` up in `boot_init()`.
- **`devtmpfs: error mounting -2`** — the squashfs root had no `/dev` directory for the kernel's own devtmpfs auto-mount to target. Fixed by adding it to `mkbootroot`'s staged tree.
- **Kernel config fragment silently missing `EFI_STUB`/`VIRTIO_BLK`/`VIRTIO_NET`/`SQUASHFS`/`BINFMT_ELF`** on a from-scratch reproduction, caught by verifying the checked-in fragment actually reproduces the interactively-tested config rather than trusting it matched. Root causes: `scripts/config --enable` sets a symbol directly without checking Kconfig's dependency graph, and a subsequent `make olddefconfig` silently drops anything unsatisfiable (`EFI` needs `ACPI`; `VIRTIO_BLK`/`VIRTIO_NET` live under `menuconfig BLK_DEV`/`NETDEVICES` gates, not just `VIRTIO`; `SQUASHFS` lives under `if MISC_FILESYSTEMS`; `allnoconfig` ignores Kconfig `default y` statements, so `BINFMT_ELF` needs to be explicit despite looking like it should be on by default).
- **No loop devices available in this LXC** (`/dev/loop*` don't exist at all) — `test_boot.c`'s original design (loop-attach the disk image, `mount()` the ESP) couldn't work. Redesigned before writing the losetup/mount code around it: `sfdisk` operates directly on the disk image file, the ESP is built as a standalone FAT32 file via `mtools` (no mount needed), and both the ESP and the root `squashfs` are written into the disk image at their exact partition byte offsets (parsed from `sfdisk -d`).

### Housekeeping: full-codebase audit — eliminate parallel-implementation duplication

A meticulous line-by-line audit of the entire codebase and documentation set, run after Phase 10 (parts 1–2) shipped, checking every Immutable Maxim directly and mechanically rather than by inspection alone: git hygiene (nothing uncommitted, unpushed, stashed, or dangling), file-mtime-vs-commit-history cross-check (no missed reintegrations), a systematic grep for structurally duplicated logic across the daemon and test suite, a full clean rebuild, three consecutive full test-suite runs, and a line-by-line cross-check of `docs/ROADMAP.md`/`CHANGELOG.md`/`docs/adr/`/`docs/api/openapi.yaml` against the actual shipped code (every documented endpoint matches `daemon/src/main.c`'s dispatch table exactly; zero drift found).

#### Fixed
- **Three independent, byte-for-byte-identical name-charset validators**: `main.c`'s `name_is_valid()`, `network.c`'s `network_name_is_valid()`, and `pkg.c`'s `pkg_name_is_valid()` each carried their own copy of the same `[A-Za-z0-9_-]`/non-empty/length-limit logic, differing only in which length constant they checked. Extracted into a single `static inline simple_name_is_valid(name, max_len)` in a new `daemon/include/namecheck.h`; each of the three call sites is now a one-line wrapper delegating to it. `dns_name_is_valid()` was left untouched — it validates a genuinely different charset (dots allowed, RFC 1035 hostname rules) and was never part of this duplication.
- **`copy_file()` duplicated across `test/test_overlay.c` and `test/test_image_fixture.c`**: `test_image_fixture.c`'s own header comment already documented this as "the same content test_overlay.c pioneered for its own lowerdir" — an acknowledgment, in the code itself, that `test_overlay.c` (Phase 2, predates `test_image_fixture.c`) was never migrated onto the shared staging module `test_image_fixture.c` (Phase 4) was built to consolidate onto. Exported it as `test_image_fixture_copy_file()` in `test/test_image_fixture.h`/`.c`; `test_overlay.c` now includes the header and calls the shared function, its own copy deleted. `Makefile`'s `test_overlay` build rule gained `test/test_image_fixture.c` as a build input (a pure, dependency-free utility file — no new coupling introduced).
- Two other flagged candidates were investigated and confirmed **not** to be violations, left as-is: `pkg.c`'s `run_subprocess()` vs. `pki.c`'s `run_openssl()` capture the child's stdout/stderr through a pipe for parsing (PKI needs to read back `openssl`'s output; `run_subprocess()` only needs an exit code) — genuinely different capabilities, not the same logic twice. `test_pkg.c`'s `run_cmd()` (shell-based, `system()` + format string, used only for building test fixtures) vs. `test_pki.c`'s `run_openssl_argv()` (direct `execve()` with an argv array, no shell) likewise solve different problems with deliberately different mechanisms.

#### Verified (no code change required)
- Git state: clean, fully pushed, no stashes, no dangling branches, no stray backup/patch files anywhere in the tree.
- File mtimes match commit history exactly — no edit was ever made outside the normal commit flow.
- Full clean rebuild (`rm -rf build && make`): zero warnings across every one of the 19 build targets.
- All 13 test binaries (`test_toolchain` through `test_pkg`) re-run 3 consecutive times, back to back: 100% pass, identical results each run.
- `docs/ROADMAP.md`, `docs/adr/README.md` (all 14 ADRs present and indexed, no orphans), `docs/api/README.md`, and `docs/api/openapi.yaml` all verified accurate against the live code — no stale claims found.
- Root `README.md` was the one stale document found: its Status table still showed Phase 3 as "Next" and Phases 4–10 as "Not started," and its Repository Layout section predated the `daemon/`/`client/`/`cli/`/`web/`/`netplane/` directories entirely (both frozen at Phase 2). Rewritten to reflect all 10 completed phases and the current directory layout.

### Phase 10 (part 2): dependency resolution + explicit upgrades

#### Added
- `daemon/src/pkg.c`: `pkg_install_start()` gained an `upgrade` parameter and now resolves the full install order via a recursive DFS over `pkg_depends` (post-order, already-installed dependencies skipped, cycle detection against the current resolution path) before forking anything — all local recipe-file I/O, no async need for resolution itself. A `pkg_depends` name with no matching recipe, or a circular dependency, is a `400`, same class as an unparseable recipe.
- `pkg_build_completed()` gained a chaining contract: on a successful build, if more packages remain queued, it starts the next one's fetch itself and returns `1` with the new pid/pidfd for the caller to track — `handle_container_event()`'s existing unconditional call just started reacting to a return value, no new call site. `pkg_install_start()` gained an `out_started_name` parameter so `POST /v1/pkg/install`'s `202` response honestly describes whichever package actually started fetching first (a dependency, not necessarily the requested name).
- Upgrades: the already-installed short-circuit is now gated on `upgrade` (still `409` unless true and the recipe's version genuinely differs). `start_fetch_for()` deliberately leaves an in-place upgrade's existing `version`/`files` untouched until the new build actually succeeds; only then are the OLD manifest's files unlinked and the new ones merged and recorded -- a failed upgrade attempt reverts to `PKG_STATE_INSTALLED` (the old, never-touched version) instead of ending `FAILED` with an orphaned, untracked binary still sitting in the base image.
- `write_pkg_json()` gained `"available_version"` (`null`, or the recipe's current version if it differs from what's installed) — re-read fresh from the recipe file on every response, the concrete "is this out of date" answer.
- `docs/api/openapi.yaml` + `docs/api/README.md`: `PkgInstallRequest.upgrade`, `PkgEntry.available_version`, worked dependency-chain and upgrade examples.
- `cli/src/main.c`: `kanxeoctl pkg install --name=NAME [--upgrade]`; `fmt_pkg_line()` gained an available-version column. `web/`: install form gained an "Upgrade" checkbox; packages table gained an Available column.
- `test/test_pkg.c`: a `top`/`leaf` dependency pair installed via one `POST` (both reach `installed`); a circular pair → `400`, confirmed never registered; a missing-dependency recipe → `400`; an upgrade scenario where `GET` shows `available_version` live before upgrading, a plain re-`POST` stays `409`, `{"upgrade": true}` proceeds, and the resulting binary's real stdout is checked against the *new* fixture's expected output (proof of an actual rebuild, not a relabeled stale cache).

#### Changed
- `docs/ROADMAP.md`: Phase 10 marked done.

#### Fixed
- **Reused `__pkgbuild` container upperdir was never cleared between builds** — a chained dependency's `pkg-dest` could silently inherit leftover files from whatever built there previously (a latent gap present since part 1, never triggered by a test that only ever built one package per daemon lifetime). Fixed with `reset_build_container_dir()` (a real `rm -rf` subprocess, not a hand-rolled recursive delete) called before every build's prep. Caught during this part's own manual verification, before the automated test was written.
- The upgrade-completion failure-handling gap described above (an upgrade attempt that fails partway would otherwise orphan a still-working, still-physically-present old install as untracked and unremovable) — also caught and fixed during manual verification.

## Phase 10 (parts 1–2): package manager — source-based, sandboxed, asynchronous installs, with dependency resolution and upgrades

### Phase 10 (part 1): package manager — source-based, sandboxed, asynchronous installs

#### Added
- `daemon/include/pkg.h` + `daemon/src/pkg.c`: a source-based package manager. Recipes are shell scripts (`pkg_name=`/`pkg_version=`/`pkg_source=`/`pkg_sha256=`/`pkg_depends=` metadata, `pkg_build()`/`pkg_install()` shell functions) — confirmed with the user, the recipe format and "builds run in our own container runtime" split were both explicit up-front decisions, mirroring DNS/PKI's "own code hand-rolled, real software as workloads" precedent. The daemon never sources/executes a recipe on the host: metadata is read with a strict, non-executing line scanner; only the isolated, network-less build container ever runs a recipe's real shell code.
- **Async pipeline, tracked entirely via `pidfd`+`epoll` (no blocking request handler for a network fetch or a real compile)**: `POST /v1/pkg/install` forks+execve's `curl` on the host (a new `CONN_PKG_FETCH` reactor kind; `sys_pidfd_open()` added to `linux_compat.h` for tracking a plain `fork()`'d subprocess the same way containers already get a pidfd for free from `CLONE_PIDFD`), verifies the download against `pkg_sha256` via the real `sha256sum`, then builds it inside a single reserved container (`__pkgbuild`, v1 serializes to one install at a time) on a new sandboxed `pkgbuild` image with no network access (this project's networking plane has no outbound NAT, and fetching already happens on the host, so the build container needs none). `handle_container_event()` gained an unconditional `pkg_build_completed()` call, mirroring `dns_record_forget_owner()`/`pki_cert_forget_owner()`'s existing shape.
- `POST /v1/pkg/bootstrap`: stages a real build toolchain (`gcc`/`make`/`ld`/`as`/`cc1`/`sh`/`tar`) from the daemon's own host into the `pkgbuild` image by copying whole `/usr/{include,lib,lib64,bin,libexec}` directories with the real `cp -a`, plus this host's own `bin`/`lib`/`lib64`/`sbin` -> `usr/...` compatibility symlinks — verified empirically via `chroot` compile+link+`make install DESTDIR=` before any daemon code was written. Idempotent; ~1.5s measured on this host.
- Every installed package lands in one canonical image, `/var/lib/kanxeo/images/base/rootfs` — any container built on `"image": "base"` gets everything installed, the thing that makes "the same 100% for host and containers" true.
- `docs/api/openapi.yaml` + `docs/api/README.md`: `/pkg/bootstrap`, `/pkg/recipes`, `/pkg/install`, `/pkg`, `/pkg/{name}` paths and schemas, with a worked example and the async model documented explicitly (not hidden behind a fake-synchronous API).
- `cli/src/main.c`: `kanxeoctl pkg bootstrap/recipes/install/ls/rm`, mirroring the `pki`/`dns` subcommand families. `web/`: "Packages: Recipes" and "Packages: Installed" dashboard panels (bootstrap button, install form, live-polled state table).
- `test/test_pkg.c`: hermetic (a `file://` URL against a tiny synthetic C fixture the test stages itself — the real `curl` subprocess path is genuinely exercised, not mocked, while staying offline-safe). Proves the full fetch → checksum verify → isolated build → merge pipeline by actually executing the installed binary from the base image and checking its real output; the `409` serialization boundary via a real overlapping-in-time request; a checksum-mismatch recipe ending in `failed`; `DELETE` actually removing the manifested file from the base image, confirmed via `stat()`.

#### Changed
- `docs/ROADMAP.md`: Phase 10 marked in progress (part 1 done).

#### Fixed
- A real use-after-free in `test/test_pkg.c`'s own `poll_pkg_state()`: `kx_response_free(&r)` (which frees `r.json`) was called before `strcmp()`-ing a `state` pointer that pointed into that same freed tree, causing unpredictable early-exit/misreported state. Caught by the test's own first real run behaving unexpectedly; fixed by comparing against a stable, already-copied buffer instead of the dangling pointer.

## Phase 9 (parts 1–2): PKI/certificate management — a root CA, issued leaf certificates, and automatic per-container issuance

### Phase 9 (part 2): automatic per-container TLS cert issuance + delivery

#### Added
- `daemon/include/pki.h` + `daemon/src/pki.c`: `struct pki_cert_record` gained `owner_container` (empty for a manually-created cert, a container's name for one it auto-issued), mirroring `dns_record`'s Phase 8 part 2 field. `pki_cert_create()` gained an `owner_container` parameter; new `pki_cert_forget_owner(container_name)` deletes a container's own cert on its deletion (via the existing `pki_cert_delete()`, one deletion path not two), but only if the cert's owner actually matches. `write_cert_json()` gained an `"owner"` field.
- New `pki_cert_deliver(name, pid, dest_dir)`: writes an already-issued cert's `.crt`/`.key` into `/proc/<pid>/root/<dest_dir>/tls.{crt,key}` (chmod 0600 on the key) — `dns_server_register()`'s `/proc/<pid>/root/` pattern (ADR-0013) getting its second real consumer. One-time delivery, no live resync (unlike DNS server bindings) since a cert doesn't change after a container starts.
- `POST /v1/containers` gained `pki_issue` (bool), `pki_cert_dir` (string, default `/etc/kanxeo-tls`), `pki_days` (int, default 365). Unlike `dns_register`, does not require `networks` (the cert's identity is the container's name, not its IP); does require the CA to already be bootstrapped, validated upfront as a `400` — a name collision discovered only at issuance time stays best-effort/skip-and-log instead, matching `dns_register`'s exact asymmetry.
- `cli/src/main.c`: `kanxeoctl run --pki-issue [--pki-cert-dir=PATH] [--pki-days=N]`; `fmt_pki_cert_line()` gained an owner column. `web/`: run form gained an "Issue TLS cert" checkbox; PKI Certificates table gained an Owner column.
- `docs/api/openapi.yaml` + `docs/api/README.md`: `ContainerCreateRequest.pki_issue`/`pki_cert_dir`/`pki_days`, `PkiCert.owner`, and a worked auto-issuance example.
- `test/test_pki.c`: `pki_issue` before CA bootstrap → `400`; an auto-issued cert's owner confirmed via `GET`, then its delivered files read directly via `/proc/<pid>/root/` (chmod 0600 on the key) and cryptographically verified against the CA; a same-named manually-created cert confirmed to survive a colliding container's deletion (the ownership check, not just "delete by name") alongside the positive case (an owned cert does disappear when its container is deleted).

#### Changed
- `docs/ROADMAP.md`: Phase 9 marked done.

#### Fixed
- A stray misplacement in `docs/ROADMAP.md`: Phase 8's own closing "not designed yet" note had been left orphaned after Phase 9's section (a side effect of how Phase 9 part 1's content was originally inserted) rather than staying inside Phase 8's own section. Caught while editing this same file for part 2; moved back into place, no content lost or duplicated going forward.

### Phase 9 (part 1): PKI/certificate management — a root CA and issued leaf certificates

#### Added
- `daemon/include/pki.h` + `daemon/src/pki.c`: a single root CA (`POST/GET /v1/pki/ca`) plus leaf certificate issuance (`POST/GET/DELETE /v1/pki/certs`), mirroring `dns.c`'s shape. Actual cryptography (keypair generation, CSR signing) is done by the daemon shelling out to the system's real, unmodified `openssl` binary as a short-lived subprocess — confirmed with the user before any code was written (same question shape as DNS's "why hand-roll?"), keeping ADR-0007's "no third-party dependency footprint in the daemon" intact since exec'ing isn't linking. `dns_name_is_valid()` exported from `dns.h` (was `static` in `dns.c`) and reused for leaf cert names/SANs rather than re-implementing hostname validation.
- On-disk layout under `/var/lib/kanxeo/pki/`: `ca.key` (chmod 0600)/`ca.crt`/`ca.srl`, `certs/<name>.key`(0600)/`certs/<name>.crt` per leaf, plus a `pki_certs.json` metadata index persisted via the existing `daemon/src/persist.c` (Phase 8 part 1's shared module getting its intended second consumer).
- Two deliberate, asymmetric security properties: the CA private key is never returned over the API, in any endpoint, ever; a leaf certificate's private key is returned exactly once, in the `POST /v1/pki/certs` response, never again by any later `GET`/list.
- `docs/api/openapi.yaml` + `docs/api/README.md`: `/pki/ca`, `/pki/certs`, `/pki/certs/{name}` paths and schemas, with a worked example.
- `cli/src/main.c`: `kanxeoctl pki ca bootstrap/show` and `pki cert create/ls/rm`, mirroring the `dns record`/`dns server` subcommand families, with a distinct multi-line PEM formatter for `create`/`show` (a cert/key can't fit the usual single-row table format). `web/`: a "PKI: Root CA" panel (bootstrap status/button) and a "PKI: Certificates" panel (issuance form + table + a one-time key/cert reveal).
- `test/test_pki.c`: real cryptographic verification (`openssl verify -CAfile`, SAN round-trip via `openssl x509 -noout -ext`) as the actual proof, not a string check; the "shown once" key guarantee asserted directly (list/get responses confirmed to carry no `key_pem` field); on-disk file deletion confirmed via `stat()`, not just the index entry; restart-survival for both the CA and the cert index.

#### Changed
- `docs/ROADMAP.md`: Phase 9 marked in progress (part 1 done).

#### Fixed
- `handle_pki_ca_get()`'s `PKI_ERR_NOT_BOOTSTRAPPED` mapping: the shared `respond_pki_error()` maps it to `400` (correct for `POST /v1/pki/certs`'s "you can't do this yet" precondition), but `GET /v1/pki/ca` on a not-yet-bootstrapped CA needs `404`, matching every other single-resource `GET` in this API. Caught before writing the automated test, once the test's own planned verification made the inconsistency obvious; fixed with an explicit special case rather than threading endpoint context through the shared error mapper.

### Phase 8 (parts 1–2): DNS records + containerized dnsmasq resolution + automatic container registration

### Phase 8 (part 2): automatic container DNS registration

#### Added
- `daemon/include/dns.h` + `daemon/src/dns.c`: `struct dns_record` gained `owner_container` (empty for a manually-created record, a container's name for one it auto-registered). `dns_record_create()` gained an `owner_container` parameter; new `dns_record_forget_owner(container_name)` deletes a container's own record on its deletion, but only if the record's owner actually matches — a safe no-op otherwise, called unconditionally alongside the existing `dns_server_forget()`. `dns_write_json_one()` gained an `"owner"` field (`null` or the owning container's name).
- `POST /v1/containers` gained an optional `dns_register` boolean: on success, best-effort registers a record named after the container pointing at its primary network's IP (`400` if set with no `networks`). Reuses the existing `dns_record_create()` -> `dns_server_sync_all()` path, so an auto-registered record reaches an already-registered dnsmasq container exactly like a manual one, live, with no new sync code.
- `cli/src/main.c`: `kanxeoctl run --dns-register`; `dns record ls` output gained an owner column. `web/`: run form gained a "Register DNS name" checkbox; DNS Records table gained an Owner column.
- `docs/api/openapi.yaml` + `docs/api/README.md`: `ContainerCreateRequest.dns_register`, `DnsRecord.owner`, and a worked auto-registration example.
- `test/test_dns.c`: `dns_register` without `networks` -> `400`; an auto-registered record's name/IP/owner confirmed via `GET`, then confirmed to actually resolve via `dig` against the already-running dnsmasq container; a same-named manually-created record confirmed to survive a colliding container's deletion (proving the ownership check, not just "delete by name") alongside the positive case (an owned record does disappear when its container is deleted).

#### Changed
- `docs/ROADMAP.md`: Phase 8 marked done.

### Phase 8 (part 1): DNS records + containerized dnsmasq resolution

#### Added
- `daemon/include/dns.h` + `daemon/src/dns.c`: DNS records as a REST resource (`POST/GET/DELETE /v1/dns/records`, name -> IPv4) mirroring `network.c`'s shape, persisted to `/var/lib/kanxeo/dns_records.json`. A second, independent piece in the same file: DNS server bindings (`POST/GET/DELETE /v1/dns/servers`) registering a running container to keep synced — in-memory only, unlike records, since a binding references a container's pid and containers don't survive a daemon restart either.
- `daemon/include/persist.h` + `daemon/src/persist.c`: `persist_atomic_write()`/`persist_read_file()` extracted from `network.c`'s `save_state()`/`load_state()` (otherwise `dns.c` would duplicate the identical logic) plus a new `persist_mkdir_p()`. `network.c` refactored to use these; re-verified against `test_networks.c`'s restart-survival scenario.
- `docs/adr/0013-proc-pid-root-for-live-container-file-writes.md`: `/proc/<pid>/root/<path>` (not raw upperdir, not `setns()`) is now the standing pattern for the daemon to read/write a specific running container's filesystem from outside it — the original upperdir-write design was verified empirically to not work (the kernel documents this as unsupported for an already-mounted overlay) before anything was built on top of it.
- `test/test_image_fixture.c` gained `test_image_fixture_add_lib()` — stages one additional shared library at its real absolute path, needed for a real (not hand-rolled) dnsmasq binary's ~20-library dependency closure, well beyond the usual ld.so+libc pair every other exec target in this project needs.
- `test/test_dns.c`: records CRUD + validation, a real dnsmasq container registered as a DNS server and queried with the host's own `dig` (genuine protocol resolution), a live record update proving the `SIGHUP`-reload path (not just the initial snapshot), and DNS-server-binding cleanup on container deletion.
- `cli/src/main.c`: `kanxeoctl dns record create/ls/rm` and `dns server register/ls/unregister`, mirroring the `network` subcommand family. `web/`: matching DNS Records and DNS Servers dashboard sections.

#### Changed
- `docs/ROADMAP.md`: Phase 8 marked in progress (part 1 done).
- `web/style.css`: form styling generalized from `#run-form`-scoped rules to `.panel form` — fixes a latent gap from Phase 7 part 1 (the Networks form was never actually getting the intended styling) while adding two more forms, rather than leaving three unstyled forms instead of one.

#### Fixed
- **The original "write to a running container's upperdir" design didn't work at all**: verified empirically before building on it (a file written directly into a running container's upperdir from the host never appeared in that container's mounted view, even after correctly pre-creating the parent directory) — the kernel documents modifying the upper layer of an already-mounted overlay as unsupported/undefined. Replaced with `/proc/<pid>/root/<path>`, confirmed to work correctly via the same kind of live test. See ADR-0013.
- **dnsmasq failed to start in a minimal container image, three times over, each caught and fixed before declaring this part done**: no `/dev/urandom` for RNG seeding (fixed with real `mknod` device nodes, `1,9`/`1,3`); `-u root`/default group still perform a real NSS lookup even for the account already running as, with no `/etc/passwd`/`/etc/group` at all (fixed with a minimal two-line version of each, plus explicit `-g root`); default pidfile path `/var/run/dnsmasq.pid` with no `/var/run` (fixed with `-x` pointed at the already-created `/etc`).
- **A real use-after-free** in `handle_dns_server_create()`: `json_free(root)` was called before reading `container_name`/`hosts_path` — pointers into the now-freed tree — to build the success response, producing garbled output. Caught via manual smoke-testing before the automated test was even written; fixed by building the response before freeing, the same ordering `handle_create()`'s own existing comment already documents for a different field.

## Phase 7 follow-up: raise the per-container network cap

#### Changed
- `include/container.h`: `CONTAINER_MAX_NETWORKS` raised from `4` to `64`, matching `daemon/include/network.h`'s `NETWORK_MAX` — a container can never attach to more networks than could possibly exist, so that's the real ceiling, not an arbitrary round number picked without justification. `cli/src/main.c`'s mirrored `CLI_MAX_NETWORKS` raised to match. `docs/api/openapi.yaml`'s `ContainerCreateRequest.networks.maxItems` and the daemon's validation error message updated accordingly.
- Deliberately still a fixed, bounded array (not switched to dynamic/heap allocation) — consistent with every other bounded table in this codebase (`REGISTRY_MAX_CONTAINERS`, `NETWORK_MAX`, `argv_buf[64]`). A cap that can't realistically be hit isn't the same problem as no cap at all.

## Phase 7 (parts 1–3): dynamic networks, multi-homed containers, IP forwarding + static routes — the container router

### Phase 7 (part 3): IP forwarding + static routes

#### Added
- `include/container.h`: `struct route_spec` (`dest_be`, `dest_prefix_len`, `gateway_be`) + `CONTAINER_MAX_ROUTES` (`8`); `container_spec` gains `ip_forward` and `routes[]`/`route_count`.
- `netplane/`: `rtnl_route_add_ipv4(fd, dest_be, dest_prefix_len, gateway_be)` generalizes the existing default-route-only helper — `rtnl_route_add_default_ipv4()` is now a one-line wrapper around it, so there's still exactly one implementation that builds an `RTM_NEWROUTE` message.
- `src/container_net.c`: `container_net_install_routes()` and `container_net_enable_ip_forward()` — both run in the child after interface setup, needing no pipe synchronization (only the child's own already-established netns is touched), both following the exact same `perror()`+`_exit(126)` failure contract every other post-barrier child-side step already uses.
- `daemon/src/main.c`: `handle_create()` parses `ip_forward` (bool) and `routes` (0-8 `{dest, prefix_len, via}` entries, format-validated: well-formed IPv4, prefix `0-32`). `registry_entry` gains `ip_forward` (routes themselves are deliberately not stored/echoed back — a stated scope boundary, not a gap).
- `docs/api/openapi.yaml`: `ContainerCreateRequest.ip_forward`/`routes` (new `RouteSpec` schema); `Container.ip_forward`.
- `cli/src/main.c`: `kanxeoctl run --ip-forward` and repeatable `--route=DEST/PREFIX:VIA`; `fmt_container_line()` shows `fwd=yes/no`.
- `web/`: an IP forwarding checkbox and comma-separated static-routes field on the create form; a Fwd column on the containers table.
- `test/net_connect.c`: a new exec target that connects *out* to a given IP (unlike `net_child`, which only listens) — needed because proving packets are forwarded through a third container requires the connection to originate from *inside* the calling container's own netns/routing table, not the host's (every other connectivity check in this project connects from the host, which isn't a router in these tests and would prove nothing about forwarding).
- `test/test_container_net.c` gained a real 3-container router topology (R on two networks with `ip_forward` on; H and T each with a static route via R for the other's subnet, including T's *reply* route — the real asymmetric-routing case that makes this a meaningful test) proving genuine L3 forwarding through R's kernel routing table. `test/test_daemon_net.c` gained the same topology driven entirely over real HTTP; `test/test_cli.c` gained a scenario confirming `--ip-forward`/`--route=` are plumbed through by the real binary.

#### Changed
- `docs/ROADMAP.md`: Phase 7 marked **done** (all three parts). No new ADR — the route-primitive generalization and the set-once-at-creation scope boundary are consistent extensions of ADR-0011, not a new durable architectural stance.

#### Fixed
- **`test_daemon_net.c`'s router scenario had T and H's gateway IPs swapped in the first draft**: each container's static route must go via the router's IP on *that container's own* subnet (a gateway has to be directly reachable on one of the container's own connected subnets), not the router's IP on the far side. Caught immediately by the test itself (`ENETUNREACH` from `container_net_install_routes()`), fixed, and re-verified before moving on — not left to surface later.

### Phase 7 (part 2): multi-homed containers

#### Added
- `include/container.h`: `struct container_spec.net` (singular) replaced by `nets[CONTAINER_MAX_NETWORKS]` + `net_count` — a container can now attach to up to 4 networks at creation; `net_count == 0` is exactly the pre-existing isolated-netns behavior, unchanged.
- `src/container_net.c`: `container_net_host_setup()`/`container_net_child_configure()` loop over the attachment array (one `rtnl_open()` for the whole loop). Veth naming gains an index suffix (`vh<pid>-<idx>`/`vc<pid>-<idx>`, one pair per attachment); the child renames each to `eth<idx>` instead of a hardcoded `eth0`. Only the first attachment ("primary") gets the default route — the rest get their subnet's connected route automatically from the address assignment, no extra syscall needed.
- `daemon/include/registry.h`: new `struct registry_network_attachment` (name + ip); `registry_entry`'s single `ip_be`/`network` fields replaced by `nets[CONTAINER_MAX_NETWORKS]` + `net_count`. `registry_network_in_use()`/`registry_alloc_ip()` generalized to scan the array; `registry_alloc_ip()`'s own contract is otherwise unchanged.
- `daemon/src/main.c`: `handle_create()` parses a `"networks"` array (1–4 entries) instead of a singular `"network"` string; allocates one IP per entry (no rollback needed on a partial failure, since allocation was already a pure scan with nothing reserved out-of-band).
- `docs/api/openapi.yaml`: `ContainerCreateRequest.networks` (array, 1–4 items) replaces `network`; `Container.networks` (array of `{name, ip}`) replaces the singular `network`/`ip` fields.
- `cli/src/main.c`: `--network=NAME` is now **repeatable** (same flag, appends to the request array); `fmt_container_line()` renders every attachment (`networks=name:ip,name:ip` or `-`).
- `web/`: the create form's Network field accepts a comma-separated list, split client-side into the request array; the containers table's IP column renders every attachment.
- `test/net_child.c` gained an optional argv connection-count (default `1`, every existing caller unchanged) so proving connectivity to a multi-homed container from more than one of its networks didn't need a second, near-duplicate exec target.
- `test/test_container_net.c` gained a scenario: one container on two bridges at once, real TCP connectivity to both independently-addressed interfaces. `test/test_daemon_net.c` and `test/test_cli.c` gained matching multi-network scenarios over real HTTP and the real `kanxeoctl` binary respectively.

#### Changed
- `docs/ROADMAP.md`: Phase 7 marked in progress (parts 1–2 done); part 3 (IP forwarding + static routes) still explicitly not designed, per Zen.
- API shape: this is the **second** time a networking field has changed shape across two phases (singular → array, following Phase 6 part 3 → Phase 7 part 1's `enum:[default]` → free-form string) — each one lifting a stated v1 limitation from the phase before it, not an unplanned break.

### Phase 7 (part 1): dynamic, REST-managed networks

#### Added
- `daemon/include/network.h` + `daemon/src/network.c`: a real `Network` resource (`POST/GET/DELETE /v1/networks`) — `struct network_def` (name, subnet, prefix length, derived gateway) in a fixed-size table, mirroring `registry.c`'s existing shape. `network_alloc_ip()` delegates straight to the existing, already topology-agnostic `registry_alloc_ip()` (Phase 6 part 3) — no changes needed there.
- The project's **first durable host-state persistence file** (ADR-0012), `/var/lib/kanxeo/networks.json`: unlike containers (safe to be in-memory-only, since they die with the daemon), a bridge created via this API outlives the process, so it's written atomically (temp file, `fsync`, `rename()`) on every create/delete and reloaded (bridges recreated idempotently, `EEXIST` tolerated) at startup.
- `daemon/include/registry.h`/`.c`: `struct registry_entry` gains `network` (the attached network's name, recorded directly) so `network_delete()` can refuse removal while a container is still attached.
- Validation in `network_create()`: unique 1–15 char name (`IFNAMSIZ` — the name *is* the bridge's ifname), subnet's host bits must be zero for the given prefix, prefix length in `[8,30]`, no overlap with any existing network's range. Gateway is always the subnet's first host address, never independently settable.
- `cli/src/main.c`: `kanxeoctl network create/ls/rm`, mirroring the container subcommands exactly.
- `web/`: a Networks section (table + create form + remove button), same pattern as the containers section.
- `test/test_networks.c`: create/list/get, full validation coverage (duplicate/misaligned/overlapping/out-of-range), delete-while-in-use refusal, and a restart-survival proof (create a network, restart the daemon, confirm the API and the underlying bridge both still show it without a second create call — the actual reason the persistence file exists).

#### Changed
- `docs/ROADMAP.md`: Phase 7 marked in progress (part 1 done); parts 2 (multi-homed containers) and 3 (IP forwarding + static routes) explicitly not designed yet, per Zen.
- `daemon/src/main.c`: the old hardcoded `DEFAULT_BRIDGE`/`ensure_default_network()`/single-fixed-subnet machinery (Phase 6 part 3) is gone, replaced entirely by the dynamic network table. `ContainerCreateRequest.network` is no longer `enum: [default]` — any name created via `POST /v1/networks` is valid.
- `test/test_daemon_net.c` and `test/test_cli.c`: their networking scenarios now create their own network via the new API first, since there is no more free-standing "default" to assume.
- `test/test_net_cleanup.c`/`.h` **removed**: the retrying `kanxeo0` cleanup it existed for (Phase 6 part 3) is now dead code — nothing creates a bridge unconditionally at daemon startup anymore, so `test_daemon.c`/`test_web.c`/`test_cli.c` no longer need it either. The class of bug it was written to paper over goes away by construction, not by another patch.

#### Fixed
- **`network_create()` always failed with a `500`, rolling back (deleting) the bridge it had just successfully created.** Root cause: `save_state()` compared the number of bytes written against `w.len` *after* calling `jw_free(&w)`, which resets `len` to `0` — so the check always saw a mismatch on an actually-successful write. Fixed by capturing the length before freeing the writer. Caught by `test_networks.c`'s very first assertion, before this part was ever declared done.

## Phase 6 (parts 2–3): wired into containers, exposed through the daemon/API/CLI/dashboard

### Phase 6 (part 3): exposed through the daemon/API/CLI/dashboard

#### Added
- `daemon/include/registry.h`/`.c`: `struct registry_entry` gains `ip_be`; `registry_create()` takes it as a construction parameter (set atomically, not poked in afterward — closes off a stale-IP-from-a-reused-slot footgun); new `registry_alloc_ip(network_base_be, host_min, host_max, *out_ip_be)`, kept topology-agnostic (subnet passed in, no hardcoded network knowledge in the registry).
- `daemon/src/main.c`: `ensure_default_network()` — creates bridge `kanxeo0` (`172.30.0.0/24`, gateway `.1`) at startup, tolerating `EEXIST` on both the bridge and the address assignment, same idempotent-restart pattern as the existing `ensure_dir()` calls. `handle_create()` gains an optional `"network"` field (`"default"` only in v1; anything else → `400`), allocates an IP, and populates `struct network_spec` from Part 2.
- `docs/api/openapi.yaml`: `ContainerCreateRequest.network`, `Container.ip` (nullable).
- `cli/src/main.c`: `kanxeoctl run --network=default`; `ip=` shown in `ps`/`inspect` output.
- `web/index.html`/`app.js`: a Network field on the create form, an IP column in the container table.
- `test/net_child.c` + `test/test_daemon_net.c`: end-to-end verification over real HTTP — two containers on `"default"` get distinct real IPs with real TCP connectivity to each, `GET /v1/containers` reflects both correctly, omitting `network` still yields `ip:null` (explicit regression check), an unsupported network name is `400`.
- `test/test_cli.c`: a new scenario driving the real `kanxeoctl` binary with `--network=default`, checking its output shows a real `ip=`.
- `test/test_net_cleanup.c`/`.h`: shared `test_cleanup_bridge()` — retries deleting a bridge (up to 20×, 100ms apart) rather than a single attempt, since netns/veth teardown runs on a kernel workqueue and can lag briefly. Used by every test that transitively starts a real `kanxeod`, since `ensure_default_network()` now creates `kanxeo0` as a side effect of every daemon startup.

#### Changed
- `docs/ROADMAP.md`: Phase 6 marked done (all three parts).

#### Fixed — test-hygiene bug, not a bug in the shipped daemon/library
- `test/test_rtnetlink.c` (Part 1) started hanging intermittently. Root cause: it creates its own standalone bridge on the same `172.30.0.0/24` subnet that `ensure_default_network()` now uses for `kanxeo0`, and `kanxeo0` left behind by a prior `test_daemon.c`/`test_cli.c`/`test_web.c`/`test_daemon_net.c` run (each of which starts a real `kanxeod`, and so silently creates `kanxeo0`, whether or not that specific test exercises networking) collided with it. Fixed by giving all four of those tests the same retrying cleanup via the new shared `test_cleanup_bridge()`, rather than four separate copies of the same retry loop.
- `test/test_overlay.c` (Phase 2), found while stress-running the full suite repeatedly to confirm the above fix: it reuses fixed `/tmp/overlay_test/...` paths across runs (needed for its host-side "did a previous container's write leak here" assertions) but never cleared them first, so a second invocation always failed against the first's leftover `status.txt`. Fixed with a small `nftw()`-based `rm_tree()` at the start of `build_lowerdir()`. Unrelated to networking, but blocked exactly the repeated-run stress-testing discipline ADR-0008/ADR-0009 established, so fixed alongside this phase's other test-hygiene work rather than left for a future phase to rediscover.

### Phase 6 (part 2): wired into the real container lifecycle

#### Added
- `include/container.h`: `struct network_spec` on `container_spec` — opt-in (`bridge == NULL` means exactly today's isolated-netns behavior, unchanged; `test_harness.c`/`test_overlay.c` needed zero source changes, confirmed by re-running them).
- `src/container_net.c` (+ declarations in `include/internal.h`): `container_net_host_setup()`/`container_net_child_configure()`, following the existing one-file-per-concern pattern (`mountns.c`, `overlay.c`).
- `netplane/`'s `rtnl_link_rename()` — needed to rename a container's veth end to `eth0`; the one operation that must identify its target by ifindex rather than `IFLA_IFNAME`, since that attribute means "set this as the new name" here.
- A synchronization barrier in `container_create()` (a `pipe()` inherited across `clone3()`) so the parent can move a veth into the child's netns before the child touches it. The container-side veth's name is sent *through* that same pipe, not recovered via `getpid()` in the child — `CLONE_NEWPID` means the child sees itself as pid 1 in its own namespace, so it can't know the name the parent used.
- `test/net_child.c` + `test/test_container_net.c`: end-to-end verification through the real `container_create()` (not the raw rtnetlink primitives) — two containers, concurrently, on the same bridge, with real simultaneous TCP connectivity to each, and confirmation that the kernel auto-removes both veth ends once each container exits.
- `test/test_image_fixture.c`/`.h` gained a third parameter (destination basename) so `test_container_net.c` could reuse it for `net_child` instead of writing a second, near-duplicate staging function.

#### Changed
- `docs/ROADMAP.md`: Phase 6 marked in progress (parts 1–2 done); part 3 (daemon bridge lifecycle + IP allocation + REST/CLI/dashboard exposure) still explicitly deferred, per Zen.

#### Fixed / real-world kernel behavior learned
- `container_create()`'s failure path when the parent's host-side network setup fails: closes the pipe's write end *without writing*, so the already-blocked child's `read()` sees EOF and fails cleanly (`_exit(126)`) instead of hanging forever; the parent then reaps it and reports the whole call as failed, same contract every other early-return in that function already honors.
- Network namespace (and veth) teardown runs on a kernel workqueue, not synchronously with the exiting process being reaped — an interface can briefly still exist right after `waitid()` returns. `test_container_net.c` polls for its disappearance (same pattern as every other "wait for something async" check in this project) after an initial run caught exactly that race.

## Phase 5 + Phase 6 (part 1)

### Phase 6 (part 1): rtnetlink primitives

#### Added
- `netplane/`: `rtnetlink.h`/`rtnetlink.c` — hand-built netlink messages (no `ip`/iproute2, no OVS, no eBPF) for bridge creation, veth pair creation, moving a link into another process's netns, bridge attachment, IPv4 addressing, default routes, and link deletion. See ADR-0011 for the "custom control plane over the kernel's own bridge, not a userspace switch" decision, and the bridge-vs-port addressing pitfall found while building this.
- `test/test_rtnetlink.c`: end-to-end verification including real TCP connectivity through a constructed bridge+veth+netns topology, not just successful syscalls.
- `docs/adr/0011-rtnetlink-control-plane-over-kernel-bridge.md`.

#### Changed
- `docs/ROADMAP.md`: Phase 6 marked in progress (part 1 done); part 2 (wiring this into real containers, the REST API, the CLI, and the dashboard) explicitly scoped as separate, not-yet-started follow-up work, per Zen.

#### Verification note
- Before writing `rtnetlink.c`, per ADR-0008's lesson, confirmed via a throwaway `sizeof`/`offsetof` check that TCC lays out every kernel-ABI struct this module touches (`nlmsghdr`, `ifinfomsg`, `ifaddrmsg`, `rtmsg`, `rtattr`, `nlmsgerr`) identically to GCC, and that the relevant `<linux/*.h>` headers don't conflict with glibc's own networking headers — verified, not assumed.

### Phase 5: Web dashboard, pure REST API client

#### Added
- `web/`: the dashboard itself (`index.html`, `app.js`, `style.css`) — vanilla HTML/CSS/JS, no framework, no build step (ADR-0010). Health indicator, auto-refreshing container table, create-container form, remove button per row — mirrors `kanxeoctl`'s exact command surface.
- `daemon/src/staticfile.c` + `include/staticfile.h`: `static_serve()` — path-traversal-safe static file serving, content-type by extension. `kanxeod` gains a `--web-root` flag (default `web/`) and serves the dashboard same-origin as the API.
- `daemon/include/http.h`'s `http_set_blocking()`, promoted from `main.c`'s private `make_blocking()` so `staticfile.c` and `main.c`'s `respond_json()` share one implementation.
- `client/include/httpclient.h`'s `struct kx_response` gained `content_type`/`body`/`body_len` — needed to verify non-JSON (static file) responses.
- `test/test_web.c`: end-to-end verification of static serving (status/content-type per asset, 404, path-traversal rejection, no regression in `/v1/...` routing).
- `docs/adr/0010-vanilla-web-dashboard-no-build-step.md`.

#### Changed
- `docs/ROADMAP.md`: Phase 5 marked done.

#### Scope boundary (stated, not silently skipped)
- Whether the dashboard renders and behaves correctly in a real browser wasn't automated-tested — no headless-browser/Node toolchain exists in this project, and adding one for one small dashboard would repeat the exact dependency-cost trade-off ADR-0010 decided against. Checked instead: `node --check` (pre-installed system tool, not a new project dependency) on `app.js`, and every DOM ID it references confirmed present in `index.html`. A real browser check at `http://127.0.0.1:7620/` is the remaining step.

## Phase 4: CLI, pure REST API client

### Added
- `client/`: `kx_client_request()` — a reusable HTTP client library for talking to the Kanxeo API, extracted from `test_daemon.c`'s ad hoc socket code so `kanxeoctl` and the test suite share one implementation instead of two. Its response buffer is dynamically-growing (no fixed size cap), unlike the test code it replaced.
- `cli/`: `kanxeoctl` — the CLI, one subcommand per endpoint (`health`, `ps`, `run`, `inspect`, `rm`), zero direct runtime access. `--json` for raw output; formatted text by default. Exit codes: `0`/`1`/`2` (success/API-or-transport-failure/usage-error).
- `test/test_image_fixture.c`: shared test-image staging, factored out of `test_daemon.c` so `test_cli.c` doesn't duplicate it.
- `test/test_cli.c`: end-to-end verification driving the real `kanxeoctl` binary as a subprocess.
- `docs/adr/0009-cloexec-daemon-fds-before-clone3.md`.

### Changed
- `test/test_daemon.c`: refactored to use `client/src/httpclient.c` and `test/test_image_fixture.c` instead of its own duplicated logic — same 9 assertions, no behavior change.
- `docs/ROADMAP.md`: Phase 4 marked done; locked-in decisions gained the CLOEXEC rule.

### Fixed
- **Every `POST /v1/containers` request silently stalled for as long as the created container ran** (discovered as `test_daemon.c` taking ~30s instead of milliseconds, under the same repeated-run stress-testing discipline that caught the Phase 3 `epoll_event` bug). Root cause: none of the daemon's own fds (listening socket, accepted client sockets, `epoll` fd, cgroup `O_PATH` fd) were `CLOEXEC`, so every `clone3()`'d container inherited a duplicate of the triggering client connection, and the kernel withholds EOF on a TCP connection until every reference to it — across every process — closes. Fixed by creating every one of those fds `CLOEXEC` from the start (`SOCK_CLOEXEC`/`EPOLL_CLOEXEC`/`O_CLOEXEC`). See ADR-0009. Verified by timing: ~30s → ~0.3s per `test_daemon.c` run.

## Phase 3: REST API spec + daemon

### Added
- `docs/api/openapi.yaml`: the versioned REST API contract (health check, container CRUD), written before any handler code — see ADR-0006.
- `daemon/`: the REST daemon (`kanxeod`) — a hand-rolled, single-threaded, `epoll`-driven HTTP/1.1 + JSON reactor with an in-memory container registry, implementing that contract. See ADR-0005 and ADR-0007.
- `struct kx_epoll_event` / `kx_epoll_ctl()` / `kx_epoll_wait()` in `include/linux_compat.h`.
- `test/test_daemon.c` + `test/daemon_child.c`: end-to-end verification driving the daemon over real HTTP.
- `docs/adr/`: Architecture Decision Records (0000–0008), covering every significant decision made so far, not just this phase's.
- `docs/api/README.md`: human-oriented quick reference alongside the authoritative OpenAPI spec.
- This file.

### Changed
- `CLAUDE.md`: added the API-First Mandate (the daemon is the only process with direct runtime access; CLI/web are pure API clients) and a Documentation Map.
- `docs/ROADMAP.md`: Phase 3 re-scoped from "minimal container CLI" to "REST API spec + daemon," and Phases 4/5 split into CLI and web dashboard, both now depending on Phase 3 instead of the runtime library directly.
- `include/linux_compat.h`: all epoll usage across the daemon now goes through `kx_epoll_event`, never the system `struct epoll_event`.

### Fixed
- **`epoll_ctl()`/`epoll_wait()` silently corrupting their `data` field under TCC**, causing an intermittent (~50–60% of runs) segfault on the very first request. Root cause: TCC ignores `__attribute__((packed))` entirely, so the kernel's 12-byte `struct epoll_event` ABI was being compiled as 16 bytes. See ADR-0008.

## Phase 2 — OverlayFS root construction

### Added
- `docs/MISSION.md` (verbatim charter), `docs/ROADMAP.md` (phase-by-phase status), `CLAUDE.md` (auto-loaded project instructions).
- `struct overlay_spec` + `overlay_create()` (`include/container.h` / `src/overlay.c`): real OverlayFS layering (lowerdir/upperdir/workdir), replacing Phase 1's bind-mount-of-live-host-root. See ADR-0004.
- `mountns_make_private()`, split out of `mountns_pivot()` so mount-propagation is privatized before the overlay mount, not after.
- `test/test_overlay.c` + `test/overlay_child.c`.
- `README.md`.

### Changed
- `struct mount_spec`: `root_source` removed (folded into `overlay_spec.merged`) to eliminate a duplicate-state footgun.
- `mountns_pivot()` no longer populates the root itself; it only requires the root already be a mount point.
- `test/test_harness.c` updated to route through `overlay_create()` (`lowerdir="/"`, preserving its original full-host-visible behavior) and to read its result file from upperdir's copy-up path instead of the bare host path.

## Phase 0 + Phase 1 — Toolchain smoke test, namespace + cgroup v2 container harness

### Added
- `test/test_toolchain.c`: confirms TCC compiles and dynamically links against host glibc, and every header family later phases need parses clean. See ADR-0001.
- `include/container.h`, `include/linux_compat.h`, `include/internal.h`, `src/cgroup.c`, `src/ns_create.c`, `src/mountns.c`, `src/container.c`: the container runtime library — `clone3`-based namespace creation with atomic cgroup v2 placement, mount-namespace pivot, pidfd-based reaping. See ADR-0002, ADR-0003.
- `test/test_harness.c` + `test/harness_child.c`.
- `Makefile`, `.gitignore`, initial repository scaffold.

### Notes
- Requires a **privileged** LXC container/host to run — an unprivileged nested LXC blocks the final `mount("proc", ...)` with `EPERM`/"VFS: Mount too revealing" regardless of mount flags or namespace combination tried (confirmed by elimination during this phase).
