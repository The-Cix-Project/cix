# 0001 — TCC exclusively, dynamically linked against system glibc

## Status

Accepted

## Context

The mission (`docs/MISSION.md`) mandates the Tiny C Compiler (TCC) for every component, with zero compile warnings. TCC is a far simpler, less-optimizing compiler than GCC/Clang, and its static linking is broken/undocumented — using it naively could mean either falling back to GCC for "hard" parts (a parallel toolchain, forbidden) or fighting static linking indefinitely.

## Decision

Every binary in this project is compiled with `tcc -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -I...` and **dynamically linked against the host's system glibc** — never `-static`, never TCC's own bundled headers. `-Wall -Werror` is a hard gate: a build with warnings is a failed step, independent of runtime behavior.

## Consequences

- We get real glibc semantics (syscall wrappers, `errno`, locale-independent behavior) for free, at the cost of the resulting binaries not being portable to a system without a compatible glibc — acceptable, since this is a from-scratch OS project building its own host anyway, not a portable utility.
- Some glibc/kernel-uapi structs and syscalls have no TCC-friendly path (see ADR-0002, ADR-0008) and need explicit workarounds, discovered incrementally rather than up front.
- Never reach for GCC/Clang "just for this one file" — that would be a parallel implementation of the toolchain itself.
