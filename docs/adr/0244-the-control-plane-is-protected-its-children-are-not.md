# 0244 — the control plane is protected, the workloads it spawns are not

## Status

Accepted

## Context

`cixd` sets `oom_score_adj -1000` on itself at startup
(`protect_control_plane()`, `daemon/src/main.c`), and that is right. On this
platform the REST daemon is not one management path among several — it is the
only one. There is no SSH and no general shell (ADR-0034), so a box whose
`cixd` has been OOM-killed is a box nobody can reach until someone walks to
the hypervisor. Issue #86 and ADR-0165 already treat starving the control
plane as the failure that matters most.

What was missed is that `oom_score_adj` is **inherited across `fork()` and
preserved across `execve()`**, and `cixd` runs as pid 1. So the exemption was
not held by one process; it was held by every process on the host. Containers,
package builds, exec sessions, a bare `sh` — all of them immune.

Verified rather than assumed, with a standalone program that sets `-1000`,
forks, and execs `/bin/sh`:

```
parent(set -1000): oom_score_adj=-1000
  exec'd sh:       oom_score_adj=-1000
```

The consequence is worse than a leaked privilege, because **a cgroup with no
eligible victim does not fail at its ceiling — it livelocks.** The kernel
scans for a victim, finds every task exempt, gives up without freeing
anything, the allocation retries, and it OOMs again immediately. Measured on
192.168.15.95 during a `cix-tests` build:

```
Memory cgroup stats for /cix-workload/cix-pkgbuild:
memory: usage 2097152kB, limit 2097152kB, failcnt 1180732
cp invoked oom-killer: ... oom_score_adj=-1000
Out of memory and no killable processes...
```

`failcnt` was 1,180,732 at first reading and 1,194,664 sixty seconds later —
roughly 14,000 failed allocations a minute, indefinitely, at about 1000 kernel
log lines per second. Every other entry was evicted from the log store. The
build neither completed nor failed; it sat in `state: building` forever. The
host had 5.88 GB of 8.27 GB free throughout, so nothing about the machine
looked wrong from `GET /v1/system/stats` — the ceiling that was full belonged
to a cgroup nothing reports (#279). It needed a manual reset, and it happened
more than once.

The general form is worse than the one incident: **every memory limit this
platform offers was unenforceable.** `POST /v1/containers` with `memory_max`
did not give an operator a bound, it gave them a livelock trigger.

## Decision

**The control plane keeps its exemption. Everything running workload code
loses it.**

`cix_oom_unprotect_self()` (`include/iohelpers.h`, already on both the
daemon's and the runtime library's build lines — one definition, not one per
call site) writes `0` back to `/proc/self/oom_score_adj`. It is called from
inside the forked child, before it runs anything:

- `src/container.c`, in the child after `ns_clone3()` — every container, and
  therefore every package build, since a build *is* a container.
- `daemon/src/exec.c`, in both grandchildren: the pty console and the piped
  diagnostic variant.

**Placed after the ADR-0179 uid/gid map sync, not before it.** A
user-namespaced container has no mapped identity until those maps are written
— it is the overflow uid, and its own `/proc/self` files are owned by an
identity it does not hold, so the write fails `EACCES`. Silently, because a
forked child a moment before `execve` has nowhere to report to. Placing it
earlier would have left every userns container carrying exactly the exemption
this removes, while the code read as though it did not. Raising
`oom_score_adj` is unprivileged (only lowering it needs `CAP_SYS_RESOURCE`),
so once the maps exist there is nothing further to wait for.

**The line is drawn at workload code, not at "anything cixd spawns."** The
daemon's own short-lived helpers — `curl` fetching a source tarball,
`openssl`, `tar`, `mksquashfs` — stay protected with it, deliberately. They
are the control plane doing its own work, and under real global pressure the
kernel should reclaim from a workload rather than from the only way into the
machine. That choice is only safe *because* workloads are now killable: before
this, "kill a workload instead" was not an option the kernel had.

**Failure is silent by design.** The helper runs in a forked child immediately
before `execve` with no channel to report on, and the process must still exec.
An unwritable `oom_score_adj` leaves the inherited value — exactly the
behaviour that existed before this, and no worse.

## Consequences

Verified end to end on 192.168.15.95 running v2.53.26. A container with a
64 MB `memory_max` running an unbounded allocator:

```
status: exited, term_signal: 9
oom-kill:constraint=CONSTRAINT_MEMCG, oom_memcg=/cix-workload/oomprobe
Memory cgroup out of memory: Killed process 205 (perl) anon-rss:65068kB
```

Killed in under five seconds, with the host healthy throughout. The same
container before this change is the livelock described above. Both an exec'd
console process and a container's own pid 1 read `oom_score_adj` of `0`, taken
from inside the container rather than inferred.

Memory limits are now bounds rather than traps, which makes
`ContainerCreateRequest.memory_max` and the pkgbuild cgroup's own ceiling mean
what they say for the first time.

Tested behaviourally rather than by inspection, because inheritance is the
whole point and only a process that has been through both `fork()` and
`execve()` can take a trustworthy reading: `console_term_child` reports its own
`oom_score_adj` and `test_console_exec` asserts `OOMADJ=0` through a real
console session.

The reporting gap that made this so hard to see is separate and still open:
host memory looked fine because the ceiling that was full belonged to a cgroup
nothing surfaces (#279). Fixing the killer does not make the pressure visible.
