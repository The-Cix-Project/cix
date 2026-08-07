# Changelog

All notable changes to this project are recorded here. Format is loosely [Keep a Changelog](https://keepachangelog.com/)-style, adapted for a rolling-release OS built phase by phase rather than a semantically-versioned library: entries are grouped by roadmap phase (see `docs/roadmap/ROADMAP.md`), newest first, with no `[Unreleased]`/version-numbered sections — every entry here is already committed (some tagged: `v1.0.0` closed Phase 0-10, `v1.1.0` closed Phase 11 parts 1-4, `v1.2.0` closed Phase 11 part 5, `v1.3.0` closed Phase 30 part 5 (a prior documentation audit), `v1.4.0` closed Phase 40 part 2 (ADR-0056), `v1.5.0` closed Phase 40 part 3 (ADR-0057) plus this full documentation audit; untagged phases in between are untagged but no less real). This file is updated as part of every meaningful change, not as an afterthought — see `CLAUDE.md`'s Documentation Map.

### Part 5 follow-on: REST-driven ISO assembly, closing the API-First Mandate gap (ADR-0064)

Closes the one honest gap Part 5 itself flagged: `image/src/mkinstalleriso.c` was a dev-machine-only tool, not reachable via REST/CLI. Requested explicitly, with an explicit instruction that this project's capabilities be API-driven "with no exceptions unless explicitly called out for valid reasons."

#### Added
- `pkg/recipes/isotools.recipe` -- a hostbuild aggregator (not a fifth from-source build): copies the already-installed `grub-mkrescue`/`sbsign`/`sbverify`/`xorriso`/`mformat`/`mcopy` binaries, grub's own real 601-file `x86_64-efi` module tree, and their confirmed real shared-library closure into a single, self-contained, portable artifact. Verified: every harvested binary runs correctly under `env -i` with an explicit `ld.so --library-path` invocation, no inherited host environment at all.
- `pkg/recipes/openssl-dev.recipe` -- real gap found while re-verifying `kanxeo.recipe`'s own hostbuild: `kanxeod` has linked `-lssl -lcrypto` since the HTTPS listener (ADR-0059), but no recipe had ever staged the link-time `libssl.so`/`libcrypto.so` dev symlinks, so the self-hosted round-trip had been silently broken since HTTPS landed. Mirrors `libc-dev.recipe`'s own "stage the real host copy, real tarball for provenance" shape.
- `daemon/src/main.c`: `CONN_ISO_ASSEMBLE` conn kind, `iso_build_start()`/`handle_iso_assemble_event()` (fork+`pidfd_open()`+epoll, non-blocking, mirroring `spawn_kanxeo_bootroot_assembly()`), `POST`/`GET /v1/system/iso`. New `SIGNING_KEYS_DIR` (`<data-dir>/keys/`, operator-populated out of band -- deliberately never generated/fetched/copied by `kanxeod` itself) and `ISO_DIR` (`<data-dir>/iso/`).
- `cli/src/main.c`: `kanxeoctl iso build [--disk=... --ip=... --prefix=... --gateway=... --interface=...] [--wait]` / `kanxeoctl iso status`.
- `docs/adr/0064-rest-driven-iso-assembly.md`.

#### Changed
- `pkg/recipes/kanxeo.recipe`: the same hostbuild round now also builds `kanxeo-install`/`mkinstalleriso`, not just `kanxeod`/`kanxeoctl`/`mkbootroot`.
- `image/src/mkinstalleriso.c`: `grub-mkrescue`/`sbsign` paths are no longer hardcoded `/usr/bin/...` -- a new required `isotools-root` argument computes them, plus grub's own `--directory=`/`--xorriso=` flags and a `PATH`/`LD_LIBRARY_PATH` `setenv()` for `grub-mkrescue`'s own un-overridable `mformat`/`mcopy` child fork/execs. A bare `/usr` still reproduces the tool's original host-borrowed dev-machine behavior.
- `docs/api/openapi.yaml`/`docs/api/README.md`/`docs/guides/building-kanxeo.md`: document the new endpoint, the `isotools`/`openssl-dev` recipes, and the updated `kanxeo-builder` minimum package set.
- `pkg/recipes/README.md`: recipe count updated 50 -> 52.

#### Notes
- Verified real, end to end: `isotools` hostbuilt from a real `dev` image; `kanxeo.recipe`'s hostbuild re-run for real through a genuine fetch+extract+build+harvest round (surfacing and fixing the `openssl-dev` gap); the parameterized `mkinstalleriso` producing a real bootable ISO standalone; the full `POST /system/iso` -> poll -> `GET /system/iso` round-trip against a live daemon, with custom `--disk=`/`--ip=`/`--prefix=`/`--gateway=`/`--interface=` values confirmed present verbatim in the resulting ISO's own `grub.cfg`; and the `kanxeoctl iso build --wait`/`iso status` CLI surface.
- **Not verified, per Zen:** a real hardware/QEMU boot of a REST-produced ISO specifically (Part 5's own ADR-0063 already established grub-mkrescue's output boots correctly in general). Downloading the finished ISO over REST was deliberately not built -- `iso_path` stays a host filesystem path, the same convention `GET /pkg/hostbuild/{name}`'s own `artifact_path` already established.

### Part 5 (core toolchain): ISO self-build -- a real, from-source GRUB2/xorriso/mtools/sbsigntools toolchain (ADR-0063)

Nine new real, from-source recipes closing the "largest, most uncertain part" of the bare-metal-readiness plan, all verified end to end against the real daemon pipeline.

#### Added
- `pkg/recipes/patch.recipe`, `pkg/recipes/gettext.recipe` -- real GRUB2 build-time dependencies (confirmed against GRUB's own upstream `INSTALL` file).
- `pkg/recipes/gnu-efi.recipe` -- needed by sbsigntools, not GRUB2 (GRUB2 has its own self-contained in-tree EFI headers).
- `pkg/recipes/xorriso.recipe`, `pkg/recipes/mtools.recipe` -- real *runtime-only* dependencies of `grub-mkrescue` (confirmed via its own source: invoked by literal name via fork/exec, never linked), not build dependencies of GRUB itself.
- `pkg/recipes/libuuid.recipe` -- util-linux's own real `--disable-all-programs --enable-libuuid` standalone build.
- `pkg/recipes/binutils-dev.recipe` -- new package splitting binutils' dev files (`bfd.h` and friends) out of `binutils.recipe`'s own runtime-only install, mirroring `libc.recipe`/`libc-dev.recipe`'s existing split.
- `pkg/recipes/sbsigntools.recipe` -- canonical upstream confirmed as James Bottomley's fork (`git.kernel.org/.../jejb/sbsigntools.git`, tag `v0.9.5`); no release tarball exists, fetched as a real git-archive snapshot; its vendored CCAN git submodule spliced in via a second multi-source entry; `autogen.sh` replicated directly (two of its steps need a live `.git` history a tarball fetch doesn't have).
- `pkg/recipes/grub.recipe` -- GRUB 2.14, a single `./configure --target=x86_64 --with-platform=efi` pass builds everything (no second "target platform" tree needed, contrary to the plan's own earlier speculation); BIOS/`i386-pc` deliberately skipped (Kanxeo is UEFI-only, ADR-0031).
- `docs/adr/0063-iso-self-build-toolchain.md`.

#### Changed
- `daemon/src/pkg.c`: `pkg_build_completed()` now also merges every ordinary install's own output into the shared build sandbox (`g_pkgbuild_rootfs`), not just the target image -- a genuine architectural gap found and fixed, not routed around. Every ordinary package build previously ran inside one fixed, shared toolchain sandbox that recipe-installed packages never became part of, so a package installed via this project's own recipe mechanism was permanently invisible to any *other* recipe's own build step, even a formally-declared `pkg_depends`. Masked until now because every prior cross-recipe dependency happened to already exist on this sandbox's real host OS.
- `pkg/recipes/README.md`: recipe count updated 41 -> 50.

#### Notes
- Verified real, end to end: all 9 recipes installed successfully through the real daemon pipeline (3 real build failures found and fixed along the way, each root-caused via the real build's own failure output, never guessed).
- **A real, complete, self-built ISO was produced and a real Secure Boot signature verified**, using *only* the freshly pkg-installed toolchain: `grub-mkrescue` produced a genuine, valid, bootable ISO 9660 image; `sbsign` (Kanxeo's own real signing key) signed a real EFI binary; `sbverify` confirmed the signature.
- **Not verified, per Zen:** the daemon-side, REST-triggered ISO-assembly mechanism (a `CONN_BOOTROOT_ASSEMBLE`-shaped addition mirroring ADR-0057's `mkbootroot`) remains a distinct, deliberately deferred follow-on -- `image/src/mkinstalleriso.c` is still a dev-machine-only manual tool, not yet reachable via REST/CLI.

### Part 4: disk quotas -- real ext4 project-quota enforcement (ADR-0062)

Real, kernel-enforced per-container disk-usage limits -- nothing enforced this before (`overlay_upperdir_size()`, ADR-0054, only ever reported usage).

#### Added
- `image/kernel/qemu-part1.config`: `CONFIG_QUOTA=y`, `CONFIG_QFMT_V2=y`.
- `image/src/kanxeo-install.c`: `mkfs_ext4()` gains a `with_quota` parameter -- `mkfs.ext4 -O quota -E quotatype=prjquota` on the containers partition only (verified empirically: both feature flags land correctly in a real test filesystem image).
- `include/container.h`: `struct overlay_spec.project_id` (0 = no quota tagging).
- `include/linux_compat.h`: `struct kx_fsxattr` + `KX_FS_IOC_FSGETXATTR`/`KX_FS_IOC_FSSETXATTR`/`KX_FS_XFLAG_PROJINHERIT` -- declared here rather than `<linux/fs.h>` directly, which clashes with glibc's own `<fcntl.h>` (confirmed: `SYNC_FILE_RANGE_WRITE_AND_WAIT` defined differently by each). Ioctl numeric values and `struct fsxattr`'s real 28-byte size verified empirically, not hand-computed and trusted blind.
- `src/overlay.c`: `overlay_create()` performs real `FS_IOC_FSSETXATTR` project-id tagging (with `FS_XFLAG_PROJINHERIT`) right after `mkdir(ov->upperdir, ...)` -- fails loud on error, unlike this codebase's usual best-effort cgroup-enablement posture.
- `daemon/src/quotamap.c`/`.h`: new, small, persisted name-to-project-id module (`quotamap_get_or_assign()`) -- deliberately separate from the in-memory-only `registry.c`.
- `daemon/src/main.c`: `resolve_backing_device()` (real `/proc/mounts` longest-match resolution, the `findmnt`/`df` algorithm) and `set_disk_quota()` (real `quotactl(2)` `Q_SETQUOTA`); `create_container_from_body()` parses `disk_quota_bytes`.
- `cli/src/main.c`: `kanxeoctl run --disk-quota=BYTES`.
- `docs/api/openapi.yaml`/`docs/api/README.md`/`docs/guides/cli-reference.md`: document the new field/flag.
- `docs/adr/0062-ext4-project-disk-quotas.md`.
- `test/test_disk_quota.c`: real HTTP end-to-end test -- `disk_quota_bytes` parsing, `quotamap_get_or_assign()`'s real persisted allocation (same name -> same id across two requests, verified by reading the real state file; different name -> distinct id), correct `500` propagation when the backing filesystem lacks project-quota support, no container left behind after a failed quota create, and zero regression for ordinary no-quota creation.

#### Notes
- Project ids are deliberately never reclaimed or reused, even after `DELETE` -- `DELETE /v1/containers/{name}` does not remove the container's own upperdir at all (ADR-0054's pre-existing backup/restore design), so the quota data those files count against must keep pointing at the same id indefinitely.
- Full clean rebuild (zero warnings); fast regression set green (24 daemon-linked/unit tests including the new one), 3 consecutive clean runs.
- **Not verified, per Zen:** a real write past the quota genuinely failing with `EDQUOT` -- this sandbox has no loop devices and no raw block-device access, so a real quota-enabled ext4 mount cannot be exercised here at all (confirmed: `FS_IOC_FSSETXATTR` against this sandbox's own real root ext4 returns `EOPNOTSUPP`, the correct behavior for a filesystem never given the quota feature). The same class of gap already documented for Part 3's kernel-module boot test and Part 5's ISO self-build. `CONFIG_QUOTA`/`CONFIG_QFMT_V2` deferred to the same combined rebuild+boot cycle as Parts 2/3's own kernel config additions.

### Part 3: kernel module loading (ADR-0061)

The largest, most novel part of the bare-metal-readiness plan -- real target hardware needs broader driver coverage than this project's fixed QEMU/VirtIO set, but the no-initramfs constraint (ADR-0014) means whichever controller could hold root can never be a loadable module.

#### Added
- `pkg/recipes/kmod.recipe`: real, unmodified upstream kmod 34.2 (modprobe/depmod/insmod/lsmod/modinfo/rmmod).
- `pkg/recipes/kernel.recipe`: `pkg_build()` now runs `make bzImage modules` in a single invocation (see Notes); `pkg_install()` runs `make INSTALL_MOD_PATH=$PKG_DESTDIR modules_install` + a real, explicitly-version-pinned `depmod -b $PKG_DESTDIR "$(make -s kernelrelease)"`.
- `image/kernel/qemu-part1.config`: root-critical storage block (`CONFIG_BLK_DEV_NVME=y`, `CONFIG_BLK_DEV_MD=y` + RAID0/1/10/456, `CONFIG_SCSI=y`, `CONFIG_BLK_DEV_SD=y`, alongside the already-`=y` `CONFIG_SATA_AHCI`); loadable-module block (`CONFIG_MODULES=y`, `CONFIG_E1000E`, `CONFIG_IGB`, `CONFIG_IXGBE`, `CONFIG_R8169`, `CONFIG_TIGON3` [module `tg3.ko`], `CONFIG_USB_EHCI_HCD`, `CONFIG_USB_STORAGE`, all `=m`).
- `image/src/mkbootroot.c`: two new optional (`""` = skip) trailing args, `modules-dir` and `kmod-bin-dir`, mirroring `firmware-dir`'s (ADR-0029) tolerant-default shape.
- `test/test_image_fixture.c`/`.h`: `test_image_fixture_copy_dir_recursive()` (real `cp -a`), needed because a `.ko` tree nests and the existing `copy_dir_files()` is flat-only.
- `daemon/src/main.c`: `load_boot_modules()`, a curated, best-effort `modprobe` of the network/USB driver list above, called from `main()` before `bootstrap_management_network()` (that function needs the kernel to have already detected the named interface, which requires its driver already loaded) and not from inside `boot_init()` (stays purely about mounts, per Part 0.5's established boundary).
- `docs/adr/0061-kernel-module-loading.md`.
- `test/test_mkbootroot_firmware.c`: new scenario proving the two new `mkbootroot` args' staging (real recursive nested-directory copy + real symlink-dereferencing flat copy landing a working `modprobe`).
- Six test files' (`test_boot.c`, `test_boot_ab.c`, `test_console_shell.c`, `test_console_pki_bootstrap.c`, `test_console_pkg_bootstrap.c`, `test_installer.c`) `mkbootroot` argv literals extended with the two new trailing `""` args.

#### Notes
- Getting a real, end-to-end kernel-with-modules hostbuild to succeed took eleven attempts against this project's own long-lived "dev" build image -- ten distinct, previously-unexposed gaps found and fixed one at a time via the real build's own failure output (full list in ADR-0061): `sed`, `grep`, `bc`, `diffutils`, `findutils`, `gzip` were never installed at all; `bison`/`bash`/`libc-dev` needed reinstalling to pick up fixes that postdate this image; `zlib`/`elfutils` are new dependencies `CONFIG_MODULES=y` itself introduces via `objtool`; and a genuine `kernel.recipe` bug -- building `bzImage` and `modules` as two separate `make` invocations left modpost unable to resolve kernel-exported symbols against a missing `vmlinux.o`, fixed by combining both into one `make bzImage modules` invocation.
- Verified real, end to end: the 11th hostbuild attempt produced a genuine `bzImage` plus 18 real `.ko` files (all curated drivers plus real dependencies like `mdio-bus`/`libphy`) with correct `depmod` metadata. That artifact's `lib/modules/` tree plus a real extracted `kmod`-tools directory were fed through a real `mkbootroot` invocation, confirming the assembled staging root contains the identical module tree and seven real, independent, correctly-dereferenced kmod tool binaries.
- Broadcom `bnx2` deliberately excluded from the module list -- it cannot function at all without a firmware blob, unlike the others here; staging `linux-firmware` blobs is separate, unstarted scope.
- **Not verified, per Zen:** a real QEMU boot with `load_boot_modules()` actually running against a real assembled squashfs -- deliberately deferred and batched with Part 2's and Part 4's own kernel config additions into one combined rebuild+boot cycle. Real bare-metal driver behavior can't be fully proven by QEMU alone regardless.

### Part 2: CPU affinity (`cpuset`) (ADR-0060)

A new mechanism, not just plumbing -- `cpuset.cpus` didn't exist anywhere in this codebase before.

#### Added
- `include/container.h`: `struct cgroup_limits.cpuset_cpus`; `src/cgroup.c`'s `cgroup_create()` writes it to `cpuset.cpus` when given, mirroring `cpu_max`'s own conditional-write shape.
- `src/cgroup.c`: `cgroup_enable_cpuset()`, mirroring `cgroup_enable_io_accounting()` line for line -- best-effort `+cpuset` written to the cgroup v2 root's `cgroup.subtree_control`, called once at daemon startup.
- `image/kernel/qemu-part1.config`: `CONFIG_CPUSETS=y`.
- `daemon/src/main.c`: `create_container_from_body()` parses an optional `cpuset_cpus` string field, same shape as `cpu_max`.
- `cli/src/main.c`: `kanxeoctl run --cpuset=0-1,3`, next to `--cpu-max=`.
- `docs/api/openapi.yaml`/`docs/api/README.md`/`docs/guides/cli-reference.md`: document the new field/flag.
- `docs/adr/0060-cpu-bandwidth-and-affinity-api-exposure.md` (covers Part 1 and Part 2 together).
- `test/test_container_lifecycle.c`: generalized `read_cgroup_cpu_max()` into `read_cgroup_value(name, file, ...)`; new step proving a real `cpuset_cpus` value lands in the container's actual `cpuset.cpus`.

#### Notes
- Full clean rebuild (zero warnings); fast 23-binary regression set green, 3 consecutive clean runs of the new test step. Also manually verified through the real `kanxeoctl --cpuset=` flag against a live daemon, including confirming `cpuset` actually appears in the root's own `cgroup.subtree_control` after startup.
- `CONFIG_CPUSETS=y` has not yet been verified by an actual kernel rebuild + boot test -- deliberately deferred and batched with Part 3's and Part 4's own kernel config additions, rather than three separate rebuild cycles. This dev sandbox's own (non-Kanxeo-built) kernel already has cpuset support, which is what the live verification above actually exercised.

### Part 0.5 follow-up: `daemon-config` CLI + web dashboard

Closes the CLI/web gap Part 0.5 itself flagged when it shipped -- every other daemon capability in this project ships CLI+web alongside its REST endpoint.

#### Added
- `cli/src/main.c`: `kanxeoctl daemon-config show`/`daemon-config set [--port=N] [--https-port=N] [--enable-http] [--disable-http] [--enable-https] [--disable-https] [--management-network=NAME]`. Unlike `site set`, the PUT is a genuine server-side partial update, so only the fields actually given are sent -- no fetch-then-merge needed.
- `web/index.html`/`web/app.js`: new "Daemon" leaf under System > Backup, `view-daemon-config` with port/https-port/enabled-toggle/management-network fields, refreshed on the same poll-loop + dirty-flag pattern `refreshSiteConfig()` already established. The management-network `<select>` only offers `has_gateway` networks. Changing the port or management network shows a `confirm()` first, since this dashboard's own requests are relative to the page's own origin and either change disconnects the page the instant it takes effect.
- `docs/guides/cli-reference.md`/`docs/guides/web-dashboard.md`/`docs/guides/installing.md`: document the new CLI subcommand and dashboard panel.

#### Fixed
- CLI's `--management-network=` flag parsing used an off-by-one prefix length (22 instead of the real 21 characters), so the flag was never recognized at all -- found by running the command against a live daemon and getting "unknown option" instead of the expected 404 for a bogus network name.

#### Notes
- Verified: `kanxeoctl daemon-config show`/`set` against a real daemon, including a full live repoint to a real network confirming the daemon actually rebinds, and the `--enable-https`-with-no-PKI 500 path. Dashboard verified via `node --check`, an HTML tag-balance check, and confirming a real daemon serves the edited files with `GET /v1/system/daemon-config` reachable underneath -- no real browser click-through was possible in this sandboxed environment (no headless browser tooling available), so a genuine visual/interactive check is still owed.

### Part 1: `cpu.max` API/CLI wiring

The mechanism already existed end to end in the runtime library (`struct cgroup_limits.cpu_max`, written by `src/cgroup.c`) but was hardcoded `NULL` in the daemon, with no API/CLI surface at all.

#### Added
- `daemon/src/main.c`: `create_container_from_body()` parses an optional `cpu_max` string field, mirroring the existing `memory_max`/`pids_max` block exactly -- a raw cgroup-native `"<quota> <period>"` value, passed straight through, not reinterpreted.
- `cli/src/main.c`: `kanxeoctl run --cpu-max="QUOTA PERIOD"`, next to `--memory-max=`/`--pids-max=`.
- `docs/api/openapi.yaml`/`docs/api/README.md`/`docs/guides/cli-reference.md`: document the new field/flag.
- `test/test_container_lifecycle.c`: new step creating a container with a real `cpu_max`, then reading `/sys/fs/cgroup/<name>/cpu.max` directly to confirm the kernel-authoritative value matches (this daemon doesn't echo `cpu_max`/`memory_max`/`pids_max` back in any response).

#### Notes
- Full clean rebuild (zero warnings); 23-binary fast regression set (QEMU-boot-based tests excluded -- this change touches neither install nor boot code) all green, 3 consecutive clean runs of the new test step. Also manually verified through the real `kanxeoctl --cpu-max=` flag against a live daemon.
- `GET .../stats` still has no `cpu.max`-reporting field -- `cpu_max` stays create-time-only, matching `memory_max`/`pids_max`'s own existing scope.

### Part 0.5: host management networking unified into a real, API-managed network, plus a live-reconfigurable daemon port and an OpenSSL-backed HTTPS listener (ADR-0058, ADR-0059)

First step of the bare-metal-readiness effort. Real-world use of a freshly rebuilt/boot-tested installer ISO on the user's own test VM surfaced a genuine design gap: `kanxeod`'s own management IP was a one-shot, GRUB-boot-parameter-driven `rtnetlink` call straight against the physical NIC, invisible to `GET /networks` and un-repointable without a reinstall.

#### Added
- `kanxeo-install.c`: new `--interface=<name>` flag, threaded into `net.conf`. `main()` bootstraps a reserved `mgmt` network at first boot via the existing `network_create()`/`network_attach_interface()` mechanism, replacing `apply_static_ip()` (removed from `boot_init()`, which stays purely about mounts).
- `network.h`/`network.c`: `network_def.is_management` field, `network_set_management()`/`network_find_management()`. `network_delete()`/`network_detach_interface()` unconditionally refuse (`409`, `NETWORK_ERR_IS_MANAGEMENT`) while set on the target network -- no override/force flag.
- New `daemon_config` module + `GET`/`PUT /v1/system/daemon-config`: listen port, HTTP/HTTPS toggles, and the management-network repoint operation -- each a live, in-process listen-socket rebind (new socket bound + added to `epoll` before the old one is torn down), persisted only once the real change succeeds.
- `daemon/src/tlsconn.c`/`tlsconn.h`: a small, additive fd -> `SSL*` side table, letting `http_write_response()`, the WebSocket frame sender, and the `main.c` read loop become TLS-aware without threading TLS through this daemon's ~100 request handlers.
- `daemon/src/main.c`: a second, independent HTTPS listener (`CONN_LISTENER_TLS`), reusing the PKI-issued `"host"` leaf certificate. Non-blocking `SSL_accept()` driven step-by-step from the existing `epoll` loop (`EPOLLOUT` widened/narrowed on `WANT_WRITE`); an explicit `SSL_pending()` re-check-and-loop after every read handles OpenSSL's own internal buffering correctly.
- `Makefile`: `-lssl -lcrypto` linked into `kanxeod`.
- `docs/adr/0058-host-management-network-unification.md`, `docs/adr/0059-openssl-https-listener.md`.

#### Fixed
- `g_tls_conns[]`'s BSS zero-init meant every slot's `.fd` started at `0`, not the intended `-1` "free" sentinel -- `tls_register()` silently registered nothing, and TLS connections silently fell back to raw, undecrypted wire bytes. Fixed with an explicit `tls_init()` setting every slot to `-1`, called once early in `main()`.
- `rebind_https_listener()` was missing the no-op short-circuit its HTTP counterpart already had, causing a spurious `EADDRINUSE` on any `daemon-config` PUT touching an unrelated field (e.g. only `http_enabled`) while HTTPS was already bound to the same address:port.

#### Notes
- Verified: full daemon-linked regression suite (24 binaries) green; a real manual end-to-end HTTPS check (`curl`/`openssl s_client` against the PKI host cert, live HTTP/HTTPS independent toggling); the definitive `test_installer` QEMU run, including a second, fully independent reboot within the same test, confirming `mgmt`/`daemon_config`/TLS-listener state persists correctly across a real reboot.
- No CLI or web dashboard surface exists for `daemon-config` yet -- a real, tracked gap, not a violation of the API-First Mandate (which only requires the REST endpoint to exist first).
- Parts 1-5 of the bare-metal-readiness plan (`cpu.max`, `cpuset`, kernel modules, disk quotas, ISO self-build) not started. A separate, previously reported dashboard-reachability bug (unrelated to this phase) remains open.

### Phase 40 part 3: `tcc.recipe`, `kanxeo.recipe`, server-side `mkbootroot` assembly -- the self-hosting plan closes (ADR-0057)

Third and final part: rebuilding `kanxeod`/`kanxeoctl`/`web` from inside Kanxeo's own container+recipe mechanism, with TCC (never GCC, per this project's own Immutable Maxim) doing the building.

#### Added
- `pkg/recipes/tcc.recipe`: ordinary recipe, installs real upstream tinycc 0.9.27, patches a real confirmed upstream bug (`lib/bcheck.c`'s glibc `__malloc_hook` reference, removed in glibc 2.34) with a version-guarded `sed` patch matching real downstream distro practice.
- `pkg/recipes/kanxeo.recipe`: `pkg_source` is Kanxeo's own self-hosted gitea archive-download endpoint, pinned to a real annotated tag (`v1.4.0`), authenticated via a scoped read-only access token embedded in the URL (the committed recipe carries only a placeholder, never a real token). Also builds+stages `build/mkbootroot` itself, alongside `kanxeod`/`kanxeoctl`/`web/`.
- `daemon/src/pkg.c`: `pkg_build_completed()` gains a new `out_hostbuild_done_name` out-parameter, populated only when a hostbuild job just completed -- keeps `pkg.c` itself agnostic to what any package name means.
- `daemon/src/main.c`: new `CONN_BOOTROOT_ASSEMBLE` conn kind, forks+execs+pidfd-tracks `mkbootroot` server-side exactly like the existing `CONN_PKG_FETCH` pattern, triggered when the hostbuild-done name is `"kanxeo"` -- the CLI never invokes `mkbootroot` itself, per the API-First Mandate.
- `cli/src/main.c`: `--deploy` support for `name=="kanxeo"` (second honest explicit case, alongside `kernel`'s) -- reads `kanxeod-root.squashfs` and calls the existing, unmodified `cmd_update()`.
- `docs/adr/0057-self-hosted-toolchain-and-control-plane-rebuild.md`.

#### Fixed
- `libc-dev.recipe` now stages a second copy of the six crt startup objects (`crt1.o`/`crti.o`/`crtn.o`/`Scrt1.o`/`gcrt1.o`/`Mcrt1.o`) at `/usr/lib/x86_64-linux-gnu`. Root cause: TCC maintains a separate, single-path search list for CRT objects, defaulting to that exact path -- confirmed against vanilla upstream tinycc source, distinct from GCC's own `LIBRARY_PATH` convention the files were originally staged for at `/lib/x86_64-linux-gnu`.
- `pkg_seed_image_baseline()`'s `runtime_libs[]` table (`daemon/src/pkg.c`) now stages a second copy of `ld-linux-x86-64.so.2` at `/lib/x86_64-linux-gnu/`. Root cause: glibc >= 2.34's own `libc.so.6` carries a `DT_NEEDED` entry on `ld-linux-x86-64.so.2` itself, which TCC resolves through its general library-search list, not the separate path the real dynamic loader lives at (`/lib64`).
- `kanxeo-builder`'s minimal image needed `bash` and `coreutils` in addition to `tcc`/`make`/`libc-dev`, each found by a real build failure (`execve failed` with no `/usr/bin/bash`; `mkdir: No such file or directory` for the root `Makefile`'s own `mkdir -p build`).
- This sandbox's own `/etc/ssl/certs/ca-certificates.crt` was stale relative to an individually-present CA cert -- `git`'s GnuTLS-backed fetches worked, plain `curl`'s OpenSSL-backed ones didn't. Fixed, with explicit user approval, by appending the missing cert to the bundle.

#### Notes
- Proven with one real, live, end-to-end operational round trip against a scratch, `--data-dir=`-isolated verify daemon -- no automated test can safely exercise "rebuild the box running the test suite with itself." `pkg hostbuild kanxeo --build-image=kanxeo-builder --wait` fetched the real tagged, token-authenticated archive, built `kanxeod`/`kanxeoctl`/`mkbootroot` entirely with the just-installed TCC (zero warnings, confirmed via the daemon's own log) and reached `state=installed`; the async `mkbootroot` assembly step logged `kanxeo bootroot assembly: succeeded`; the resulting `kanxeod-root.squashfs` confirmed via `file` to be a genuine, valid squashfs image.
- The final deploy-and-reboot step against a real box was not performed this phase -- a live, consequential action deliberately left for an operator to trigger explicitly. `GET /v1/health` still has no build-identifying field.

### Phase 40 part 2: hostbuild artifact mechanism, plus a real from-scratch Linux 6.18.40 kernel built end-to-end (ADR-0056)

Second part of the self-hosting plan: build a standalone artifact (starting with the kernel `kanxeod` boots) using Kanxeo's own container+recipe mechanism, without merging the output into any image. A second mode of the existing `pkg install` pipeline, reusing its state machine/fetch/build-container machinery almost entirely unmodified.

#### Added
- `pkg_hostbuild_start()` (`daemon/src/pkg.c`): build container's lowerdir is a named, real image's own rootfs (`--build-image=`) instead of the shared toolchain sandbox; completion harvests output into `ARTIFACTS_DIR/<name>/` instead of merging into any image. Storage reuses `pkg_find()`'s existing per-`(name, image)` slots via a reserved sentinel image name, `PKG_HOSTBUILD_IMAGE = "__hostbuild"`. A hostbuild recipe must have empty `pkg_depends`.
- `POST /v1/pkg/hostbuild`, `GET /v1/pkg/hostbuild/{name}` (the latter a thin wrapper over the existing `pkg_get_one()`); `kanxeoctl pkg hostbuild <name> --build-image=<image> [--wait] [--deploy]`.
- New base-tool recipes, each a real gap found by a real build failing at that exact point: `sed`, `grep`, `diffutils`, `bc`, `elfutils` (`libelf`), `zlib`, `findutils` (`xargs`), `gzip`.
- `test/test_pkg.c` step 17: a trivial (non-kernel) hostbuild fixture proving artifact landing on disk, `PKG_ERR_BUSY` symmetry with ordinary installs, `pkg_depends` rejection, unknown-`build_image` rejection.
- `docs/adr/0056-hostbuild-artifact-mechanism.md`.

#### Fixed
- `pkg_entry_add_file()` had no NULL guard; the hostbuild harvest path's `e = NULL` (no manifest needed) would have crashed the daemon the moment any hostbuild's `$PKG_DESTDIR` actually got populated. Latent until now because every earlier attempt failed before reaching that point.
- `start_fetch_for()`'s curl invocation gained `--retry 8 --retry-all-errors --retry-delay 3 -C -` plus an `unlink()` of the destination before each attempt, fixing real, reproducible mid-transfer connection resets on large downloads in this sandbox. Confirmed empirically that plain `--retry` alone doesn't cover a raw connection reset, and that `-C -` without the `unlink()` causes persistent HTTP 416s against a stale, already-complete file from a prior attempt.
- `bash.recipe` now installs a `/bin/sh -> /usr/bin/bash` symlink. Root cause: glibc's `popen()` (used by the kernel's own Kconfig `$(shell ...)` macro evaluation) hardcodes `/bin/sh` with no override mechanism at all; its absence surfaced as a misleading "Cannot allocate memory" rather than "No such file or directory."
- `libc-dev.recipe` now stages `libpthread.a`/`libpthread_nonshared.a` (glibc >= 2.34 dropped the standalone `libpthread.so`, but real build systems still pass `-lpthread` explicitly).
- `bison.recipe`'s `pkg_install()` was deleting its own required runtime data (`usr/share/bison/`) along with genuinely doc-only content -- a real, previously-undetected bug, caught only once something ran bison from a target image's own copy instead of the toolchain sandbox.

#### Notes
- Full clean rebuild (zero warnings), full daemon-linked regression sweep including the new hostbuild fixture, 3 consecutive clean runs. Real-world verification: a genuine, from-scratch Linux 6.18.40 kernel built end-to-end against a real "dev" image, producing a valid `bzImage` (confirmed via `file` and boot-sector magic bytes `55 aa` at offset `0x1fe`) -- not part of the automated suite (too slow to run repeatedly), verified live against a real running daemon instead.
- Part 3 of the self-hosting plan (`tcc.recipe`, `kanxeo.recipe` self-building the control plane, server-side `mkbootroot` assembly) not started.

### Phase 40 part 1: general container file-read REST endpoint (ADR-0055)

First part of a plan aimed at eventual full self-hosting (rebuilding both the kernel and Kanxeo's own control plane entirely on-box). Designing that surfaced a genuinely missing, independently useful capability: Kanxeo's `files[]` mechanism has always been write-only, host-to-container -- there was no way to read a file back out of a container over the API at all.

#### Added
- `GET /v1/containers/{name}/files?path=...` (`handle_container_file_read()`, `daemon/src/main.c`): raw bytes (`application/octet-stream`), not JSON. Running containers read via `/proc/<pid>/root<path>` (the same ADR-0013 pattern `dns.c`/`pki.c` already use); exited-but-not-removed containers fall back to the real overlay upperdir, then the image's own read-only rootfs. Path validation reuses `file_path_is_safe()` verbatim, the same function `files[].path` already validates against.
- `url_query_param()` (`daemon/src/main.c`): this daemon's first query-string parser, deliberately narrow (one key, `%XX`-only decoding).
- `kanxeoctl files get NAME --path=/some/path [--output=PATH]` -- needed no new HTTP client primitive, `kx_client_request()` already captures raw response bytes regardless of Content-Type.
- `test/test_container_files.c` extended with real read-while-running, read-after-exit (both the upperdir and image-rootfs fallback paths), 400-on-traversal, and 404 test cases.
- `docs/api/openapi.yaml`'s new path entry; `docs/adr/0055-container-file-read-endpoint.md`.

#### Fixed
- The new route's suffix-match used `strcmp()` against the container name's own tail, but that name still carries its `?path=...` query string attached -- every request 404'd by falling through to the generic get-one handler until switched to `strncmp()` against just the fixed-length suffix that matters. Caught by actually running the daemon, not by review.

#### Notes
- Full clean rebuild (zero warnings), full 18-test daemon-linked regression sweep, all passing -- no regressions from the shared `main.c` route-dispatch changes. Verified through the real, compiled `kanxeoctl` binary end-to-end (not just the test harness's own HTTP client) against a real daemon and a real running container.

### Phase 39: real, host-side per-container stats -- CPU/memory/disk/network, API-first, plus web dashboard graphs (ADR-0054)

Direct user request: out-of-the-box monitoring, per container, pulled entirely from the host side (no in-container agent), exposed through the REST API first, then graphed in the web dashboard.

#### Added
- `GET /v1/containers/{name}/stats` (`daemon/src/main.c`): a raw, point-in-time snapshot -- cumulative counters (`cpu.*_usec`, `disk.read/write_bytes/ios`, `networks[].rx/tx_bytes/packets`) or gauges (`memory.current/peak/max`, `disk.upper_bytes`), no server-side history. Works for a container that exited on its own, correctly 404s after an explicit `stop` (same `registry_remove()` convention `pause`/`unpause` already use).
- `src/cgroup.c`: `cgroup_read_stat_key()`/`cgroup_read_single_value()`/`cgroup_read_io_totals()` (work directly off the container's already-open `cgroup_fd`), `cgroup_enable_io_accounting()` (best-effort, called once at daemon startup -- the cgroup v2 `io` controller was not enabled anywhere on this platform before this phase).
- `src/overlay.c`: `overlay_upperdir_size()` -- an `nftw()` walk of a container's own overlay upperdir, real content only (excludes directory-tree overhead).
- `kanxeoctl stats NAME` (flat top-level verb, matching `stop`/`start`/`pause`/`unpause`), `docs/api/openapi.yaml`'s new path + `ContainerStats` schema.
- Web dashboard: a 6th "Stats" tab on the container detail view, four hand-rolled `<canvas>` line charts (CPU/Memory/Disk/Network) -- no charting library. `web/app.js` keeps a small client-side rolling window (60 samples) only while the tab is open, computing CPU%/network-KB/s from consecutive raw-sample deltas.
- `test/stats_child.c` + `test/test_container_stats.c`: a real fixture that actively burns CPU, touches memory, and appends to an on-disk file in a loop, proving the returned numbers genuinely advance across two real samples.

#### Fixed
- `stats_child.c`'s first version burned CPU before writing to disk, so an early sample saw genuinely zero disk usage -- fixed by writing first, burning CPU second.
- `test_container_stats.c` never deleted its own `statsnet` bridge, so a second run collided with the first run's leftover bridge on the host (`EEXIST`) -- fixed with the same explicit `DELETE /v1/networks/...` cleanup every other network-creating test already does.

#### Notes
- Full clean rebuild (zero warnings), full 18-test regression sweep, all passing. Verified live against the production daemon: real, moving CPU/memory/network numbers, byte-exact disk-space accounting, and real io-controller-recorded I/O -- confirmed to survive a real daemon restart with the entire live topology auto-recovering, no disruption.
- No real browser click-through of the new Stats tab was possible in this sandboxed dev environment (no headless browser tooling available) -- verified instead via clean JS syntax checks, confirming the daemon serves the exact edited files, and a manual dry-run of the identical CPU%/network-rate math against real live API data. A genuine visual/interactive check is still owed.

### Phase 38: a real platform identity -- domain-based CA naming, an auto-issued host cert, image trust, default name qualification, an auto-maintained DNS record (ADR-0049 through ADR-0053)

Direct follow-up to `instance_name`: the root/intermediate CA's common names should reflect this install's own `domain_suffix`, not the fixed placeholder they were bootstrapped with. No existing mechanism could change an already-bootstrapped CA's subject at all (`pki_ca_create()`/`pki_intermediate_create()` are hard one-shots) -- this is the real "start over" operation that design deliberately never provided implicitly.

#### Added
- `pki_ca_reset()` (`daemon/src/pki.c`): wipes and regenerates root (+ intermediate, if one existed) with new common names, reissuing every currently-tracked leaf under the new chain -- same name/SANs/owner, fresh keypair. Every reissued leaf's `cert_pem`/`key_pem` are included in the response, the same "shown exactly once, right now" treatment a leaf's key gets at first issuance.
- `POST /v1/pki/reset` (defaults each CN to `"Kanxeo Root/Intermediate CA - <domain_suffix>"`), `kanxeoctl pki reset`, and a destructive web-dashboard action (confirm-gated) under PKI.
- Any still-live container that owns a reissued leaf gets it automatically redelivered (`pki_cert_deliver()`), so a running service's `tls.crt`/`tls.key` don't go stale.
- New `registry_list_names()` (`daemon/src/registry.c`) and `pki_cert_owned_by()` (`daemon/src/pki.c`) -- needed after the first redelivery implementation (enumerating via `containerdef_resolve_order()`) turned out to silently skip any `restart:"no"` container, since `containerdef_add()` only persists a definition for `restart != "no"`. Caught by comparing a live container's delivered cert file content before/after reset, not just the reset endpoint's status code.
- `test/test_pki.c` Part 5: real `openssl verify` proof that an old leaf stops verifying against the new root, a new serial after reissue, and a live container's own on-disk cert confirmed byte-different post-reset (the actual redelivery proof).

#### Fixed
- `kanxeoctl pki reset`'s own flag-parsing had three off-by-one `strncmp()` prefix lengths (`--root-common-name=`, `--intermediate-common-name=`, `--intermediate-days=`) -- every value-bearing invocation silently fell through to "unknown option" while the REST endpoint itself worked correctly. Caught only by testing the actual CLI command against the live daemon, not just the endpoint it calls.

#### Notes
- Verified against this project's own live production daemon: reset the real root/intermediate, confirmed the `cr-1`/`cr-2`/`srv1`/`blah` demo topology unaffected via a real `nsenter`+`ping` check.
- A `restart:"no"` container using a non-default `--pki-cert-dir=` gets redelivered to the default path after a reset, not its actual one -- no persisted body to recover that override from (see ADR-0049).

#### Added (ADR-0050: auto-issued host cert)
- `siteconfig_host_fqdn()` (`daemon/src/siteconfig.c`/`.h`) composes `<instance_name>.<site_name>.<domain_suffix>`.
- `reissue_host_pki_cert()` (`daemon/src/main.c`): a fixed `"host"` leaf record name (never the FQDN itself, so a rename doesn't orphan a differently-named cert), real current FQDN as its SAN. Called from `POST /pki/ca`, `POST /pki/intermediate`, `PUT /system/site`, and `POST /pki/reset` -- best-effort, never fails whichever of those actually-requested operations triggered it.
- `test/test_pki.c` Part 4 extended: `GET /pki/certs/host` carries the just-set FQDN after a site PUT; a second PUT with a different `instance_name` gets a genuinely different serial. Verified live against production (`gibsson.uk.home.arpa`, signed by the intermediate).

#### Added (ADR-0051: CA trust chain staged into every image)
- `pki_write_trust_bundle_file()` (`daemon/src/pki.c`/`.h`): root + intermediate (if bootstrapped), concatenated to a destination path -- a pure trust-anchor bundle, distinct from `pki_cert_deliver()`'s own order-significant leaf `fullchain.pem`.
- `pkg_seed_image_baseline()` (`daemon/src/pkg.c`) writes `etc/ssl/certs/kanxeo-ca-bundle.pem` into every new image -- idempotent, tolerant of no CA bootstrapped yet. No image built by this platform has ever shipped any CA trust before this, not even public roots.
- `test/test_pki.c` Part 6: creates a real image, reads the bundle off disk, `openssl verify`s a live leaf against it -- a genuine working trust anchor, not just "the file exists."

#### Notes (ADR-0051)
- This project's own `base`/`router`/`dev`/`pkgbuild` images (predating this feature) manually reseeded once and verified live. A CA reset does not retroactively propagate to any already-staged image's bundle -- a real, named gap (the image-level analog of leaf redelivery).

#### Added (ADR-0052, revises ADR-0046: server-side default DNS/PKI name qualification)
- `siteconfig_qualify()` (`daemon/src/siteconfig.c`/`.h`): a bare label (no `.`) gets `<label>.<site_name>.<domain_suffix>` appended; a dotted label is returned unchanged. Wired into `handle_dns_record_create()` and `handle_pki_cert_create()` (CN + default SAN, never an explicit `sans[]` entry).
- `web/app.js`'s `suggestedFqdn()` updated to the identical gate in the same pass.
- `test/test_dns.c` and `test/test_pki.c` extended with real qualify/don't-qualify assertions.

#### Fixed (ADR-0052)
- The first version gated qualification on `domain_suffix` alone, which defaults to a non-empty `"internal"` on every install regardless of configuration -- silently qualifying every bare name from the very first request, configured or not. Caught by running the full regression sweep: `test_dns.c`'s own bare `"shadow"` record 404'd under its own literal name. Fixed by gating on `site_name` being non-empty instead (defaults to `""`, "no site tier," an already-established ADR-0046 concept) -- a fresh, unconfigured install now behaves identically to before this phase. `suggestedFqdn()` had the same bug, fixed in the same pass before it shipped inconsistent with the server.

#### Notes (ADR-0052)
- Verified live: `POST /dns/records {"name":"testbox"}` against the production daemon (`site_name=uk` already configured) created `testbox.uk.home.arpa` for real.

#### Added (ADR-0053: auto-maintained instance DNS record)
- `reconcile_instance_dns_record()` (`daemon/src/main.c`): a single DNS record for this install's own FQDN, auto-maintained (reflects back, not a second editable record) -- deletes the old name and creates the new one on a rename. Skipped entirely when `--bind=` is `"0.0.0.0"` or `"127.0.0.1"` (no single correct address to publish for either). Called once at daemon startup and from `PUT /system/site`'s success path.
- `test/test_dns.c` extended to start its own daemon with a real, specific bind address (`--bind=127.0.0.2`, needs no host setup) specifically to exercise this -- every other test harness in this project binds to the default `127.0.0.1`, exactly the address this feature treats as "nothing to publish."

#### Notes (ADR-0053)
- Not verified against this project's own live production daemon -- it runs `--bind=0.0.0.0` (needed for the user's own desktop connectivity, Phase 35), which this feature correctly treats as "no address to publish"; rebinding it just to demonstrate this one feature would have reintroduced the connectivity problem `--bind=0.0.0.0` exists to fix. Verified through the isolated test instead.

#### Added (Part 6: a real management DNS container)
- `pkg/recipes/dnsmasq.recipe`: real, from-source, Debian's unmodified `dnsmasq_2.90.orig.tar.xz` (matching this sandbox's already-verified dnsmasq -- the exact binary `test/test_dns.c`'s own fixture already proved end-to-end against `dns_server_register()`), zero runtime dependencies beyond libc.
- Verified live against the production daemon: installed onto a new `dnssvc` image, run as a `dns1` container on a new `dnsnet` network (real `--gateway=`), registered via `POST /dns/servers`, and a real record resolved via the host's own `dig` -- genuine wire-protocol DNS, not a hosts-file check.

#### Fixed (Part 6)
- The shared `pkgbuild` toolchain image's own persisted rootfs (`images/pkgbuild/rootfs`) carried a stale, fully-built `coreutils` source tree at `build/src/` -- an uncleaned leftover from Phase 33's own self-hosting toolchain capture, not a defect in the new recipe. Since the build container's upperdir overlays *on top of* this lowerdir, the stale `GNUmakefile`/`maint.mk` pair shadowed dnsmasq's own real `Makefile` (`make` prefers `GNUmakefile`), failing every from-source build with `GNUmakefile:43: /maint.mk: No such file or directory` regardless of recipe correctness. Fixed by clearing `build/` from the live image rootfs (confirmed with the user first -- destructive against production daemon state, even though the path is pure internal build tooling).

#### Notes (Part 6)
- Explicitly out of scope, per the approved plan: reachability from outside Kanxeo's own managed networks (would need either enslaving `eth0`, this box's only reachable interface with no recovery path, or a host-port-forward mechanism this platform doesn't have).
- No dedicated `test_pkg.c` coverage added -- consistent with every other real, from-source recipe in this project (`bird`, `keepalived`, `lldap`, the full toolchain set): `test_pkg.c` is deliberately network-independent (local synthetic fixtures only), and every real recipe is instead proven live, exactly as this one was.

### Phase 35 (follow-up): instance_name -- a real, persisted identity for this specific install

Direct user request: "would giving the kanxeo instance a name make sense, so it can be managed?" Extends the existing site config subsystem (ADR-0046) rather than starting a new one -- `instance_name` is a third field alongside `site_name`/`domain_suffix`, same persisted file, same PUT-requires-everything-together contract, always non-empty (defaults to `"kanxeo"`).

#### Added
- `siteconfig.c`/`.h`: `instance_name` field, validated with the same `dns_name_is_valid()` rule as the other two. A persisted `site_config.json` from before this field existed loads cleanly -- the default fills in for just that one missing key, not treated as a malformed file.
- `GET`/`PUT /v1/system/site`: `instance_name` now required on PUT (matching `domain_suffix`'s existing required treatment) and always present on GET.
- `GET /v1/system/backup` / `POST /v1/system/restore`: new `site_config` field, same raw-file-content-as-a-JSON-string embedding every other backup field already uses -- this install's identity now actually survives a backup/restore round trip, which it didn't before (a real, if minor, pre-existing gap: site config was never in the backup bundle at all).
- `kanxeoctl site set` now fetches the current config before PUTting, so `--site-name=` alone no longer silently resets `domain_suffix`/`instance_name` to blank/default -- a latent footgun in the original single-shot PUT, fixed as part of adding the third field rather than left to compound.
- Web dashboard: instance name is now a real, visible identity -- shown in the header (`Kanxeo — kanxeo1`) and the browser tab title, editable in the same Site settings form, updated live via the existing poll (subject to the same dirty-tracking guard as the other two fields).

#### Notes
- No ADR -- an additive field on an already-decided subsystem (ADR-0046), not a new hard-to-reverse call.

### Phase 35 (web dashboard follow-up): site config form poll-clobber fix + FQDN suggestion wiring

Two related gaps surfaced by direct user reports against the live dashboard, both in `web/app.js` only -- no daemon/API change, `/v1/system/site` already worked correctly end-to-end.

#### Fixed
- Site settings form (`refreshSiteConfig()`) was unconditionally overwritten by every ~2s poll tick, stomping whatever the operator had just typed before they could hit Save -- reported as "I cannot change the site/domain." Same class of bug Phase 37's recipe-content editor was already built to avoid; this form never got that guard. Fixed with the same pattern: an `input`-driven dirty flag suppresses the poll-driven overwrite until save.

#### Added
- The client-side FQDN suggestion Phase 35's own ROADMAP entry already described (`<name>.<site_name>.<domain_suffix>`) was never actually wired into the DNS-record-create or PKI-cert-issue forms -- a documented-but-unbuilt gap. Now real: `df-name`/`pf-name` auto-expand a bare label (no dot typed) to this site's suggested FQDN on blur, still a plain editable text field afterward; their placeholders reflect the real configured suffix instead of a hardcoded `.internal` example.

### Phase 37: Packages tree UI -- recipe tab + installed-versions tab per package

Direct user request, last of the same batch Phase 34-36 came from: list packages under the Packages tree, each with a Recipe tab (view/edit) and an Installed tab (versions across images).

#### Added
- `GET /v1/pkg/recipes/{name}`: one recipe's own `{name,version,depends,content}` -- the raw `.recipe` text, not just the list view's metadata. New `pkg_recipe_get()` (`daemon/src/pkg.c`), `PkgRecipeDetail` schema. `kanxeoctl pkg recipe show NAME`.
- Web dashboard: Packages tree now lists one child per package name (union of recipes on file and installed-tracked names); new landing table + per-package detail view with Recipe/Installed tabs, same `.tab-bar` pattern as image/device detail views. Recipe tab supports view + edit-and-resubmit (existing upsert modal gained an optional textarea alongside its file input).
- `test/test_pkg.c`: recipe-content round-trip after upsert, 404 for an unknown name, 404 again after delete.

#### Notes
- Pure UI/UX layer over data that already existed -- no ADR, matching Phase 32's own precedent.
- Recipe content is fetched once per name and cached client-side (not every poll tick), so a poll-driven re-render never clobbers an in-progress edit -- same guard the console tab's own WebSocket already uses.

### Phase 36: persistent device name mappings, Proxmox-style (ADR-0048)

Direct user request: name a device (exact bus location, or vendor/model so it follows across USB ports), only named devices appear in the Devices tree.

#### Added
- New `daemon/src/devicemap.c`/`.h`: persisted `{name, kind, selector}` mappings on top of `device.c`'s own never-persisted discovery. `kind`: `"exact"` (a real device id) or `"vendor_model"` (`"<vendor_id>:<product_id>"`, port-independent). Resolution always re-derived fresh, never cached.
- `GET`/`POST /v1/devicemaps`, `DELETE /v1/devicemaps/{name}`. A container's own `devices[]` field now resolves a mapping name first, falling back to the existing raw device id path only if no mapping exists by that name -- fully backward-compatible.
- `kanxeoctl devicemap create/ls/rm`. Web dashboard: Devices tree now lists named mappings only (previously a flat link with no children); Devices view gained a USB/PCI/Network/GPU tab-bar plus a Named mappings table; "Name…" button per unmapped device row.
- `test/test_daemon_devices.c` extended: exact + vendor_model resolution against this sandbox's own real USB hardware, container creation via a mapping name, and the "mapping exists but resolves to nothing" 400 case.

#### Notes
- A mapping that exists but currently resolves to no device is a real 400 at container-creation time, deliberately not silently retried as a literal raw device id.
- Deleting a mapping never affects a container already using it -- device grants are resolved once, at creation time.

### Phase 35: site-scoped DNS/PKI naming + a real intermediate CA (ADR-0046, ADR-0047)

Direct user request in the same batch as Phase 34: a configurable site identity (not hardcoded `.internal`) that affects both DNS and PKI via a real API, plus a real intermediate CA tier.

#### Added
- New `daemon/src/siteconfig.c`/`.h`: persisted `site_name`/`domain_suffix` (default `"internal"`). `GET`/`PUT /v1/system/site` -- the platform's first `PUT` endpoint. Convenience only (ADR-0046) -- never enforced against DNS records or PKI SANs, which stay plain operator strings; only the web dashboard's own forms compose a suggested FQDN from it, client-side.
- `pki_intermediate_create()`/`pki_intermediate_bootstrapped()`/`pki_intermediate_get()` (`daemon/src/pki.c`, ADR-0047): a real intermediate keypair and cert, signed by the root (not self-signed), `basicConstraints=CA:TRUE,pathlen:0` + `keyUsage=keyCertSign,cRLSign`. `GET`/`POST /v1/pki/intermediate`, mirroring `/pki/ca`'s own shape.
- `pki_cert_create()` now signs with the intermediate whenever one is bootstrapped, the root directly otherwise -- transparent, no API change. `pki_cert_deliver()` now writes the real, complete chain (leaf + intermediate) into a running container's `tls.crt` when an intermediate exists.
- `kanxeoctl site show`/`site set`, `pki intermediate bootstrap`/`show`. Web dashboard: Site settings form under System, Intermediate CA block under PKI.
- `test/test_pki.c` Part 3/4: real chain verification (leaf fails against root alone, succeeds with the intermediate completing the chain -- proof the leaf was actually signed by the intermediate, not just claimed to be) and site config CRUD/validation coverage.

#### Notes
- Fully backward-compatible: an operator who never bootstraps an intermediate sees zero behavior change anywhere in existing PKI flows.
- `srv1`'s missing VRRP return route (found while restoring the live demo topology after Phase 34's own testing) was independently re-confirmed still fixed during this phase's regression sweep.

### Phase 34: container lifecycle completeness -- start/pause/unpause (ADR-0045)

Direct user report: a stopped container simply vanished from `GET /v1/containers` with no way back short of a full daemon restart. Root cause: `GET` only ever listed the live registry, never persisted-but-stopped definitions. Also confirmed with the user: build real cgroup-freezer pause/resume, not just the start fix.

#### Added
- `POST /v1/containers/{name}/start` -- replays a persisted definition's stored body through the same `create_container_from_body()` path autostart already uses; idempotent if live, 404 if no definition exists.
- `POST /v1/containers/{name}/pause` and `.../unpause` -- real cgroup v2 freezer control (`cgroup.freeze`), not `SIGSTOP`, via the container's own already-open `O_PATH` cgroup fd. Double-pause/double-unpause are `409`, not idempotent.
- `GET /v1/containers`/`GET /v1/containers/{name}` now also report stopped-but-defined containers (`status: "stopped"`), via new `containerdef_write_json_stopped_list()`/`_one()`.
- `Container.status` gains `"paused"`; new `paused: boolean` field. `kanxeoctl start/pause/unpause NAME`. Web dashboard: gated action buttons, right-click context menu entries, amber paused tree-status dot.
- New `test/test_container_lifecycle.c` -- stop-then-still-visible, start-without-daemon-restart, real cgroup-freeze/thaw checked against `/sys/fs/cgroup/<name>/cgroup.events` directly, pause-then-delete completing without hanging, autostart stale-flag fix confirmed across a real daemon restart.

#### Fixed
- **`registry_remove()` (the shared kill path for both `.../stop` and `DELETE`) would hang forever SIGKILLing an already-frozen container** -- a cgroup v2 freezer blocks signal delivery to every task inside it. Fixed by thawing (`registry_set_paused(e, 0)`) immediately before the existing `SIGKILL`.
- **`containerdef_autostart_all()` never cleared a stale `stopped=1` flag after successfully reviving an `"always"`/`"on-failure"` container** -- `handle_restart_timer_event()` (the crash-restart path) checks that flag unconditionally regardless of policy, so any container manually stopped even once, then revived by a daemon restart, would silently lose its own restart policy forever after. Fixed with the same `containerdef_set_stopped(name, 0)` call `handle_start()` already makes on success.
- `test_container_restart.c`'s own `fetch_pid()`/`container_exists()` helpers relied on "GET returns 404" as a proxy for "not live" -- broken by the new stopped-container listing above (GET now legitimately 200s for a stopped definition too). Updated both to check the `status` field instead, restoring their real original intent.
- `srv1`'s own static route back to `lan1` (via the VRRP address) had gone missing from this sandbox's live demo topology, silently breaking the `blah`→`srv1` round-trip (100% loss) -- the exact asymmetric-routing bug Phase 24 already fixed once, dropped by an unrelated mid-session state recreation. Recreated correctly; 0% loss confirmed.

### Phase 33: a real dev-toolchain recipe set -- gcc, python, coreutils, and 13 supporting packages, proven self-hosting

Direct user request for individual, real from-source dev-tool recipes rather than one bundled image, plus reloading `lldap` (already existed, had fallen out of the live daemon's own catalog after an earlier `--data-dir=` reset). Sixteen packages total, closing with a genuine self-hosting proof: a real Kanxeo container compiling, linking, and running a real C program with its own from-source `gcc`+`libc-dev`, and separately running real Python.

#### Added
- `pkg/recipes/m4.recipe` (1.4.19), `binutils.recipe` (2.42 -- `as`/`ld`/`ar`/`nm`/`ranlib`/`objdump`/`objcopy`/`readelf`/`strip`/etc), `bison.recipe` (3.8.2, `pkg_depends="m4"` -- hardcodes `/usr/bin/m4`), `flex.recipe` (6.17), `make.recipe` (4.4.1), `gawk.recipe` (5.3.0, needed by `autoconf`'s own `AC_PROG_AWK`), `autoconf.recipe` (2.71, `pkg_depends="m4 perl gawk"`), `automake.recipe` (1.16.5), `libtool.recipe` (2.4.7), `pkgconf.recipe` (3.0.5, with a real `pkg-config` compat symlink), `perl.recipe` (5.40.1), `python.recipe` (3.13.5, `--enable-shared`), `gcc.recipe` (16.1.0, real multi-source vendored build with `gmp`/`mpfr`/`mpc`/`isl`), `libc-dev.recipe` (glibc 2.36 headers + `crt1.o`/`crti.o`/`crtn.o`/etc, new recipe class -- the first to stage build-only C-runtime files into a *target* image), `coreutils.recipe` (9.11, `FORCE_UNSAFE_CONFIGURE=1`).
- A new `dev` target image with all sixteen packages installed.

#### Fixed
- `gcc.recipe`'s multi-source `pkg_source=`/`pkg_sha256=` were originally written multi-line with embedded newlines -- an instant ~20s parse failure on the real daemon, not a build failure. Fixed to the real single-line, space-separated format (ADR-0036), and `pkg_build()` now extracts the vendored `gmp`/`mpfr`/`mpc`/`isl` tarballs from their real `/build/extra/<basename>` landing spot instead of assuming pre-extraction.
- `gcc.recipe`'s `pkg_install()` now `strip --strip-unneeded`s every binary (`cc1`/`cc1plus`/`lto1` alone were ~400MB each unstripped, ~1.8GB total; stripped brings the real footprint to ~300MB) and symlinks `/usr/bin/ld` into gcc's own private `usr/libexec/gcc/x86_64-pc-linux-gnu/16.1.0/` prefix directory -- `collect2` (gcc's linker driver) searches for `ld` via the same private `COMPILER_PATH` as `cc1`, never falling back to `$PATH`, even though the real `ld` was on `$PATH` the whole time.
- Python's `configure` initially reported `ctypes`/`_sqlite3`/`_bz2`/`_lzma`/`_dbm`/`_uuid` all disabled ("necessary bits ... not found"); fixed by installing `libffi-dev`/`libbz2-dev`/`liblzma-dev`/`libgdbm-dev`/`uuid-dev`/`libsqlite3-dev` on the build host (flows into the isolated build sandbox via the existing wholesale toolchain copy) and rebuilding.

#### Notes
- gcc must be invoked by its absolute path (`/usr/bin/gcc`), not a bare `gcc` via `$PATH` -- bare-name invocation makes gcc compute a wrong *relative* `-iprefix`, breaking `cc1` with a misleading `posix_spawnp: No such file or directory`. Real GCC behavior in this minimal-container environment, not a Kanxeo bug.
- `coreutils`'s own `cat` was missing from every image before this phase -- self-hosting test scripts using `cat > file << EOF` silently failed with "command not found," producing stale/empty files that briefly looked like a real gcc bug before being traced to the missing `cat` itself.

### Phase 32: image detail view -- add/install/remove packages inline, tabbed to declutter

Direct user follow-up in the same request as Phase 31: install/remove packages straight from an image's own detail page, and split its two always-both-visible tables into tabs.

#### Changed
- `web/index.html`'s `#view-image-detail` restructured to the same `.detail-topbar`/`.tab-bar`/`.tab-panel` pattern the container detail view already uses (Phase 30 part 3) -- "Containers using this image" and "Packages installed" are now tabs instead of stacked sections; the page's existing generic tab-switching handler picked up the new tabs with no new JS.
- `web/app.js`: `renderImageDetail()` split into `renderImageDetailPackages()` and a new `renderImageDetailRecipes()` -- the latter lists the full recipe catalog inline with a per-row "Install onto this image" / "Remove from image" action, driven straight from `POST /v1/pkg/install` / `DELETE /v1/pkg/{name@image}` (ADR-0040) rather than requiring the standalone Packages section's separate name/image form. An "Add recipe..." button opens that section's existing upload modal rather than duplicating it.

An "image version" field was asked for alongside this but not built: `Image` (`docs/api/openapi.yaml`) has only a `name`, no version concept exists for images themselves (only individually-installed packages do, already shown per-row).

### Phase 31: a wireless-tools recipe set -- `procps`, `iw`, `hostapd`

Direct user request, verified the same two-phase way every prior recipe has been -- real build, then a full install through the actual `kanxeod` pipeline, ending with every installed binary confirmed to actually run inside a real running container.

#### Added
- `pkg/recipes/procps.recipe` (4.0.6) -- `ps`/`top`/`free`/`kill`/`pgrep`/`pkill`/`pidof`/`pidwait`/`pmap`/`pwdx`/`slabtop`/`tload`/`uptime`/`vmstat`/`w`/`watch`/`hugetop`, plus `sysctl`.
- `pkg/recipes/iw.recipe` (6.17) -- the nl80211-based CLI for wireless device configuration.
- `pkg/recipes/hostapd.recipe` (2.11) -- the IEEE 802.11 AP/authentication daemon, built with `CONFIG_IEEE80211W`/`CONFIG_SAE` added to upstream's `defconfig` for WPA3-Personal support.
- Six new permanent `extras[]` entries in `test/test_image_fixture.c` (`/usr/share/gettext`, `/usr/share/aclocal`, `/usr/share/automake-1.16`, `/usr/share/libtool`, `/usr/share/aclocal-1.16`, `/usr/share/misc`), found one at a time chasing `procps.recipe`'s `autogen.sh` through a real cascading toolchain-staging gap via direct chroot reproduction against the daemon's own `pkgbuild` sandbox.

#### Fixed
- **`hostapd.recipe`'s `pkg_install()` silently failed to copy its own binaries**, discovered by comparing the daemon's reported package manifest (missing `usr/sbin/hostapd`/`usr/sbin/hostapd_cli`, present only the unrelated absolute-path `libnl`/`libssl` copies) against a real build's actual output despite a clean "installed" state. Root cause: `pkg_build()`'s first line (`cd hostapd`) persists into `pkg_install()` since both run in the same shell session/cwd (`daemon/src/pkg.c`), so the original `cp hostapd/hostapd hostapd/hostapd_cli "$dir/"` looked for a nonexistent doubly-nested path and failed silently (no `set -e` in this recipe contract). Fixed by making the `cp` paths bare, matching the cwd `pkg_install()` actually inherits.

Verified end-to-end against a live daemon carrying its real 5-container VRRP/OSPF demo topology throughout (restart-survival re-confirmed via `restart: "always"` before each daemon restart this phase needed): all three packages installed for real onto a `wifitools` image through the actual pipeline; every binary confirmed to actually execute inside a real running container via `kanxeoctl console --cmd=` (`ps` printed a real process list, `iw`/`hostapd` printed correct version/usage banners). `top`/`watch` were confirmed (via `strace`) to need a terminfo database no image in this project ships yet -- a real, separate, named gap, not a defect in these recipes; container-liveness verification used `vmstat` instead (no curses dependency). Full clean rebuild, zero warnings; `test_pkg` re-run clean against its own isolated `--data-dir=`.

### Phase 30 part 6: `architecture.svg` redrawn for real -- content, not just paths

Direct user follow-up to part 5's own named boundary ("the diagram is real, current-format, and now correctly linked to from everywhere -- it just isn't a current picture of the system yet"): the actual diagram content, unchanged since Phase 16, is now current through Phase 30.

#### Added
- A new "Exec / Console" daemon module box (setns + PTY exec, WebSocket relay, ADR-0043), with a new dashed arrow down to the containers layer, routed along the runtime-library box's own right margin so it doesn't cross any box interior.
- A new "Physical Console" client-surface box (video tty0 + serial ttyS0, PID1-spawned `kanxeoctl`, ADR-0034) -- a real, architecturally distinct way of reaching `kanxeod` that predates this phase (Phases 18-19) but had never been drawn.

#### Changed
- The daemon module row grew from 6 to 7 boxes (128px each, down from 150px) and every box's text was tightened and refreshed for current capabilities: `Pkg / Image` now says "recipes via API" (Phase 26, ADR-0040) instead of implying static/installer-baked; `Network` gained "VLAN, gateway-opt" (Phase 22); `System` gained "backup" (Phase 17). ADR citations were deliberately dropped from this small row in favor of the larger, roomier boxes that already carry them (runtime library row, Persistent State column, Host OS layer) -- a legibility trade-off, not an accuracy loss.
- The Persistent State column's `pkg/recipes` box updated to "REST-managed catalog (ADR-0040)".
- Title/subtitle/footer updated from "Phase 16" to "Phase 30".

Verified by actually rendering it: `librsvg2-bin` was installed specifically for this (no SVG renderer existed in this sandbox before), and every edited region was rendered to PNG and inspected at full and cropped resolution -- confirming no text overflow, no unintended overlap, and the new exec/console arrow's routing genuinely clears every box it passes near, a risk pure coordinate arithmetic couldn't fully rule out on its own.

**Two real, substantive gaps found and fixed in `docs/api/README.md` during this same "are all the docs current" pass, not just the diagram**: `GET /v1/containers/{name}/console` (Phase 30, ADR-0043) was fully documented in `openapi.yaml` but entirely absent from this narrative walkthrough -- zero mentions, not even in the endpoint table -- fixed with a new table row and a full "Interactive container console" section. `/pkg/recipes` was still described as "provisioned out of band" / "the same v1 boundary container images already have" -- accurate before Phase 26, false since (ADR-0040 made recipe management a real, live REST API) -- fixed with corrected table rows for the now-existing `POST`/`DELETE`, and a rewritten paragraph plus a real request/response example replacing the stale claim.

### Phase 30 part 5: documentation audit -- `docs/` restructured into one subdirectory per document kind, no orphaned references

A meticulous, user-requested audit of the entire codebase and documentation set: validated git hygiene (nothing uncommitted or unpushed at the start), confirmed no orphaned/stray files and no duplicate implementations (one dead static function found and removed, `test_container_files.c`'s unused `json_str_field()`), confirmed the ADR timeline is already chronologically accurate (all 45 ADRs' dates strictly non-decreasing against `git log`, no gaps, no duplicate numbers), and restructured `docs/` so nothing but `README.md` lives in its root -- `docs/MISSION.md` moved to `docs/mission/MISSION.md`, `docs/ROADMAP.md` to `docs/roadmap/ROADMAP.md`, `docs/architecture.svg` to `docs/architecture/architecture.svg`, joining the pre-existing `docs/adr/` and `docs/api/` as one-subdirectory-per-document-kind.

#### Added
- `docs/README.md` -- a new top-level index explaining what lives in each `docs/` subdirectory and which document answers which question, without repeating any of their content.

#### Changed
- Every cross-reference to the three moved files updated across the repo: `CLAUDE.md`, root `README.md`, `docs/roadmap/ROADMAP.md`, `docs/api/README.md`, `docs/adr/README.md`, all 45 ADR bodies, and doc-comments inside `daemon/include/{dns,persist,registry}.h`, `daemon/src/main.c`, `image/src/{kanxeo-install,mkbootroot}.c`, `image/kernel/qemu-part1.config`, and `test/{test_daemon,test_dns,test_dual_console,test_rtnetlink}.c`. `CHANGELOG.md`'s own historical entries (e.g. "`docs/ROADMAP.md`: Phase 11 marked done") were deliberately left referring to the path that was actually correct at the time each entry was written -- rewriting them to the new path would misrepresent when the restructuring happened.
- Root `README.md`'s own Status table, stale since Phase 16, extended through Phase 30; added a `kanxeoctl console`/web-console usage entry to the "Using the installed system" section (present in the code and API since Phase 30 parts 1-3, absent from this walkthrough until now).

Verified: full clean rebuild, zero warnings; a repo-wide grep confirms zero remaining references to any of the three old paths outside of `CHANGELOG.md`'s own intentionally-preserved historical entries and `docs/mission/MISSION.md`'s own explanatory note about its rename.

### Phase 30 part 4: `kanxeod --data-dir=` for test/production state isolation

Raised by a real incident, twice in the same session: running this project's own test suite against this sandbox's own live daemon (a real 5-container VRRP/OSPF demo topology) silently wiped its persisted container/network/DNS/PKI state and, in the worse instance, the `router` image's entire installed-package rootfs -- because every daemon-linked test's own `reset_state()` and every test daemon it spawns shared the exact same hardcoded `/var/lib/kanxeo` default path with any real daemon on the same host. See ADR-0044.

#### Added
- `kanxeod --data-dir=PATH` (default unchanged: `/var/lib/kanxeo`) -- `daemon/src/main.c`'s compile-time `BASE_DIR`/derived-macro block became a runtime `g_base_dir[PATH_MAX]` plus derived `static char [PATH_MAX]` path buffers, computed once by `init_base_dir_paths()` right after argv parsing and before any subsystem touches them. Two new derived paths (`PKG_RECIPES_DIR`, `PKI_CERTS_DIR`) replace call sites that previously relied on compile-time string-literal concatenation, which a runtime buffer can't do.
- `test_data_dir_create()`/`test_data_dir_cleanup()` (`test/test_image_fixture.c`/`.h`) -- one shared `mkdtemp("/tmp/kanxeo_test_data_XXXXXX")` helper, matching the naming convention `test_pki.c`/`test_installer.c`/`test_boot_ab.c` already each hand-rolled independently.

#### Changed
- All 16 daemon-linked tests (`test_daemon.c`, `test_cli.c`, `test_web.c`, `test_daemon_net.c`, `test_networks.c`, `test_network_interfaces.c`, `test_images.c`, `test_container_restart.c`, `test_container_files.c`, `test_dns.c`, `test_pki.c`, `test_pkg.c`, `test_daemon_devices.c`, `test_system_update.c`, `test_system_backup.c`, `test_console_exec.c`) now spawn their own `kanxeod` with `--data-dir=` pointed at a fresh, isolated directory, and derive every direct-filesystem path they touch (fixture image roots, state-file reads for persistence checks, `reset_state()`'s own cleanup targets) from that same directory instead of a hardcoded `/var/lib/kanxeo/...` literal.
- `Makefile`: `test_web`, `test_pkg`, `test_system_update` now link `test/test_image_fixture.c` (needed for the new shared helper; previously omitted since none of the three used any other fixture function).

#### Fixed
- `test_images.c`'s "`base` is protected from deletion" check had never actually created a `base` image itself -- it silently depended on one already existing from whatever prior state happened to be on disk, true by accident under the old shared-path setup, never true under a genuinely fresh isolated directory. Surfaced by this same isolation work, not introduced by it; fixed by creating `base` via the real API first.

Verified: full clean rebuild, zero warnings; all 16 daemon-linked tests plus all 7 runtime-library tests pass under isolation; the live daemon's own real state (5 containers, `lan1`/`wan1` networks) confirmed byte-for-byte unaffected by the full test run via direct `curl` before and after.

### Phase 30 (web dashboard follow-up): fix `[hidden]` losing to author `display` rules on `.view`/`form`/`.modal-overlay`

A real, significant bug present since Phase 29's very first redesign, caught by the user's own real browser (three screenshots) after every structural check this sandbox could run (tag balance, id cross-referencing, JS syntax) had missed it, because none of them render CSS.

#### Fixed
- `[hidden]` does not reliably beat an author CSS rule setting `display` on the same selector -- cascade origin outranks specificity, so a normal-priority author `display` rule always wins over the browser's own default `[hidden] { display: none }`, regardless of how low that author rule's specificity is. `.view { display: flex }` had this defect from Phase 29 onward: every view section was rendering simultaneously, stacked down the page, no matter which one `renderCurrentView()` actually set `.hidden = false` on. `form { display: flex }` had the same defect wherever a form's own `.hidden` is toggled directly (`#pki-ca-form`). `.modal-overlay` (added this same day) had it too.
- Added explicit `<selector>[hidden] { display: none; }` overrides for all four affected selectors: `.view`, `form`, `.modal-overlay`, `.tab-panel` (the last already fixed in part 3 -- noticing that fix was the first clue, not generalized to the others until this bug surfaced visually).

**Lesson, stated plainly**: fixing `.modal-overlay`'s instance of this without immediately sweeping every other selector with the same shape (explicit `display` + JS-toggled `.hidden`) was a real miss, not a one-off. A systematic check now exists for it: cross-reference every CSS selector with an explicit `display` value against every element `app.js` actually toggles via `.hidden`.

### Phase 30 part 1: an interactive shell into a running container, over a hand-rolled WebSocket

Raised directly by the user while reviewing the redesigned dashboard: no exec/attach capability existed anywhere in Kanxeo. Staged in 3 parts; this covers part 1, the daemon mechanism itself. See ADR-0043.

#### Added
- `daemon/src/websocket.c`/`.h` -- minimal hand-rolled RFC 6455 (no fragmentation, 64KiB payload cap; `Sec-WebSocket-Accept` computed by shelling to `openssl`, same convention `pki.c` already established).
- `daemon/src/exec.c`/`.h` -- `exec_into_container()`: `setns()` into a running container's mount/UTS/net/pid namespaces (double-fork, mirroring `nsenter`/`docker exec`) and execs against a PTY allocated in the daemon's own namespace before any `setns()` call, so the exec'd process never needs a working `devpts` inside the container.
- `GET /v1/containers/{name}/console` (`docs/api/openapi.yaml`) -- upgrades to the WebSocket session; optional `X-Kanxeo-Exec-Cmd` header overrides the default `/usr/bin/bash`.
- `daemon/src/main.c`: `try_console_upgrade()`, new `CONN_CONSOLE_WS`/`CONN_CONSOLE_PTY` conn kinds, a deferred-free queue (`g_pending_free`) for the new hazard of two independently-epoll-registered fds that can tear each other down within the same event batch.
- `daemon/src/http.c`'s `find_content_length()` generalized into a reusable `http_find_header()`.
- `test/test_console_exec.c` -- hand-rolled raw-socket WS client against a real daemon and a real running container (`client/src/httpclient.c` has no streaming/Upgrade support to test against); reuses `test/dual_console_child.c` (Phase 28) as the exec target.

#### Fixed
- (Caught during design, before it shipped) namespace fds must all be opened *before* any `setns()` call, not interleaved -- the first `setns()` (mnt) moves the caller into the container's own mount namespace, so opening later fds by path afterward resolves against the container's own (often proc-less) `/proc`. Confirmed via `ENOENT` against a real running container before the fix.

Verified: full clean rebuild, zero warnings; `exec_into_container()` proven against a real running container (real shell arithmetic evaluating, not just PTY input echo); full handshake+relay via a manual smoke test and `test_console_exec.c` (5 consecutive clean runs); no leaked exec'd process survives teardown.

**A real mistake, caught and fixed the same session**: `test_console_exec.c`'s `reset_state()` (matching every other daemon test's own pattern) wiped `/var/lib/kanxeo/container_defs.json` -- this project's daemon has no test/production state isolation, and running the new test repeatedly against this sandbox's own live daemon deleted its persisted container definitions (the running containers themselves were unaffected; a future daemon restart would have lost them). Fixed by reading each container's real config out of its own still-running `/proc/<pid>/{cmdline,root}` and recreating all 5 through the real API -- confirmed byte-identical, VRRP re-electing correctly afterward.

**Not built:** parts 2 (`kanxeoctl console`) and 3 (web dashboard terminal, deliberately staying hand-rolled rather than vendoring xterm.js -- confirmed with the user); terminal resize propagation.

### Phase 30 part 2: `kanxeoctl console` -- a real, fully-interactive terminal client

#### Added
- `client/src/console.c`/`.h` -- WebSocket upgrade over a raw socket, a masked client-frame writer (the mirror image of `daemon/src/websocket.c`'s server-side, never-masks writer), a tolerant server-frame reader, and `cfmakeraw()` on the local terminal for the session's duration.
- `kanxeoctl console NAME [--cmd=PATH]`.
- `client/src/httpclient.c`'s internal `connect_to()` promoted to a public `kx_client_connect_raw()`; its own `write_all()` consolidated into the shared `kx_write_all()` (`include/iohelpers.h`, introduced in part 1).

#### Fixed
- (Caught by testing, before it shipped) the relay's `poll()` loop shrank `nfds` to 1 once local stdin hit EOF, which stopped it from ever examining the WebSocket socket again -- `poll(2)` already ignores a negative fd on its own; fixed by always passing `nfds=2`.

Verified: full clean rebuild, zero warnings; a piped (non-tty) session and a real pty-backed session (`pty.fork()`, exercising the full raw-mode path) against this sandbox's own live `cr-1` container -- real bash prompt, real shell arithmetic evaluating, clean exit, 3 consecutive runs, no leaked process.

**Not built:** part 3 (web dashboard terminal); terminal resize propagation.

### Phase 30 part 3: web dashboard console tab, right-click context menu, Proxmox-style container view

#### Added
- `web/app.js`'s `createTerminal()` -- a minimal, hand-rolled line-buffer terminal renderer (not a full VT100 emulator): `\r`/`\n`/backspace/Tab plus SGR color codes; every other CSI escape sequence recognized structurally and swallowed rather than leaked as garbage text. No cursor-addressable screen model -- `vim`/`top`/`less` render wrong, a stated boundary (see ADR-0043), `kanxeoctl console` has none of it.
- Container detail view restructured: `.detail-topbar` (Stop/Remove, always visible) + `.tab-bar` (Console/Summary, Console active by default). The browser's native `WebSocket` talks directly to `GET /v1/containers/{name}/console`.
- Right-click context menu (`#tree-context-menu`) on tree items -- Open console/Stop/Remove for containers, Remove for networks/images, reusing the existing action functions verbatim.

#### Fixed
- (Caught before it shipped) the existing 2s poll loop re-rendering the container detail view would have reopened -- and reset -- the console's WebSocket every cycle; fixed with an `openConsole()` guard keyed on the currently-connected container name.

Verified: live daemon confirmed serving these exact files; every new DOM id cross-checked between `app.js`/`index.html`; terminal renderer logic verified with a headless Node + DOM-stub unit check against a real captured bash transcript and a synthetic SGR/unsupported-CSI sequence. No browser in this sandbox -- data/logic-level verification, user's own browser session is the remaining check.

**Not built:** a real "start a stopped container" action (no backend support exists -- `cmd` isn't echoed back by the API and there's no `POST .../start`, named rather than faked); terminal resize propagation.

#### Added (same-day follow-up)
- Tree status dot (`.tree-status-dot`) before each container's name -- green (`running`) / grey (`exited`), the only two states `Container.status` has.
- Every category view's create form is now a collapsed-by-default `<details>` element -- list shows first, form only on demand. Package recipes' two actions (add recipe, bootstrap build image) split into two independent `<details>`.

#### Added (second same-day follow-up)
- Header "+ Create" dropdown (Proxmox-style) replacing the just-added inline `<details>` forms entirely -- every creatable resource opens the same shared `#modal-overlay`, which the 8 existing forms were relocated into (not rewritten) as hidden per-form panels. Every submit handler's success path now closes the modal too, except `pki-cert-form` (must stay open so the one-time-shown private key isn't hidden).
- Container detail view expanded from 2 tabs to 5: Summary (status/image/pid/exit status), Hardware (devices/interfaces/**networks** -- new, had no home before), Options (restart/depends_on/readiness/ip_forward/sysctls/files), Console (unchanged, still default), Backup (honest -- no per-container backup mechanism exists; links to System's own real backup/restore instead of faking one). Tab-switching generalized to a `data-tab`-driven loop instead of two hardcoded panel ids.

Verified: live daemon confirmed serving these exact files; all 101 element ids `app.js` references cross-checked programmatically against `index.html` (present exactly once, no orphans/duplicates); tag balance confirmed. No browser in this sandbox -- structural/logic verification, user's own browser session is the remaining check.

### Phase 29: Proxmox-style web dashboard redesign -- left resource tree, per-resource detail views

Raised directly by the user: the dashboard was a flat column of forms/tables with no navigation, and several real, already-shipped API capabilities had zero UI at all (Images, Devices, System, network interface attach/detach, recipe add/delete, container Stop). The user wanted something closer to Proxmox -- a left-side resource tree, compute wired to hardware/networks -- while staying 100% API-driven and framework-free (ADR-0010, explicitly reconfirmed). Two honest scope boundaries, not glossed over: no live resource-usage graphs (no backing endpoint) and no virtual-disk concept (storage is OverlayFS, not attachable disks) -- neither built, since the API-First Mandate means UI-only work doesn't invent backend capability.

#### Added
- `web/index.html`/`web/style.css`: full restructure into a `.layout` grid -- a left `<nav id="tree">` resource tree (Containers/Networks/Images/Devices/DNS/PKI/Packages/System) plus a content pane, replacing the single centered column. New detail-view sections for containers, networks, and images; new Images, Devices, and System sections built from scratch (none existed before).
- `web/app.js`: hash-based router (`location.hash` -> `renderCurrentView()`, no router library) and tree builder, both driven off the same 2s `poll()` cycle already powering the summary tables -- one fetch, two renderings. Detail-view renderers for containers (devices/interfaces/files/sysctls/restart/depends_on/readiness, plus a real Stop action distinct from Remove), networks (attached interfaces with attach/detach against `GET /devices`'s assignable `net` entries), and images (cross-references already-polled container/package data client-side, no new endpoints). Devices view groups by bus, with GPU member nodes grouped under their synthesized `gpu:N` prefix to mirror `device_find_group()`'s own server-side expansion. System view: backup download, restore upload, update (image/kernel path staging), and reboot/shutdown behind a real `confirm()` guard.
- "Run a container" form extended with every `ContainerCreateRequest` field that had no UI before: devices/interfaces (multi-select, sourced from `GET /devices`), files (repeatable path/content/mode rows), sysctls, restart policy + delay, depends_on, readiness.
- Recipe add/update-by-file-upload and delete added to the Packages: Recipes section (`POST`/`DELETE /v1/pkg/recipes`, backend already existed since Phase 26, no UI until now).

Verified: full clean rebuild, zero warnings. Every element id `app.js` binds against cross-checked directly against `web/index.html`; every field/endpoint/schema referenced cross-checked directly against `docs/api/openapi.yaml`. Exercised live against this sandbox's own already-running `kanxeod` (serving these exact files with no restart needed) and its real state: container/network/image/device/pkg listings match the new renderers field-for-field, and the real 400/409 error bodies (`DELETE /images/base` -> "the base image cannot be removed"; `DELETE /images/router` -> "image is still referenced by a running container") surface through `apiRequest()`/`showStatus()` exactly as designed.

**Not built:** interactive browser click-through (tree navigation, live form submission) -- this sandbox has no browser or headless-browser tooling available; verification here is build + live-data/schema cross-check, not a human click-test. The user's own browser session against the running daemon is the remaining check.

### Phase 28: installer dual-console (tty0 + ttyS0) support via a PTY relay

Raised directly by the user: the installer only ever worked interactively over serial, even though its own GRUB kernel command line already requests both `/dev/tty0` and `/dev/ttyS0` -- Linux binds `/dev/console` to whichever is listed last. See ADR-0042.

#### Added
- `image/src/dual_console.c`/`.h` -- new module, the first PTY usage in this codebase. `dual_console_open()`/`dual_printf()`/`dual_perror()` mirror status output to both consoles; `run_subprocess_dual_console()` relays an interactive child's I/O (via `posix_openpt()`, a `poll()`-based relay loop) across both simultaneously.
- `test/test_dual_console.c` + `test/dual_console_child.c` -- fully-automated test (no QEMU) proving the real relay logic against two throwaway PTYs standing in for the two real consoles.

#### Fixed
- `image/src/kanxeo-install.c`: `fdisk`'s interactive partitioning session and `mokutil`'s Secure Boot password prompt now use `run_subprocess_dual_console()`; every other status/error message in the file mirrored to both consoles via `dual_printf()`/`dual_perror()`.
- `early_mounts()` now also mounts `devpts` -- a genuinely new prerequisite (`posix_openpt()`'s slave device needs it, nothing else in this environment ever mounted it).

Verified: `test_dual_console` -- 3 consecutive clean runs; `test/test_installer.c`'s real 5-session Secure-Boot QEMU flow re-run clean through the new code, no regression on serial.

**Not built:** real video-console confirmation stays the user's own real-hardware/Proxmox check -- this sandbox's QEMU harness has no video backend to automate against.

### Phase 27: a real container-image baseline FHS layout

Closes the specific gap Phase 23 (`iptables`'s `/run/xtables.lock`) and Phase 24 (`bird` crashing with no `/dev/null`) both hit and fixed by hand on the already-built image, not reproducible from a fresh `pkg install`. See ADR-0041.

#### Fixed
- `daemon/src/pkg.c`'s `pkg_seed_image_runtime()` renamed to `pkg_seed_image_baseline()` and extended with standard `/dev/{null,zero,full,random,urandom}` char device nodes and a plain, empty `/run` directory -- reuses the same idempotent, self-healing extension point (`image_create()` + every `pkg_build_completed()`) already proven for runtime libs, so every existing image self-heals on its next install too.
- `pkg/recipes/bird.recipe`: added `--runstatedir=/run`, fixing `birdc`'s control socket at its actual root (confirmed via bird's own `configure.ac`/`Makefile.in`: `CONTROL_SOCKET="$(runstatedir)/bird.ctl"`) instead of leaving it defaulted to `/usr/var/run`. Verified against a real local build -- the compiled binary's own `PATH_CONTROL_SOCKET` is genuinely `/run/bird.ctl`.

Verified: full clean rebuild, zero warnings; `test/test_pkg.c`'s existing per-image seeding scenario extended with `stat()` checks for all 5 dev nodes and `/run`.

**Not built:** still a fixed, hardcoded baseline set, not extensible; `/tmp` and a `/var/run` compat symlink both deliberately deferred (neither has a confirmed real gap behind it -- see ADR-0041's own Consequences).

### Phase 26: package recipes are a real, live-managed catalog via a REST API

Found live while walking the user through creating their first test container: `pkg install --name=bash` failed on their freshly-installed box because no real install has ever had *any* recipe file staged anywhere -- a total gap, not bash-specific. A first attempt (ADR-0039) baked a fixed recipe set into the installer ISO, mirroring ADR-0019's runtime-lib mechanism -- the user correctly rejected it (updating a package catalog shouldn't require an OS reinstall) and it was reverted in full. See ADR-0040.

#### Added
- `POST /v1/pkg/recipes` (upsert by name, content validated before anything on disk changes -- an invalid upload can never clobber a working recipe) and `DELETE /v1/pkg/recipes/{name}`, plus `kanxeoctl pkg recipe add --name=NAME --file=PATH` / `pkg recipe rm NAME`. The real, ongoing way a recipe catalog is managed on a running system -- no ISO rebuild, no reinstall.

#### Fixed
- (Reverted) ADR-0039's install-time recipe staging (`mkinstalleriso.c`/`kanxeo-install.c`/`test_installer.c`/`README.md`) -- confirmed back to byte-for-byte their pre-ADR-0039 shape.
- `respond_pkg_error()`'s shared wording ("no such package") read wrong reused for a recipe 404 -- found live testing the new endpoints. New, dedicated `respond_pkg_recipe_error()` used only by the two recipe handlers.

Verified: full clean rebuild, zero warnings; an isolated harness proves the core add/delete logic (invalid name, name/`pkg_name=` mismatch, malformed content, and a bad upsert all rejected with nothing clobbered on disk); `test/test_pkg.c` gained a full HTTP-level scenario proving a recipe added purely through the API installs for real end-to-end; then verified live against this sandbox's own restarted daemon (add/list/rm/rm-again-404 over real HTTP). Restarting that daemon briefly killed its real Phase 24 VRRP topology outright (`PR_SET_PDEATHSIG`, not the "survives as an orphan" outcome predicted beforehand) -- self-healed on the very next startup via the daemon's own `restart:"always"` reconciliation, re-verified as a genuine VRRP re-election (not just four processes existing again), not just assumed recovered.

**Not built:** a fresh install still starts with zero recipes -- a deliberate trade-off (ADR-0040), not a gap.

### Phase 25: diagnose real-install PKI/pkg bootstrap failures, fix silent error paths

The user hit generic "PKI operation failed"/"package operation failed" dashboard errors on a real booted install. Traced both to ground and closed the actual gaps found along the way, rather than patching the symptom. See `docs/ROADMAP.md` Phase 25 for the full trace.

#### Fixed
- `include/pathutil.h`'s `kx_mkdir_p()` and `daemon/src/persist.c`'s `persist_atomic_write()`/`persist_read_file()` failed completely silently on error -- no `fprintf`/`perror` at all, unlike `pki.c`'s own `openssl` subprocess failures, which already logged a real reason. Fixed once at the shared primitive level (used at the time by `pki.c`, `pkg.c`, `dns.c`, `image.c`, `main.c` -- `network.c`/`containerdef.c` hadn't yet adopted these primitives themselves at this point, and only started using them in later phases) rather than per call site.
- `daemon/src/pkg.c`'s `run_subprocess()` and `test/test_image_fixture.c`'s near-duplicate `run_cp_a()` swallowed `execve()` failures and nonzero exit status/signal with no diagnostic. Both now report the exact reason.
- The web dashboard's "Bootstrap build image" button (`web/app.js`) always POSTed an empty body, permanently locked into the dev-convenience toolchain-copy fallback that's explicitly empty/non-functional on a real minimal install. `web/index.html`/`web/app.js` gain an optional toolchain artifact path field wired into the existing `toolchain_path` POST field (already supported server-side and via the CLI's `--toolchain=` since Phase 20) -- blank preserves today's exact behavior.

Verified: full clean rebuild, zero warnings. Ran both of Phase 20's own permanent QEMU console regression tests (`test_console_pki_bootstrap`, `test_console_pkg_bootstrap`) fresh against current source -- both PASS. Then built a real, distributable installer `.iso` from that same source and had the user do a genuine fresh install with it on their own real environment: `pki ca bootstrap`, `pkg bootstrap`, and `pki cert create` all confirmed working there -- decisive, real-world proof, not just a sandbox test. New logging verified in isolation against a guaranteed-unwritable path.

**Not built:** the base-image FHS-layout convention itself (still open from Phases 23-24) -- this phase made failures loud, not the convention; the improved diagnostics aren't surfaced through the REST API response itself, only the console, a deliberate scope boundary.

### Phase 24: the actual VRRP + bird topology, proven end-to-end

Two real router containers, a real VRRP-shared gateway address, real OSPF between them, real client traffic surviving one router's failure with zero config change -- the scenario that started the whole networking-redesign conversation (Phase 22), now genuinely deployed and verified. Pure deployment/configuration, no daemon code changes. See `docs/ROADMAP.md` Phase 24 for the full topology and verification sequence.

#### Fixed
- `bird` hard-fails with no `/dev/null` -- no image built by this platform ships any baseline `/dev` at all. Standard device nodes staged into the `router` image (same convention `test_image_fixture_stage_toolchain()` already uses for the toolchain image).
- `birdc`/`birdcl` need `/usr/var/run` to exist for their control socket -- staged the same way.
- `pkg/recipes/bird.recipe` never staged `libreadline.so.8`, needed by `birdc`'s own interactive client (confirmed via `ldd`) -- fixed in the recipe, the same pattern every other recipe in this set already uses. `birdcl` (no readline) is a real, complete substitute for scripted queries and needed no fix.

Real design lesson from a real failure: the first deployment gave only the client-facing segment (`lan1`) a VRRP address, leaving the "far side" target with no route back -- forwarding worked one-way, replies had nowhere to go. Fixed by making VRRP symmetric (a second `vrrp_instance` on the upstream segment too) -- the more correct design, not a workaround.

Verified end-to-end against live containers: VRRP election, genuine OSPF `Full` adjacency (`birdcl show ospf neighbors` on both sides), real packet forwarding (0% ping loss), and the actual payoff -- stopping the MASTER router makes the BACKUP take over both VRRP addresses within seconds, with the client's own route table never changing and connectivity never dropping; restarting the original router reclaims MASTER via keepalived's default preemption.

**Not built:** no base-image FHS-layout convention yet (`/dev`/`/run`/`/usr/var/run` -- fixed manually on the already-built image this phase, not reproducible from a fresh `pkg install` yet); no VRRP authentication (VRRPv3 doesn't support it, and this is a verification deployment, not hardening); OSPF's dynamic route learning isn't load-bearing in this specific 2-router topology (both routers are already directly connected to both segments) -- real adjacency-forming is genuine proof bird works, a topology where it's actually required is future work.

### Phase 23: a real router recipe set -- bird, keepalived, iproute2, ipset, iptables, iputils, bash

Four new recipes (keepalived, ipset, iptables, iputils -- bird/iproute2/bash already existed) giving a router container real tools: routing (bird), VRRP failover (keepalived), kernel networking/filtering (ip, ipset, iptables), diagnostics (ping/arping/tracepath), a shell (bash). See `docs/ROADMAP.md` Phase 23 for the full build-constraint reasoning (library version mismatches, toolchain dependency staging).

#### Added
- `pkg/recipes/keepalived.recipe`, `ipset.recipe`, `iptables.recipe`, `iputils.recipe` -- each real, from-source, doubly verified (real local build, then a full install through the actual `kanxeod` pipeline).
- This build host gained real `-dev` packages (`libmnl-dev`, `libnftnl-dev`, `libnfnetlink-dev`, `libnl-3-dev`, `libnl-genl-3-dev`, `libiptc-dev`, `libipset-dev`, `libcap-dev`, `libidn2-dev`, `meson`, `ninja-build`) -- flows into the pkgbuild toolchain the same way the Rust/Go toolchains already do.

#### Fixed
- `daemon/src/pkg.c`'s `merge_tree()` silently dropped every symlink when merging a package's build output into its target image -- a documented but never-exercised "v1 boundary" until iptables' own `make install` (which creates `iptables`/`ip6tables`/... as symlinks to one `xtables-legacy-multi` binary) became the first real recipe to need one. Fixed properly: a real `S_ISLNK` branch (`readlink()`/`symlink()`, manifest-tracked like a regular file).
- `pkg/recipes/iproute2.recipe` (written in an earlier phase) gained new optional features -- and new, previously-unstaged runtime library deps (`libelf`, `libmnl`, `libcap`, `libz`) -- once this phase's own toolchain additions made them newly detectable at build time. Fixed by staging those real extras in `pkg_install()`, the same pattern every other recipe in this set already uses.

Verified: all seven packages installed for real through `kanxeod` onto a `router` image; every binary confirmed running; genuine kernel-level proof inside a real running container -- a real `iptables -A`/`-L` round-trip, a real `ipset create`/`add`/`list` round-trip, real `ip addr show` output. Full clean rebuild, zero warnings; full pre-existing non-QEMU regression suite (21 binaries) re-run clean.

**Not built:** no standard `/run` (or other FHS runtime dir) convention for minimal images yet -- a real, generic gap (iptables' own locking needs it), not any one recipe's to solve; the actual two-router VRRP+bird topology that motivated Phase 22 hasn't been wired up end-to-end yet -- this phase proves the tools work individually.

### Phase 22: network gateway becomes optional, VLAN + physical-NIC bridge attachment

A network's host-owned gateway address was always mandatory (hardcoded `.1` on the bridge, every attached container got an unconditional default route toward it) -- impossible to build a router-container topology (a VRRP pair owning the actual gateway, not the host) on top of. See ADR-0037, ADR-0038.

#### Added
- `daemon/src/network.c`/`network.h`: `network create` gains optional `gateway`/`--gateway=A.B.C.D` -- omitted (new default) means a pure-L2 bridge with no host-owned address at all. `network_spec`/`container_net_child_configure()` (`src/container_net.c`) only install a default route when the primary attachment actually has a gateway. `registry_alloc_ip()` gained an `exclude_be` parameter since a gateway is no longer always host-part 1.
- `network_attach_interface()`/`network_detach_interface()` (`daemon/src/network.c`): enslaves a real host interface to a network's bridge (untagged, via the existing unchanged `rtnl_link_set_master()`) or, with a `vlan_id`, creates and enslaves an `<ifname>.<vlan_id>` 802.1q sub-interface instead (new `rtnl_vlan_create()`/`rtnl_link_clear_master()`, `netplane/src/rtnetlink.c`). New `POST`/`DELETE /v1/networks/{name}/interfaces[/{ifname}]`, CLI `network attach-interface`/`detach-interface`.
- `daemon/src/device.c`: `enumerate_net_one()` now reports an interface `assignable=0` if it already has a `master` (enslaved to anything), reusing the same sysfs-visibility-is-exclusivity pattern ADR-0022 established for netns-moved interfaces.
- `docs/adr/0037-network-gateway-optional.md`, `docs/adr/0038-vlan-and-physical-nic-bridge-attachment.md`.
- `test/test_network_interfaces.c`, new; `test/test_rtnetlink.c`/`test/test_container_net.c`/`test/test_networks.c` extended.

#### Fixed
- `test_dns.c`/`test_daemon_net.c`/`test_container_restart.c` each created a network with no gateway and then relied on the host or the daemon itself connecting straight into it (`dig`, `connect()`, readiness checks) -- broken under the new default, not a bug in it. Fixed by giving each of those tests' own networks an explicit `--gateway=` matching what they actually need, not by changing the default.

Verified: full clean rebuild, zero warnings; all new/changed test scenarios 3 consecutive clean runs; full pre-existing non-QEMU regression suite (21 binaries) re-run clean. A real, pre-existing characteristic of this sandbox surfaced along the way (not a regression): `connect()` to a closed port on a container here takes several real seconds to fail rather than an instant refusal, so `test_container_restart.c` needs a longer wall-clock budget than a quick interactive run allows, though it completes correctly given one.

**Not built:** the full positive interface-attach path (real hardware needed, this sandbox has none -- same boundary ADR-0022 already documents); bridge VLAN filtering (a considered non-choice, not a gap).

### Phase 21: git/gitea recipes, multi-source package recipes, and a real LDAPS deployment

Real git and gitea recipes, a new multi-source recipe mechanism needed for lldap's own real build, and the platform's first genuinely TLS-secured workload service -- Kanxeo's own PKI issuing a real cert an LDAP server actually uses for LDAPS. See ADR-0036.

#### Added
- `pkg/recipes/git.recipe`, `pkg/recipes/gitea.recipe`: real, from-source builds, each individually proven through the real `kanxeod` pipeline.
- `daemon/src/pkg.c`/`daemon/include/pkg.h`: `pkg_source=`/`pkg_sha256=` become space-separated, positionally-paired lists (`PKG_MAX_SOURCES=16`) -- index 0 extracted as before, indices 1+ copied verbatim into `/build/extra/<basename>`, fetched via nested `fork()`/`execve()`/`waitpid()` (never a shell script). `docs/adr/0036-multi-source-package-recipes.md`.
- `pkg/recipes/lldap.recipe`: a real, from-source lldap build (chosen over `glauth` for its genuine first-party web UI), doubly verified -- a real local build (every CDN asset's sha256 confirmed and cross-checked against lldap's own SRI hashes) then a full install through `kanxeod`.
- `test/test_image_fixture.c`: `/usr/local/cargo`'s staged extras now include a pre-populated `wasm-pack-cache` (rides along with the existing wholesale copy, no new staging entry).

#### Fixed
- `wasm-pack` failing outright inside the isolated `pkg_build()` container ("couldn't find your home directory, is $HOME not set?") -- `g_build_envp` now sets `HOME=/build`.
- `wasm-pack` needing to `cargo install wasm-bindgen-cli` at build time, which the network-less build container can't do -- pre-populated once via wasm-pack's own `WASM_PACK_CACHE` env var on the real build host; `lldap.recipe` exports it to match.
- `pkg_seed_image_runtime()`'s `runtime_libs[]` (ADR-0023) missing `libgcc_s.so.1`/`libm.so.6` -- a real Rust binary (`lldap`) failed to even start (`error while loading shared libraries`). Fixed generically, not lldap-specifically: any future Rust/C++ package needs both.
- `daemon/src/main.c`'s `--pki-issue` handling treated `pki_cert_create()`'s `PKI_ERR_DUPLICATE` (expected on every restart after the first) as a hard failure, skipping `pki_cert_deliver()` -- a container whose process starts fast enough to read its TLS cert before delivery finishes on the *first* attempt would crash-loop **forever** under `restart:"always"`, since every respawn after that hit `DUPLICATE` and never reached delivery again. Now treated as "cert already exists, deliver it." Confirmed fixed against both a real `kill -9` crash and a real daemon restart.

Verified: a real LDAPS deployment (config staged via the pre-existing `--file=` mechanism with every path absolute, sidestepping `run`'s lack of a `--workdir=` option entirely; `--pki-issue --pki-cert-dir=/opt/lldap`; `--restart=always`) -- `openssl s_client -verify_hostname ldapsvc -CAfile ca.crt` returns `Verify return code: 0 (ok)` against Kanxeo's own root CA; `ldapsearch -H ldaps://ldapsvc:6360` performs a real bind and returns real directory entries. Full clean rebuild, zero warnings; full pre-existing non-QEMU regression suite (20 binaries) re-run clean.

**Not built:** DNS registration for the LDAP service (`--dns-register` alongside `--pki-issue`); LDAPS is proven for this one container, not yet adopted as a platform-wide auth convention.

### Phase 20: fix silent real-install breakage in PKI/pkg, portable build-toolchain artifact

While answering how to get gitea onto a Kanxeo host, checking the user's own build-toolchain-container proposal surfaced two real, live-confirmed bugs: `pki ca bootstrap` failed outright on a genuinely fresh installed image (booted one, ran it on the real console, confirmed `CA genpkey failed`/HTTP 500). See ADR-0035.

#### Added
- `image/src/mkbootroot.c`: stages `openssl`/`curl`/`tar`/`sha256sum`/`cp`/`rm`/`unsquashfs` (the exact binaries `kanxeod` shells out to, grep-confirmed) plus each one's real shared-library closure onto the installed root -- previously entirely absent.
- `test/test_image_fixture.c`: new `test_image_fixture_stage_toolchain()` -- shared toolchain-staging logic (extracted from `daemon/src/pkg.c`), now including `/usr/local/go` when present.
- `image/src/mktoolchainimage.c`, new: standalone build-time tool producing one real, portable toolchain squashfs artifact.
- `daemon/src/pkg.c`: new `pkg_bootstrap_from_toolchain()` -- imports a toolchain artifact via `unsquashfs -f -no-xattrs -d`, validated by real squashfs magic bytes (not `stat()`+`S_ISREG`, so a raw scratch-partition device path works too). `POST /v1/pkg/bootstrap` gains an optional `toolchain_path` field; `kanxeoctl pkg bootstrap` gains `--toolchain=PATH`.
- `daemon/src/main.c`: new `--test-bootstrap-toolchain=` self-test flag, same precedent as `--test-update-image=`.
- `docs/adr/0035-portable-toolchain-artifact-for-pkg-bootstrap.md`.
- `test/test_console_pki_bootstrap.c`, new: boots a genuinely fresh install and scripts `pki ca bootstrap` over the console -- the real regression guard for the PKI bug.
- `test/test_console_pkg_bootstrap.c`, new: builds a real toolchain squashfs, boots fresh, imports it via the new self-test flag, confirms a real `gcc` binary landed.
- `test/test_disk_image.h`/`.c`: new `mem_mib` field on `qemu_boot_opts` (configurable guest RAM), needed once the real decompressed toolchain content (~2.0GB) exceeded what a default 512MB guest's tmpfs fallback could hold.

#### Fixed
- `daemon/src/pkg.c`'s `runtime_libs[]` (used by `pkg_seed_image_runtime()`, ADR-0023): read from `/usr/lib/x86_64-linux-gnu/...`, but `mkbootroot.c` only ever writes those files to `/lib64/...`/`/lib/x86_64-linux-gnu/...` on the installed root -- silently broke *every* `image create` on a real install, not just PKI/pkg. This dev sandbox's own merged-`/usr` symlinks masked it locally.
- `openssl req -x509` additionally needed its own default config file (`/usr/lib/ssl/openssl.cnf`) -- found by a second live failure after the binary itself was staged; now staged too.
- `unsquashfs` exiting nonzero on a benign "can't write xattrs to this filesystem" warning (tmpfs) -- fixed with `-no-xattrs`, the tool's own diagnostic named the fix.

Verified: `test_console_pki_bootstrap`/`test_console_pkg_bootstrap` each 3 consecutive clean runs; full clean rebuild, zero warnings; full pre-existing regression suite re-run clean.

**Not built:** a real, working gitea recipe -- this phase proves the toolchain-import mechanism, not gitea itself.

### Phase 19: real console login -- keyboard input + PID1-spawned kanxeoctl shell

Phase 18 shipped video output and a REPL, but real Proxmox use surfaced two gaps neither closed: no keyboard input driver at all on the video console, and nothing on the box ever launched `kanxeoctl` on any console. Raised directly by the user immediately after using the Phase 18 ISO for real. See ADR-0034.

#### Added
- `image/kernel/qemu-part1.config`: PS/2 (`CONFIG_KEYBOARD_ATKBD` + its own `SERIO`/`SERIO_I8042` selects) and USB HID (`CONFIG_USB`/`CONFIG_USB_XHCI_HCD`/`CONFIG_HID`/`CONFIG_USB_HID`) keyboard drivers, plus three menuconfig gates (`CONFIG_INPUT_KEYBOARD`/`CONFIG_USB_SUPPORT`/`CONFIG_HID_SUPPORT`) a first build attempt found silently required.
- `image/src/mkbootroot.c`: new required `kanxeoctl_bin` argv, stages `/bin/kanxeoctl` into the installed root -- previously absent entirely.
- `daemon/src/main.c`: `spawn_console_shell()`/`register_console_shell_pidfd()`/`handle_console_shell_event()` (pidfd+epoll reap, mirrors `register_pkg_fetch_pidfd()`), `arm_console_respawn_timer()`/`handle_console_respawn_timer_event()` (timerfd delay, mirrors `arm_restart_timer()`). `kanxeod` (PID 1) now `execve()`s `kanxeoctl` on both `/dev/tty0` and `/dev/ttyS0` once boot is healthy, no login, respawning forever on exit after a 2-second delay.
- `docs/adr/0034-console-login-via-supervised-kanxeoctl.md`.
- `test/test_console_shell.c`, new: scripts a real serial-console round-trip (exit the first shell instance, wait for the *respawned* instance's own fresh prompt, confirm it answers `health`) via `qemu_boot_capture()`'s existing `scripted_input` mechanism -- genuine proof the reap+respawn machinery works, not just that it compiles.

Verified: kernel driver binding confirmed in a real boot log (`input: AT Translated Set 2 keyboard`, `usbcore: registered new interface driver usbhid`); `test_console_shell` 3 consecutive clean runs; `test_boot`/`test_boot_ab`/`test_installer`/`test_boot_update` each re-run 3 consecutive times against the new kernel + image; full clean rebuild, zero warnings; full regression suite re-run clean.

**Not built:** actual keyboard-driven interaction on the video console is unverifiable in this sandbox (no QMP channel to synthesize a keypress) -- the user's own final check, same boundary GPU passthrough/Secure Boot already have.

### Phase 18: kernel video console + interactive kanxeoctl shell

Raised directly by the user after their first real Proxmox install: no video console on the installed system (only serial ever worked), and no way to stay on the box issuing `kanxeoctl` commands one after another without re-typing `--host=...` each time.

#### Added
- `image/kernel/qemu-part1.config`: `CONFIG_VT`/`CONFIG_VT_CONSOLE`/`CONFIG_FRAMEBUFFER_CONSOLE`/`CONFIG_SYSFB_SIMPLEFB`/`CONFIG_DRM_SIMPLEDRM`/`CONFIG_DRM_FBDEV_EMULATION` -- rides the EFI GOP framebuffer UEFI firmware already sets up via DRM's `simpledrm` driver, works without any GPU-specific driver bound to anything.
- `cli/src/main.c`: `dispatch_command()` (the one-shot dispatch chain, extracted so both call paths share it), `tokenize_line()` (quote-aware whitespace tokenizer), `run_shell()` (interactive REPL, entered when `kanxeoctl` is invoked with no command on a real terminal). `exit`/`quit`/EOF end it; `help` reuses `print_usage()`.
- `README.md`: notes on both.

#### Fixed
- `tokenize_line()` didn't treat `\n`/`\r` as delimiters, so `fgets()`'s trailing newline stayed attached to the last token -- every typed command, including `exit`, silently fell through to "unknown command." Found via a real `pty`-based interactive test, not just the non-tty gating check.

Verified: real kernel source fetch, `merge_config.sh`, `olddefconfig`, programmatic confirmation every fragment symbol (not just the new ones) landed as `=y`, then a full `make bzImage`. `test_boot`/`test_boot_ab`/`test_installer`/`test_boot_update` each re-run 3 consecutive times against the new kernel; the captured serial log confirms `simpledrm` bound and the console switched to it (`Console: switching to colour frame buffer device 160x50`) -- final confirmation on a real video output is the user's own check, same boundary GPU passthrough/Secure Boot already have. A real `pty`-based interactive session confirmed `health`/unknown-command/`ps`/`exit` all behave as designed. Full clean rebuild, zero warnings; full regression suite (20 non-QEMU + 4 QEMU-based binaries) re-run clean.

**Not designed or built yet:** the larger web dashboard redesign (tree-based navigation, full API parity, splittable into its own container) the user asked about in the same conversation -- deliberately deferred to its own future planning cycle.

### Phase 17: platform state backup/restore

Kanxeo had no backup, export, or restore mechanism at all. Raised directly by the user while preparing a real, one-shot, no-rollback migration of a production home lab from Proxmox. Scope, confirmed with the user before any code was written: platform configuration state only (container defs, networks, DNS records, pkg install state + recipes) -- not workload data, not image content, never PKI. See ADR-0033.

#### Added
- `daemon/src/main.c`: new `do_system_backup()`/`handle_system_backup()` implementing `GET /v1/system/backup` (bundles each state file's raw content as an escaped JSON string via the existing `persist_read_file()`, plus every `*.recipe` file on disk); new `json_string_field_is_valid()`, `do_system_restore()`/`handle_system_restore()` implementing `POST /v1/system/restore` (independent, all-optional fields, every field validated before any file is written via the existing `persist_atomic_write()`; does NOT reboot or hot-reload -- restored state takes effect on the next boot via the existing, unmodified `containerdef_autostart_all()` replay path).
- `cli/src/main.c`: `backup [--output=PATH]` (saves the bundle verbatim, byte-for-byte) and `restore --input=PATH`.
- `docs/api/openapi.yaml`: new `/system/backup` (GET) and `/system/restore` (POST) paths, new `SystemBackupBundle` schema.
- `docs/adr/0033-platform-state-backup-restore.md`.
- `test/test_system_backup.c`, new: a real persisted container/network/DNS record created, `GET /system/backup` confirmed to embed matching content, the container deleted entirely, `POST /system/restore` with just the captured `container_defs` field, a real daemon restart afterward confirmed the deleted container came back -- proof the existing boot-time replay path genuinely reconstructs state, not just that a write succeeded. Malformed-field restore confirmed rejected `400` with the real file byte-for-byte untouched; restore confirmed to not hot-reload.

Verified end-to-end against a live, twice-restarted daemon, no QEMU needed (pure file I/O + JSON). One real test-hygiene bug found and fixed along the way: a network's own real kernel bridge interface isn't removed just by deleting its JSON state file between test runs -- fixed by following `test_networks.c`'s own already-established convention (real API `DELETE` for cleanup, not filesystem-level state deletion). 3 consecutive clean runs; full pre-existing regression suite re-run clean. Zero compiler warnings.

## Phase 16 (parts 1-2): host OS update mechanism (root + kernel), and automatic package updates

### Phase 16 (part 2): per-slot kernel updates

Part 1 left the kernel out: both A/B loader entries hardcoded the identical `linux /kanxeo-bzImage`, one file shared by both slots with no way to update it without corrupting whichever was currently booted. Raised directly by the user immediately after part 1 shipped, once they understood `/system/update` didn't cover the kernel. See ADR-0032.

#### Added
- `image/src/kanxeo-install.c`: `populate_esp()` now stages the kernel to *both* `kanxeo-bzImage-a` and `kanxeo-bzImage-b` (identical content) instead of one shared `kanxeo-bzImage` -- unlike root B's own deliberate emptiness, there's nothing meaningful about slot B lacking a kernel file before its first update, only a real edge case removed at near-zero cost.
- `daemon/src/main.c`: `write_file_to_device()` split into a shared `copy_bytes()` core (the existing read/write/`fsync` loop) plus two thin, flag-specific callers -- itself unchanged in behavior, plus a new `write_file_to_esp()` (the ESP is a real, already-mounted filesystem, needs `O_CREAT` where a raw device doesn't); `do_system_update()` gains optional `kernel_path` (independent of `image_path`, at least one of the two required), validated via a real bzImage magic check (boot-sector signature at `0x1FE`, `struct setup_header`'s `"HdrS"` at `0x202`) before anything is written, both fields fully validated before either is written; the loader entry's `linux` line is now templated per-slot (`/kanxeo-bzImage-<slot>`) instead of a fixed string; response gains `"updated"` (`["root"]`/`["kernel"]`/`["root","kernel"]`); new `--test-update-kernel=` self-test flag, sibling to the existing `--test-update-image=`.
- `cli/src/main.c`: `update` gains `--kernel=PATH` (now both `--image=`/`--kernel=` optional, at least one required).
- `docs/api/openapi.yaml`: `/system/update`'s request schema gains optional `kernel_path`, response gains `"updated"`.
- `docs/adr/0032-per-slot-kernel-updates.md`.
- `test/test_disk_image.c`/`.h`: new `esp_mcopy_out()`, the reverse of the existing `esp_mcopy_in()` -- reads a file back off the ESP for byte-exact verification.
- `test/test_boot.c`, `test/test_boot_ab.c`: updated to stage per-slot `kanxeo-bzImage-a`/`-b` instead of the old shared filename, matching what the real installer now produces.
- `test/test_boot_update.c`: extended with a fifth scratch partition holding a second copy of the real `build/bzImage`; the self-test call now updates root *and* kernel together, verifying `kanxeo-bzImage-b` byte-exact, the fresh loader entry's own content referencing `/kanxeo-bzImage-b` specifically, `kanxeo-bzImage-a` unchanged, and a second real QEMU boot succeeding from the freshly-written kernel.
- `test/test_system_update.c`: extended with `kernel_path` validation scenarios (missing/nonexistent/bad-magic, and a valid `image_path` combined with a bad `kernel_path`) -- all `400` against a plain dev daemon, no QEMU needed.

Verified inside a real QEMU guest: a combined root+kernel update reports both updated, lands byte-exact bytes at `kanxeo-bzImage-b`, correctly rewires the loader entry to reference it, leaves `kanxeo-bzImage-a` untouched, and boots successfully from it on a second, independent power-on -- proof the per-slot resolution works, not just that bytes landed somewhere. Deliberately reuses the same real kernel bytes as both the initial slot-A kernel and the "new" `kernel_path` source (mirroring `test_boot_ab.c`'s own precedent for squashfs) rather than requiring a second, genuinely different, real bootable kernel build. 3 consecutive clean runs; full pre-existing regression suite re-run clean, including the QEMU-based `test_boot`/`test_boot_ab`/`test_installer`. Zero compiler warnings.

### Phase 16 (part 1): host + package update mechanism

Raised directly by the user: a mechanism to update the running host and its packages, matching the existing A/B configuration. The A/B rollback machinery (ADR-0014) existed since Phase 11, but nothing could ever write a new image into the inactive slot on a live system -- root B stayed genuinely empty until now. See ADR-0031.

#### Added
- `daemon/src/main.c`: `g_slot`/`g_bind_addr` promoted from `main()` locals to file-scope statics (same precedent as `g_epfd`); new `ROOT_A_DEVICE "/dev/vda2"`/`ROOT_B_DEVICE "/dev/vda3"` macros, matching `ESP_DEVICE`/`CONFIG_DEVICE`/`CONTAINERS_DEVICE`'s existing fixed-device precedent; new `write_file_to_device()` (mirrors `kanxeo-install.c`'s `copy_file()`); new `do_system_update()`/`handle_system_update()` implementing `POST /v1/system/update` (writes a fresh squashfs onto the inactive slot, stages a fresh loader entry with a fresh Automatic Boot Assessment counter -- does NOT itself reboot); new `--test-update-image=` daemon flag (same precedent as `--simulate-unhealthy-boot`) making `do_system_update()` observable from a real QEMU guest's serial console for testing.
- `daemon/src/pkg.c`/`daemon/include/pkg.h`: new `pkg_find_update_candidate()`, reusing `write_pkg_json()`'s own fresh-recipe-vs-installed-version comparison; new `handle_pkg_update_all()` implementing `POST /v1/pkg/update-all` (starts one real upgrade for the first drifted installed package, or reports nothing to update).
- `cli/src/main.c`: `update --image=PATH` (top-level, alongside `shutdown`/`reboot`); `pkg update-all`.
- `docs/api/openapi.yaml`: new `/system/update` and `/pkg/update-all` paths.
- `docs/adr/0031-host-and-package-update-mechanism.md`.
- `test/test_system_update.c`, new: proves `POST /system/update`'s validation logic (no `--slot=`, missing/unreadable `image_path`, bad squashfs magic) against a plain dev daemon -- none of these checks need a real device.
- `test/test_boot_update.c`, new: proves the real device-write/loader-entry path inside a real QEMU guest, via `--test-update-image=` against a raw scratch partition holding a genuinely different second squashfs; confirms the written bytes are byte-exact, the loader entry lands on the ESP, and a second, independent QEMU boot actually boots the freshly-updated slot.
- `test/test_pkg.c`: extended with an `update-all` scenario -- nothing-to-update when no package has drifted, exactly one real upgrade job started once a recipe's version is bumped, nothing-to-update again once drained.

Verified two ways matching the two different risk profiles: the pure validation logic needs no real device at all (`boot_init()`, the only thing that would need one, only runs under `--init-mode`) and is proven against a plain dev daemon; the real write needs a real virtio-blk disk and is proven inside QEMU, including a genuine second, independent boot landing on the freshly-written slot -- not just that the write syscall succeeded. `pkg update-all` proven against a real bumped recipe, respecting (not working around) the existing v1 single-install-in-flight constraint. Full clean rebuild, zero warnings; full pre-existing regression suite, including the QEMU-based tests, re-run clean.

### Phase 15: per-container config files + generalized sysctls

Raised directly by the user: several containers built from the same image, each needing its own config file and startup script, plus sysctls beyond `ip_forward` (`rp_filter`, for policy-based routing). There was no operator-facing way to put a file into a container at all before this. See ADR-0030.

#### Added
- `include/container.h`: `struct container_sysctl`, `CONTAINER_MAX_SYSCTLS`/`CONTAINER_SYSCTL_KEY_MAX`/`CONTAINER_SYSCTL_VALUE_MAX`, `sysctls[]`/`sysctl_count` on `container_spec`; `CONTAINER_MAX_FILES`/`CONTAINER_FILE_PATH_MAX`/`CONTAINER_FILE_CONTENT_MAX` (files never touch `container_spec` -- daemon-side only, see below).
- `src/container_net.c`/`include/internal.h`: new `container_net_apply_sysctl()`, mirroring `container_net_enable_ip_forward()` exactly (child-side, writes `net.*` sysctls into the container's own already-entered netns).
- `src/container.c`: child-side loop applying `spec->sysctls[]`, right beside the existing `ip_forward` call.
- `daemon/src/main.c`: `create_container_from_body()` gains `"files"` (validated -- absolute path, no `.`/`..` components, bounded content/mode -- then written directly into the container's own overlay `upperdir` on the daemon's own host process, before `registry_create()`/`clone3()` ever runs) and `"sysctls"` (validated -- `net.*` keys only, a real security boundary since most other sysctls aren't namespace-isolated -- threaded into `spec`).
- `daemon/include/registry.h`/`daemon/src/registry.c`: `registry_entry` gains `file_paths[]`/`file_count` (paths only) and `sysctls[]`/`sysctl_count`; `registry_create()` gains `file_paths`/`file_count` parameters; `registry_write_json_one()` echoes both.
- `cli/src/main.c`: `run --file=CONTAINER_PATH=LOCAL_PATH[:MODE]` (repeatable, reads the local file), `run --sysctl=KEY=VALUE` (repeatable); `ps`/`inspect` gain `files=N`/`sysctls=N` counts.
- `docs/api/openapi.yaml`: new `ConfigFile` schema; `files`/`sysctls` on `ContainerCreateRequest`/`Container`.
- `docs/adr/0030-per-container-config-files-and-sysctls.md`.

Verified against a live daemon: a real file lands at the correct host path with the correct content and mode; path-traversal (`../../etc/passwd`) and a missing leading `/` both `400`; a `net.ipv4.conf.all.rp_filter` sysctl confirmed via `nsenter` into the container's own `/proc/<pid>/ns/net` to land inside *that* netns specifically (value `0`) while the host's own value stayed completely untouched (`2`) -- real per-netns isolation, not just a successful write syscall; a non-`net.*` key and a malformed key (`net..foo`) both `400`. Because `restart:"always"` already persists the whole request body verbatim (ADR-0025), `files`/`sysctls` are automatically re-staged/re-applied correctly on every future boot/crash-restart replay, for free. Full pre-existing regression suite re-run clean. Zero compiler warnings.

### Phase 14 (part 2): GPU kernel driver, firmware staging, KFD discovery

Part 1 built discovery and grants, but the kernel had no GPU driver built in at all, and nothing staged the firmware it needs. See ADR-0029.

#### Added
- `image/kernel/qemu-part1.config`: `CONFIG_FW_LOADER`/`CONFIG_DRM`/`CONFIG_DRM_KMS_HELPER`/`CONFIG_DRM_AMDGPU`/`CONFIG_HSA_AMD` -- verified via a real kernel source fetch + `make allnoconfig` + `merge_config.sh` + `make olddefconfig`, confirming every fragment symbol (not just the new ones) lands as `=y`, then a full `make bzImage`.
- `image/src/mkbootroot.c`: new required 5th argv, `firmware_dir` (empty string = skip); stages it into `<image_root>/lib/firmware/amdgpu` via the existing `copy_dir_files()`, reused verbatim.
- `test/test_boot.c`, `test/test_boot_ab.c`, `test/test_installer.c`: each `mkbootroot` call updated to pass `""` for the new argument.
- `test/test_mkbootroot_firmware.c`, new: proves the staging mechanism against a synthetic scratch directory -- `firmware_dir=""` is a no-op, a real directory's files land verbatim under `lib/firmware/amdgpu`, an unreadable-but-explicitly-requested path fails the whole build.
- `daemon/src/device.c`: `enumerate_gpu()` gains a `/sys/class/kfd/kfd` pass, emitting `gpu:<idx>:kfd` -- the shared ROCm/HSA compute device, entirely separate from the DRM nodes part 1 already found -- for every discovered GPU group.
- `README.md`: new firmware-fetch recipe (`git clone --filter=blob:none --sparse` of upstream `linux-firmware`, `git sparse-checkout set amdgpu`) in the installer-ISO build walkthrough; updated `mkbootroot` example invocation; `device ls`/`--device=` walkthrough mentions `gpu:` ids.
- `docs/adr/0029-gpu-kernel-driver-firmware-and-kfd.md`.

Firmware is deliberately not vendored into this repo (fetched fresh at build time instead, per this project's own kernel-source-fetch precedent, not automated into `make`) -- confirmed directly with the user after finding this dev sandbox has no amdgpu firmware anywhere and the distro package isn't even available here. `copy_dir_files()` reuse for staging relies on `linux-firmware`'s own `amdgpu/` directory being flat -- confirmed directly against the real upstream repo (675 files, zero nested), not just assumed. Verified to the extent possible without real GPU hardware: the kernel config genuinely compiles (a real `make bzImage`, not just `make olddefconfig`) and the resulting kernel was booted for real -- `test_boot`/`test_boot_ab`/`test_installer` all re-run clean against it; `mkbootroot`'s new staging path proven against a synthetic scratch directory, 3 consecutive clean runs; `firmware_dir=""` confirmed a no-op. Full pre-existing regression suite re-run clean, including the 3 QEMU-based tests this time since `mkbootroot.c` itself changed. Zero compiler warnings.

### Phase 14 (part 1): GPU passthrough discovery + grouped device grants

ADR-0017 explicitly named GPU passthrough as the next consumer of its own PCI/USB passthrough mechanism; a GPU needs several `/dev` nodes granted together, which the existing strictly-one-id-per-node model couldn't express. See ADR-0028.

#### Added
- `daemon/src/device.c`/`daemon/include/device.h`: new `"gpu"` bus (`enumerate_gpu()`), walking `/sys/class/drm` and grouping multiple DRM nodes (`cardN`/`renderDN`) belonging to one physical GPU under a stable `gpu:<idx>` (resolved via each node's `device` symlink back to its parent PCI address); `enumerate_pci_one()`'s placeholder-suppression extended to PCI class `03` (display controller) alongside the existing `02` (network controller), so a GPU isn't also listed generically under `pci:`. New, additive `device_find_group()` -- `device_find()` itself untouched -- resolves a bare `gpu:<idx>` logical id into every currently assignable member node at once.
- `daemon/src/main.c`: `create_container_from_body()`'s device-grant loop reworked from a strict 1:1 request-index-to-grant-slot mapping to a decoupled read/write-index loop (`device_find_group()`), since one requested id can now expand into several grants; bounds-checked against `CONTAINER_MAX_DEVICES` incrementally.
- `cli/src/main.c`: `device ls`/`run --device=` usage text documents grouped GPU ids (no functional CLI change -- both were already fully data-driven/generic).
- `docs/api/openapi.yaml`: `Device.bus` gains `gpu`; `ContainerCreateRequest.devices` documents grouped-id expansion and that `GET` echoes real granted members, not the requested id.
- `docs/adr/0028-gpu-passthrough-grouped-device-grants.md`.

Verified over real HTTP against a live daemon, to the extent possible without real GPU hardware (confirmed directly: this sandbox's `/sys/class/drm` is empty, no driver bound to anything): `GET /v1/devices` returns cleanly with zero `gpu:` entries; `POST /v1/containers` with `"devices":["gpu:0"]` 400s via a genuine exercise of `device_find_group()`'s empty-expansion path; every pre-existing `usb:`/`pci:`/`net:` grant scenario re-verified unchanged. 3 consecutive clean runs; full pre-existing regression suite (all 17 binaries) re-run clean. Zero compiler warnings. Kernel driver enablement and firmware staging are deliberately a separate part 2, not included here.

### Phase 13 (part 3): restart policy expansion + backoff

Part 1 deliberately shipped only `restart: "always"`/`"no"` and a single fixed 2s crash-restart delay, deferring the rest. See ADR-0027.

#### Added
- `daemon/include/containerdef.h`/`daemon/src/containerdef.c`: `struct container_def` gains persisted `restart_policy`/`restart_delay_seconds`/`stopped` and non-persisted `consecutive_failures`; `containerdef_add()` gains matching parameters and explicitly clears `stopped`/`consecutive_failures` on every call; new `containerdef_set_stopped()`; `parse_persisted_entry()` defaults an absent `restart_policy` to `"always"` for backward compatibility with parts 1-2's own `container_defs.json`.
- `daemon/include/registry.h`/`daemon/src/registry.c`: `struct registry_entry` gains `started_at`, set in `registry_create()`.
- `daemon/src/main.c`: `POST /v1/containers`'s `restart` enum expands to `always`/`on-failure`/`unless-stopped`/`no`; new optional `restart_delay_seconds` (1-300, default 2); `on-failure` skips the crash-restart timer only for a clean exit; new `handle_stop()` + `POST /v1/containers/{name}/stop` routing (reuses `registry_remove()` verbatim, sets the new persisted `stopped` flag, does not touch the persisted definition/DNS/PKI ownership); `containerdef_autostart_all()` skips a `stopped` `unless-stopped` definition; `handle_restart_timer_event()` checks `stopped` unconditionally (any policy) to close a stop-during-pending-delay race; `arm_restart_timer()` takes a per-call delay; `handle_container_event()` computes that delay via automatic exponential backoff (doubling per consecutive failure, capped at 30s, reset after 30s of stable uptime).
- `daemon/src/registry.c`: `registry_write_json_one()` echoes `restart` (now the real policy), `restart_delay_seconds`, and `stopped`.
- `cli/src/main.c`: `run --restart=always|on-failure|unless-stopped [--restart-delay=N]`; new `stop NAME` subcommand; `ps`/`inspect` gain `delay=`/`stopped=` columns.
- `docs/api/openapi.yaml`: `restart` enum expansion (request + response), new `restart_delay_seconds`/`stopped` properties, new `POST /containers/{name}/stop` path, `DELETE` description updated to point at it.
- `docs/adr/0027-restart-policy-expansion-and-backoff.md`.

Verified end-to-end against a live, restarted daemon: `on-failure` confirmed to skip a restart after a clean exit but perform one after a crash; `restart_delay_seconds` confirmed honored; a `stop` mid-pending-delay-window confirmed to permanently cancel that restart; backoff confirmed genuinely growing across two consecutive fast-crash cycles, contrasted against a stable-running container landing at the un-doubled base delay instead (proving the reset branch); `unless-stopped` confirmed to stay down across a real daemon restart while an identically-stopped `always` container came back. A real bug in the *test's own* first-draft timing expectations (not the daemon) was found and fixed during verification -- see ADR-0027's Consequences for the full account. 3 consecutive clean runs; full pre-existing regression suite (all 17 binaries) re-run clean. Zero compiler warnings.

### Phase 13 (part 2): TCP readiness checks for `depends_on`

Part 1's `depends_on` was start-order only -- a dependency being "started" didn't mean it was actually ready to serve. See ADR-0026.

#### Added
- `daemon/include/containerdef.h`/`daemon/src/containerdef.c`: `struct container_def` gains cached `has_readiness`/`readiness_tcp_port`/`readiness_timeout_seconds`, parsed once at add/load time; `containerdef_add()` gains matching parameters; persisted alongside `depends_on` in `container_defs.json`.
- `daemon/src/main.c`: `POST /v1/containers` gains an optional `"readiness": {"tcp_port": N, "timeout_seconds": N}` object (requires at least one network attachment, 400 otherwise); new `wait_for_tcp_ready()` -- a plain, blocking, retried `connect()` against the container's own primary network address, no non-blocking-connect-plus-`poll()` needed since the destination is always a directly L2-adjacent bridge network; called only from `containerdef_autostart_all()`, never from the live `POST` path or crash-restart. A readiness check that never succeeds within its own timeout logs a warning and lets boot proceed anyway (best-effort).
- `daemon/src/registry.c`: `registry_write_json_one()` echoes `readiness` (object or `null`) alongside `restart`/`depends_on`, sourced live from the same `containerdef_find()` lookup.
- `cli/src/main.c`: `run --readiness-tcp-port=N [--readiness-timeout=N]`; `ps`/`inspect` gain a `readiness=` column.
- `test/tcp_listen_child.c`, new fixture: binds+listens only after a deliberate startup delay, proving a dependent genuinely waits for readiness rather than merely for process start.
- `docs/api/openapi.yaml`: new `Readiness` schema; `ContainerCreateRequest.readiness`, `Container.readiness`; `depends_on`'s stale "no readiness concept exists" wording corrected.
- `docs/adr/0026-tcp-readiness-checks-for-depends-on.md`.

Verified end-to-end against a live, restarted daemon: a dependent's autostart measurably waited on its dependency's real 2s-delayed listen socket (the whole restart-to-healthy window at least 1s, dominated by that delay); a readiness check pointed at a port nobody ever listens on (1s timeout) still let its dependent autostart afterward, proving best-effort holds and boot never hangs; `readiness` without a network attachment rejected with 400 at creation. 3 consecutive clean runs; full pre-existing regression suite re-run clean. Zero compiler warnings.

### Phase 13 (part 1): persisted, auto-restarting containers (`restart: "always"`, `depends_on`)

Containers have been in-memory only since Phase 3 -- for a real deployment (routers, a NAS, a git-repo container, an ad-blocking DNS, some depending on others), that's the real blocker. See ADR-0025.

#### Added
- `daemon/src/containerdef.c`/`daemon/include/containerdef.h`, new: persists a container's exact create request when `"restart": "always"` is set, replayed verbatim at boot and after any unprompted exit. `containerdef_resolve_order()` mirrors `pkg.c`'s own `resolve_chain()` shape (DFS + cycle detection) for `depends_on`.
- `daemon/src/main.c`: `POST /v1/containers` gains `restart`/`depends_on` fields; `handle_create()` split into a thin wrapper + reusable `create_container_from_body()` (zero HTTP coupling), shared by the REST path, new `containerdef_autostart_all()` (boot-time, right after `confirm_boot()`), and a new timerfd-based crash-restart with a real, measured delay (`CONN_RESTART_TIMER`, the reactor's first-ever timer) -- never a blocking `sleep()`, which would freeze the whole single-threaded event loop. `DELETE /v1/containers/{name}` now also permanently removes the persisted definition.
- `daemon/src/registry.c`: `registry_write_json_one()` sources `restart`/`depends_on` response fields live from `containerdef_find()`.
- `cli/src/main.c`: `run --restart=always`, repeatable `--depends-on=NAME`; `ps`/`inspect` echo `restart` status.
- `docs/api/openapi.yaml`: `ContainerCreateRequest.restart`/`depends_on`, `Container.restart`/`depends_on`, updated `DELETE /containers/{name}` description.
- `docs/adr/0025-persisted-auto-restarting-containers.md`.

Two real bugs found and fixed during verification, not before. First: registering the same container's pidfd with epoll a second time (present in both new call sites on top of the existing call already inside the refactored core function) aborts the daemon outright -- confirmed directly via a real daemon restart with a persisted container, fixed by leaving the one call where it already was. Second, surfaced by the full regression sweep rather than the new test's own assertions: `handle_delete()` required a container to be currently live before it would even check for a persisted definition, so a `restart:"always"` definition that had never once successfully autostarted (a `depends_on` cycle or unknown dependency) could never be `DELETE`d at all -- found via cross-test log pollution (`container_defs.json` leaking stuck definitions from `test_container_restart.c` into every other test's daemon startup on the same host); fixed by decoupling "stop the live instance" from "remove the persisted definition" into two independent steps.

Verified end-to-end: a fast-exiting `restart: "always"` container observed crash-looping with a real, measured ~2.0s gap (not instant) between each exit and the next restart; a daemon restart brought a persisted container back with a fresh pid; `DELETE` confirmed to stop it and keep it gone across a subsequent restart, including for a definition that never successfully autostarted. The entire pre-existing REST-facing test suite re-run unchanged after the `handle_create()` refactor, zero regressions, before any new test was added. Zero compiler warnings.

### Phase 12 part 7 follow-up: NIC passthrough negative-path test coverage

This dev sandbox has no real, physically-backed NIC visible in its own root netns (ADR-0022), so the positive "grant a real interface, watch it work" path stays unprovable here. Added what *is* provable without real hardware, confirmed with the user directly rather than left unaddressed.

#### Added
- `test/test_daemon_devices.c`: a real, kernel-backed veth pair proves `GET /v1/devices` never lists a software-created interface under `bus: "net"`, and that `POST /v1/containers` correctly 400s an `interfaces` entry naming that same veth.

3 consecutive clean runs. `docs/adr/0022-...md` and `docs/ROADMAP.md` updated to point at this coverage precisely, so the boundary between "proven" and "not provable here" stays exact.

### Phase 12 (part 9): image lifecycle endpoints

Before this part, images were purely implicit -- a directory that came into existence at install time (`base`) or as a side effect of the first `pkg install` targeting it. No way to list, create, or delete one. See ADR-0024.

#### Added
- `daemon/src/image.c`/`daemon/include/image.h`: `GET`/`POST /v1/images`, `GET`/`DELETE /v1/images/{name}` -- filesystem-backed, no separate persisted state.
- `daemon/src/registry.c`/`daemon/include/registry.h`: `registry_entry.image[]`, populated via a new explicit `image` parameter on `registry_create()`; new `registry_image_in_use()`. `GET /v1/containers` responses gain an `"image"` field.
- `daemon/src/pkg.c`/`daemon/include/pkg.h`: `pkg_image_has_packages(const char *image)`.
- `cli/src/main.c`: `image create --name=NAME`, `image ls`, `image rm NAME`.
- `docs/api/openapi.yaml`: `/images` paths, `Image`/`ImageCreateRequest` schemas, `Container.image`.
- `docs/adr/0024-image-lifecycle-endpoints.md`.
- `test/test_images.c`, new.

Verified over real HTTP against a live daemon: create/list/get, 409 on duplicate create, runtime seeded immediately after create (ADR-0023), 400 deleting `base`, 404 for an unknown image, 409 deleting an image a running container references (204 once that container is gone, directory genuinely removed), 409 deleting an image with a package still tracked against it. 3 consecutive clean runs. Zero warnings; full regression sweep re-run clean.

### Phase 12 (part 8): C-runtime seeding generalized to every image

ADR-0019's own Consequences section named this gap directly: runtime seeding only ever landed in `images/base/rootfs` -- a non-default image (e.g. `router`, from part 5's own per-image `pkg install`) had no way to execve() anything installed into it. See ADR-0023.

#### Added
- `image/src/mkbootroot.c`: also stages `libtinfo.so.6` into the control-plane squashfs (previously only `ld.so`/`libc.so.6`) -- fixes a real risk found while designing this part: the natural "copy from wherever kanxeod is running" source would otherwise have found nothing on any real deploy, working in this dev sandbox only by coincidence.
- `daemon/src/pkg.c`/`daemon/include/pkg.h`: `pkg_seed_image_runtime(const char *image)` -- copies the C runtime into any image's rootfs, idempotent, tolerant of a missing source file; called from `pkg_build_completed()` for whatever image a job merges into.
- `docs/adr/0023-per-image-runtime-seeding.md`.

Verified: `test/test_pkg.c`'s existing per-image scenario extended to confirm all three runtime files land in a freshly created `router` image after its first install -- 3 consecutive clean runs. `kanxeo-install.c`/`mkinstalleriso.c` confirmed byte-for-byte untouched; zero warnings.

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

#### Added
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

#### Notes
- Verified, no code change required. Git state: clean, fully pushed, no stashes, no dangling branches, no stray backup/patch files anywhere in the tree.
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

## Phase 8 (parts 1–2): DNS records + containerized dnsmasq resolution + automatic container registration

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

#### Notes
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

#### Notes
- Scope boundary, stated not silently skipped: whether the dashboard renders and behaves correctly in a real browser wasn't automated-tested — no headless-browser/Node toolchain exists in this project, and adding one for one small dashboard would repeat the exact dependency-cost trade-off ADR-0010 decided against. Checked instead: `node --check` (pre-installed system tool, not a new project dependency) on `app.js`, and every DOM ID it references confirmed present in `index.html`. A real browser check at `http://127.0.0.1:7620/` is the remaining step.

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
