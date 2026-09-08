# 0262 — A container sees its own limits, not the host's

## Status

Proposed

Issue [#336](https://git.home.arpa/itdlabs/cix/issues/336). Kernel support (`CONFIG_FUSE_FS`) ships in
`kernel@7.2.3-4`; nothing else here is built yet.

## Context

A process inside a Cix container reads `/proc/meminfo` and sees the host's memory. It reads
`/proc/cpuinfo` and counts the host's processors. Its cgroup says otherwise, and nothing reconciles
the two.

**This is not cosmetic, and the platform has already paid for it.** Issue #278/#279 was a build
sizing itself by host memory against a 2 GiB cgroup ceiling it had no way to see: the dashboard
showed 5.88 GB available while `/cix-workload/cix-pkgbuild` sat pinned at its limit, OOMing
continuously. Both numbers were true. The workload could only read the wrong one.

The general case is every tool that sizes itself from `/proc`:

| reads | decides |
|---|---|
| `/proc/meminfo` | a JVM's default heap, a database's cache, whether an allocator gives up |
| `nproc`, `/proc/cpuinfo` | `make -j$(nproc)`, a thread pool's width, GOMAXPROCS |
| `/proc/stat`, `/proc/uptime`, `/proc/loadavg` | anything reporting utilisation |

`nproc` is a partial exception worth naming precisely, because it is the one people assume is
already fine: it reads `sched_getaffinity`, which a `cpuset` genuinely constrains — so a container
with `cpuset_cpus` set already counts correctly, while one limited only by `cpu.max` (a quota, the
common case here) does not.

The owner raised this directly, and the second half of the request is the harder half: it should be
the **default**, not an opt-in a recipe remembers.

## Decision

**One host-side FUSE server, owned by the control plane, whose files the runtime bind-mounts into
every container.** This is the shape lxcfs chose, for reasons that apply here unchanged.

### The server identifies the asking container from the request, not from itself

The first sketch of this read `/sys/fs/cgroup/memory.max` — the *reader's own* cgroup — which is
correct only if the server runs inside the container being measured. That would mean one server per
container: twelve copies on this box today, each a process, each a mount, each a thing to supervise
and to fail independently.

A FUSE request carries the requester's pid in its header (`in_header.pid`). The server resolves that
pid to its cgroup through `/proc/<pid>/cgroup` and reads the limits from that cgroup's own directory
under `/sys/fs/cgroup`. One server answers every container correctly, and a container that is not
under a cgroup at all gets the host's real numbers, which is the right answer for it.

### It is not part of `cix-init`

`cix-init` is freestanding, holds no policy, and — decisively — runs *after* the container's
capabilities are dropped, so it cannot mount anything. A FUSE server inside PID 1 would also make a
stalled `/dev/fuse` read a stalled init, which is precisely the failure ADR-0247 exists to refuse.
The two are different jobs and stay different processes.

### It is a forked child of `cixd`, not a package and not a second binary

`stallwatch` is the precedent: `cixd` forks a child that runs its own loop
(`daemon/src/stallwatch.c`'s `watchdog_main()`), in the same binary, supervised by the parent it
already has. The FUSE server takes that shape. Its loop blocks on `/dev/fuse`, which is exactly why
it must not be in the reactor — and being a separate process is what makes blocking there correct
rather than a violation.

### It speaks the `/dev/fuse` protocol directly, in our own C

The alternative is packaging libfuse and calling `fuse_main()`. Against that: a read-only filesystem
serving seven synthetic files needs `INIT`, `LOOKUP`, `GETATTR`, `OPEN`, `READ`, `RELEASE`,
`OPENDIR`, `READDIR`, `FORGET` and `STATFS` — a few hundred lines with no ABI surface, no
`FUSE_USE_VERSION` to track, and no third-party dependency for one consumer. This is Cix's own code,
so it is TCC by the Toolchain Tenet either way; writing it against a library would add a package to
the platform without removing any of the work that matters.

### What it serves, and from what

Synthesised per requester, from that requester's cgroup:

- **`/proc/meminfo`** — `MemTotal` from `memory.max` (the host's real total when `max`), and
  `MemFree`/`MemAvailable`/`Cached`/`Buffers` derived from `memory.current` and `memory.stat`'s
  `file`, `anon` and `inactive_file`. **Never fractions of the limit.** A process deciding whether it
  can allocate reads those fields, and a fixed half is a lie in both directions.
- **`/proc/cpuinfo`** — the host's own per-processor stanzas for the CPUs in
  `cpuset.cpus.effective`, renumbered `0..n-1`, truncated to `ceil(quota/period)` from `cpu.max`
  when a quota narrows it further. Not a synthetic vendor and model: something reads those.
- **`/proc/stat`**, **`/proc/uptime`**, **`/proc/loadavg`**, **`/proc/swaps`** — from `cpu.stat`,
  the container's own start time, and `memory.swap.*`.

### Mounted by default, refusable per container

The runtime bind-mounts each file over the real one at container creation, in the mount namespace it
already builds (`src/mountns.c` mounts `/proc` today). Default on, because a default that has to be
remembered is the bug: nobody writing a recipe knows they need it until something has already sized
itself wrong. A container may decline with an explicit field, and the build container is the case to
watch — its own `nproc` decides `make -j`.

## Consequences

**A new failure mode: a filesystem in the boot path with a userspace server behind it.** If the
server dies, reads of those paths block or fail inside every container at once. The mitigations are
the ones the shape already gives — the parent supervises it exactly as it supervises `stallwatch`,
and a container whose mount is gone falls back to the host's real `/proc` files, which is degraded
rather than broken. This is the real cost of the decision and it is not hypothetical.

**`/proc/meminfo` becomes a number the platform computes**, so it can be wrong in a new way. It is
covered by a test that sets a real `memory_max` on a real container and reads the file back from
inside it, which is the only check that means anything here.

**It does not virtualise `/sys`.** lxcfs also serves `/sys/devices/system/cpu`; nothing measured on
this platform needs it yet, and it can follow if something does.

**Not a security boundary.** A process can still read the host's real figures through other paths.
This makes tools size themselves correctly; it does not hide the host.

## Alternatives considered

**Per-container server (the first sketch).** Simplest to reason about — the server reads its own
cgroup — and rejected on multiplicity: N processes, N mounts, N supervision problems, for a job one
process does. It also cannot mount itself into the container without holding capabilities the
container has deliberately dropped.

**Patch the kernel to virtualise `/proc` per cgroup.** The honest way to do it, and repeatedly
rejected upstream over many years. Out of scope for a platform that builds mainline unmodified.

**Nothing; document it.** What the platform does today. #278/#279 is the argument against: the
failure surfaced as an unexplained OOM against a machine reporting plenty free, and the person
looking at it could not see the number that mattered.
