# 0008 — Use #pragma pack, not __attribute__((packed)), for kernel-ABI structs under TCC

## Status

Accepted

## Context

Phase 3's daemon (ADR-0007) intermittently segfaulted (roughly half of runs) on its very first request, with no obvious cause in the C source and no clean reproduction on demand — a genuine Heisenbug requiring `gdb`/`strace` bisection to pin down (see `docs/roadmap/ROADMAP.md` Phase 3).

Root cause: the kernel ABI for `struct epoll_event` is 12 bytes (`uint32_t events` at offset 0, an 8-byte `data` union at offset 4, no padding) — `<sys/epoll.h>` marks it `__attribute__((packed))` specifically because it would otherwise naturally align to 16 bytes with `data` at offset 8. **TCC ignores `__attribute__((packed)) `entirely** — confirmed not just on the system header but on a minimal struct written by hand with the same attribute. Every `epoll_ctl()`/`epoll_wait()` call was therefore silently corrupting its `data` field: the kernel writes/reads 12-byte packed records, while TCC's compiled code was reading/writing them at 16-byte stride with `data` at the wrong offset. The intermittent, timing-dependent nature came from `data` (which we use to carry a `void *` connection pointer) landing on whatever garbage happened to occupy the misaligned bytes.

`#pragma pack(push, 1)` / `#pragma pack(pop)`, by contrast, **is** honored correctly by TCC (verified directly: a struct wrapped in it compiles to the expected packed size).

## Decision

`include/linux_compat.h` defines `struct cix_epoll_event` (and matching `union cix_epoll_data`) using `#pragma pack`, byte-identical to the real kernel ABI, plus `cix_epoll_ctl()`/`cix_epoll_wait()` wrapper functions that cast our pointer to `struct epoll_event *` when calling the glibc functions. This is safe because glibc's `epoll_ctl`/`epoll_wait` wrappers only forward the pointer to the kernel syscall — they never interpret the struct's fields themselves. All epoll usage in this project goes through `cix_epoll_event`/`cix_epoll_ctl`/`cix_epoll_wait`, never the system `struct epoll_event` directly.

## Consequences

- Any future kernel-uapi or glibc struct that relies on non-default packing must be checked under TCC before being trusted, and given the same `#pragma pack`-based treatment if needed — this is now a standing checklist item (recorded in `docs/roadmap/ROADMAP.md`'s locked-in decisions), not just a one-off fix for `epoll_event`.
- This class of bug is invisible to `-Wall -Werror` and to a correct-looking single test run; it only surfaced under repeated/varied runs. Anything touching raw syscall ABI structs under TCC should be stress-tested (many runs, not one) before being trusted as verified.
