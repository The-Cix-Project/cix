# Architecture Decision Records

See [0000-adr-process.md](0000-adr-process.md) for what an ADR is for and how it's kept here (short-lived reasoning that lives forever, append-only, distinct from `docs/ROADMAP.md`'s what-shipped narrative).

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
