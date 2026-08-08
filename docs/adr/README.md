# Architecture Decision Records

See [0000-adr-process.md](0000-adr-process.md) for what an ADR is for and how it's kept here (short-lived reasoning that lives forever, append-only, distinct from `docs/roadmap/ROADMAP.md`'s what-shipped narrative).

| ADR | Title | Status |
|---|---|---|
| [0000](0000-adr-process.md) | Recording architecture decisions | Accepted |
| [0001](0001-tcc-exclusive-toolchain.md) | TCC exclusively, dynamically linked against system glibc | Accepted |
| [0002](0002-raw-syscalls-for-missing-glibc-wrappers.md) | Raw syscalls where glibc has no wrapper | Accepted |
| [0003](0003-clone3-atomic-cgroup-placement.md) | clone3 + CLONE_INTO_CGROUP + CLONE_PIDFD over legacy clone() | Accepted |
| [0004](0004-overlayfs-dedicated-lowerdir.md) | OverlayFS lowerdir is a dedicated, purpose-built tree | Accepted |
| [0005](0005-api-first-mandate.md) | API-First Mandate: the REST daemon is the only path to the runtime | Accepted |
| [0006](0006-openapi-spec-format.md) | OpenAPI 3.0 YAML as the API spec format | Accepted |
| [0007](0007-hand-rolled-daemon-no-external-libs.md) | Hand-rolled HTTP/JSON/reactor in the daemon, no external libraries | Accepted |
| [0008](0008-tcc-pragma-pack-for-kernel-abi-structs.md) | Use #pragma pack, not __attribute__((packed)), for kernel-ABI structs under TCC | Accepted |
| [0009](0009-cloexec-daemon-fds-before-clone3.md) | CLOEXEC every daemon-owned fd before any clone3() call | Accepted |
| [0010](0010-vanilla-web-dashboard-no-build-step.md) | Vanilla HTML/CSS/JS dashboard, no framework, no build step, served by kanxeod | Accepted |
| [0011](0011-rtnetlink-control-plane-over-kernel-bridge.md) | rtnetlink control plane over the kernel's own bridge/veth, not a userspace switch | Accepted |
| [0012](0012-networks-json-persistence.md) | Atomically-rewritten flat file for network persistence, not a database | Accepted |
| [0013](0013-proc-pid-root-for-live-container-file-writes.md) | /proc/<pid>/root/ for writing into a running container's filesystem, not raw upperdir or setns() | Accepted |
| [0014](0014-squashfs-ab-root-with-native-boot-counting.md) | Read-only squashfs A/B root with systemd-boot's native boot counting, not a writable root or hand-rolled rollback | Accepted |
| [0015](0015-shim-mok-secure-boot-signing.md) | shim + MOK enrollment for Secure Boot on the installed system, installer media stays unsigned | Accepted |
| [0016](0016-reboot-syscall-for-kanxeod-shutdown.md) | kanxeod calls reboot(2) as PID 1, exposed via REST, SIGTERM/SIGINT default to poweroff | Accepted |
| [0017](0017-ebpf-cgroup-device-filter-for-hardware-passthrough.md) | BPF_CGROUP_DEVICE for device passthrough enforcement, not a blanket eBPF ban | Accepted |
| [0018](0018-containers-partition-for-base-dir-persistence.md) | The real kanxeo-containers partition backs BASE_DIR, not a fresh tmpfs every boot | Accepted |
| [0019](0019-runtime-libs-seeded-at-install-time.md) | The shared "base" image's C runtime is seeded at install time, not by pkg or at boot | Accepted |
| [0020](0020-per-image-pkg-install-compound-key.md) | pkg install targets an explicit image, tracked by a (name, image) compound key | Accepted |
| [0021](0021-explicit-network-ip-override.md) | A container's network attachment IP can be explicitly chosen, not only auto-allocated | Accepted |
| [0022](0022-real-nic-passthrough-netns-move.md) | Real network interface passthrough via direct netns move, with fd-anchored teardown | Accepted |
| [0023](0023-per-image-runtime-seeding.md) | The C runtime is seeded into every image, not only "base" | Accepted |
| [0024](0024-image-lifecycle-endpoints.md) | Image lifecycle as a filesystem-backed REST resource, no separate persistence | Accepted |
| [0025](0025-persisted-auto-restarting-containers.md) | Persisted, auto-restarting containers (`restart: "always"`, `depends_on`) | Accepted |
| [0026](0026-tcp-readiness-checks-for-depends-on.md) | TCP readiness checks for `depends_on` | Accepted |
| [0027](0027-restart-policy-expansion-and-backoff.md) | Restart policy expansion (`on-failure`, `unless-stopped`) and crash-restart backoff | Accepted |
| [0028](0028-gpu-passthrough-grouped-device-grants.md) | GPU passthrough: grouped multi-node device grants | Accepted |
| [0029](0029-gpu-kernel-driver-firmware-and-kfd.md) | GPU kernel driver, firmware staging, and KFD discovery | Accepted |
| [0030](0030-per-container-config-files-and-sysctls.md) | Per-container config files and generalized sysctls | Accepted |
| [0031](0031-host-and-package-update-mechanism.md) | Host OS update mechanism (write to inactive A/B slot), and automatic package updates | Accepted |
| [0032](0032-per-slot-kernel-updates.md) | Per-slot kernel files, extending POST /system/update to cover bzImage | Accepted |
| [0033](0033-platform-state-backup-restore.md) | Platform state backup/restore -- configuration only, not workload data, never PKI | Accepted |
| [0034](0034-console-login-via-supervised-kanxeoctl.md) | Console login via a kanxeod-supervised kanxeoctl, not a general shell | Accepted |
| [0035](0035-portable-toolchain-artifact-for-pkg-bootstrap.md) | Portable toolchain artifact for pkg bootstrap, not just a live-host copy | Accepted |
| [0036](0036-multi-source-package-recipes.md) | Multi-source package recipes | Accepted |
| [0037](0037-network-gateway-optional.md) | A network's host-owned gateway becomes optional, default flips to gateway-less | Accepted |
| [0038](0038-vlan-and-physical-nic-bridge-attachment.md) | VLAN via 802.1q sub-interfaces, physical NIC attachment via the existing rtnl_link_set_master | Accepted |
| [0039](0039-package-recipes-seeded-at-install-time.md) | Package recipes seeded at install time, same mechanism as ADR-0019 | Superseded by [0040](0040-package-recipes-managed-via-rest-api.md) |
| [0040](0040-package-recipes-managed-via-rest-api.md) | Package recipes are managed via a real REST API, not baked into the installer ISO | Accepted |
| [0041](0041-container-image-baseline-fhs-layout.md) | Container images get a real, fixed baseline FHS layout at seed time | Accepted |
| [0042](0042-installer-dual-console-pty-relay.md) | The installer relays interactive I/O through a PTY across both consoles at once | Accepted |
| [0043](0043-container-console-exec-websocket.md) | An interactive shell into a running container, over a hand-rolled WebSocket | Accepted |
| [0044](0044-daemon-data-dir-test-isolation.md) | `kanxeod --data-dir=` for test/production state isolation | Accepted |
| [0045](0045-container-start-pause-lifecycle.md) | Container start/pause: cgroup-freeze lifecycle completeness | Accepted |
| [0046](0046-site-scoped-dns-pki-naming.md) | Site-scoped DNS/PKI naming: a real, configurable convenience, not enforcement | Accepted |
| [0047](0047-two-tier-pki-intermediate-ca.md) | Two-tier PKI: an intermediate CA for day-to-day signing | Accepted |
| [0048](0048-persistent-device-name-mappings.md) | Persistent device name mappings: exact vs. vendor/model resolution | Accepted |
| [0049](0049-pki-ca-reset-regeneration.md) | PKI CA reset/regeneration: reissue-in-place, not destroy | Accepted |
| [0050](0050-auto-issued-host-pki-cert.md) | Auto-issued host PKI cert: a fixed record name, not the FQDN itself | Accepted |
| [0051](0051-ca-trust-staged-into-images.md) | CA trust chain staged into every image at creation time | Accepted |
| [0052](0052-server-side-default-name-qualification.md) | Server-side default DNS/PKI name qualification (revises ADR-0046) | Accepted |
| [0053](0053-auto-maintained-instance-dns-record.md) | Auto-maintained instance DNS record | Accepted |
| [0054](0054-host-side-container-stats.md) | Host-side per-container stats: raw point-in-time snapshot, no server history | Accepted |
| [0055](0055-container-file-read-endpoint.md) | Container file-read REST endpoint: raw bytes, running-vs-exited path resolution | Accepted |
| [0056](0056-hostbuild-artifact-mechanism.md) | Hostbuild artifact mechanism: a second pkg-install mode that harvests a standalone artifact instead of merging into an image | Accepted |
| [0057](0057-self-hosted-toolchain-and-control-plane-rebuild.md) | Self-hosted toolchain and control-plane rebuild: tcc.recipe, kanxeo.recipe, server-side mkbootroot | Accepted |
| [0058](0058-host-management-network-unification.md) | The host's own management address becomes a real, API-managed network | Accepted |
| [0059](0059-openssl-https-listener.md) | OpenSSL-backed HTTPS listener, reusing the PKI host cert, via a small TLS side-table | Accepted |
| [0060](0060-cpu-bandwidth-and-affinity-api-exposure.md) | Expose cpu.max and cpuset.cpus through the API/CLI, no reinterpretation layer | Accepted |
| [0061](0061-kernel-module-loading.md) | Kernel module loading: root-critical drivers stay built-in, only network/USB become modules | Accepted |
| [0062](0062-ext4-project-disk-quotas.md) | Disk quotas: real ext4 project-quota enforcement, permanent project-id assignment | Accepted |
| [0063](0063-iso-self-build-toolchain.md) | ISO self-build: a real, from-source GRUB2/xorriso/mtools/sbsigntools toolchain | Accepted |
| [0064](0064-rest-driven-iso-assembly.md) | REST-driven ISO assembly: closing the API-First Mandate gap ADR-0063 left open | Accepted |
| [0065](0065-pkg-bootstrap-url-fetch.md) | pkg bootstrap URL-fetch: closing a real gap in ADR-0031's own "operator scp's it" assumption | Accepted |
| [0066](0066-kernel-routing-table-diagnostic.md) | A real kernel routing-table diagnostic: closing the last blind spot in "the daemon is the only way to inspect a running box" | Accepted |
| [0067](0067-network-address-field-rename.md) | Rename a network's `gateway`/`has_gateway` field to `address`/`has_address` | Accepted |
| [0068](0068-dedicated-daemon-bind-ip.md) | A dedicated daemon bind IP, decoupled from the management network's own address | Accepted |
| [0069](0069-host-swap-file.md) | A single, on-demand host swap file: direct swap-header write, no `mkswap` dependency | Accepted |
| [0070](0070-consolidated-log-store.md) | A consolidated, API-accessible, size-capped log store: kernel dmesg + kanxeod diagnostics + per-request audit trail | Accepted |
| [0071](0071-disk-format-mount.md) | Disk format + mount as a separate, explicitly-confirmed action; a non-execve()ing job child for its two sequential steps | Accepted |
| [0072](0072-log-message-cap-and-min-level.md) | Log message cap raised 512->4096, build-output capture switched head->tail, settable write-time minimum severity | Accepted |
| [0073](0073-host-stats-endpoint.md) | Host-wide stats endpoint (GET /v1/system/stats): load/CPU/memory/disk/network, mirroring per-container stats | Accepted |
| [0074](0074-pressure-stall-information.md) | Pressure-stall information (PSI) in host and per-container stats: cpu/io/memory.pressure | Accepted |
| [0075](0075-icmp-reachability-endpoint.md) | Reachability endpoint (GET/POST /v1/system/ping): real, hand-rolled ICMP echo | Accepted |
| [0076](0076-host-dns-resolver-config.md) | Host DNS resolver config (GET/PUT /v1/system/resolv) + a real /etc/resolv.conf bind-mount fix | Accepted |
| [0077](0077-health-boot-identity-split.md) | Split GET /health into liveness + GET /system/boot identity | Accepted |
| [0078](0078-from-source-host-tools-bootstrap.md) | From-source host tools bootstrap (mkbootroot.c's shelled-out binaries) | Accepted |
| [0079](0079-cgroup-controller-delegation.md) | cgroup v2 controller delegation at daemon startup (real-PID-1/no-systemd container-create 500 fix) | Accepted |
| [0080](0080-container-diagnostics-visibility.md) | Container creation/exit diagnostics visibility -- real errno text, not just numeric codes | Accepted |
| [0081](0081-cgroup-controller-delegation-atomic-write-fix.md) | cgroup controller delegation: fix the atomic-write regression ADR-0079 introduced | Accepted |
| [0082](0082-kernel-smp-missing.md) | CONFIG_SMP was never enabled -- uniprocessor kernel, and the real root cause of the cpuset gap | Accepted |
| [0083](0083-mkbootroot-ld-linux-second-copy.md) | mkbootroot's own control-plane root was missing the second ld-linux copy | Accepted |
| [0084](0084-mkbootroot-mksquashfs-unreachable-on-real-host.md) | mkbootroot's own mksquashfs exec target was never reachable on a real installed host | Accepted |
| [0085](0085-mkbootroot-source-paths-not-merged-usr.md) | The real, final root cause: mkbootroot's own source reads assumed a merged-usr host | Accepted |
| [0086](0086-pkgbuild-stop-bypasses-pkg-completion.md) | Stopping __pkgbuild via POST .../stop left pkg.c's job lock stuck forever | Accepted |
