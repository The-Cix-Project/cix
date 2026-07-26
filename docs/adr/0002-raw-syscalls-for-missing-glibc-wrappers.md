# 0002 — Raw syscalls where glibc has no wrapper

## Status

Accepted

## Context

`clone3(2)`, `pivot_root(2)`, and `pidfd_send_signal(2)` have no glibc wrapper function on this system's glibc version. `clone3`'s argument, `struct clone_args`, is a stable kernel uapi struct, but the obvious way to get its definition — `#include <linux/sched.h>` — clashes with glibc's own `<sched.h>` over the `CLONE_*` macros and `struct sched_param`.

## Decision

For any syscall with no glibc wrapper, declare the syscall number (with a fallback `#define` if the running glibc's `<sys/syscall.h>` doesn't already have it) and call it via `syscall(SYS_xxx, ...)`, wrapped in a small `static inline` helper. Kernel-uapi structs needed for these calls (currently just `struct clone_args`) are declared by hand in `include/linux_compat.h`, not pulled in from `<linux/*.h>` headers that conflict with glibc's own. All of this lives in one file, `include/linux_compat.h`, rather than scattered inline `syscall()` calls at each use site.

## Consequences

- One place to look for "how do we call this kernel feature," and one place to update if a syscall number or struct layout ever needs adjusting for a different kernel/glibc combination.
- We take on the burden of keeping our hand-declared structs in sync with the real kernel ABI ourselves — see ADR-0008 for a case where this assumption (that a plain `#include` would have gotten the layout right anyway) would *also* have failed, for a different reason.
- Every future syscall-without-a-wrapper need (there will be more, e.g. in the networking phases) follows this same pattern rather than inventing a new one each time.
