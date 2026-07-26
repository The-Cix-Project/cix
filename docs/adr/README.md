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
