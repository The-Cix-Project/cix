# 0355 — A container's CPU directory lists only its own CPUs

## Status

Accepted, 2026-10-25, for #363. The owner asked for #363 to be done. The mechanism below was not put to them separately.

## Context

[ADR-0262](0262-a-container-sees-its-own-limits.md) makes a container read its own limits through procfuse, and by now that covers `/sys/devices/system/cpu/{online,present,possible}` as well as six `/proc` files. It did not cover the `cpuN` directories, because no file bind can remove a directory.

Measured on 192.168.15.95, 2026-10-25, in a `jumpbox` container with `cpuset_cpus: "0"` on a 2-CPU host:

- `online`, `present` and `possible` read `0`;
- `nproc`, `getconf _NPROCESSORS_CONF` and `getconf _NPROCESSORS_ONLN` all read 1;
- `/proc/stat` had a `cpu0` line only;
- `/sys/devices/system/cpu` still listed `cpu0` and `cpu1`, and `cpu1/online` read `1`;
- `htop` drew `1 [ offline ]` beside CPU 0.

So every number was right, and the one tool an operator opens to look at a container's CPUs showed a failed CPU. The owner's own `jump` container has `cpuset=0` and shows exactly that.

`getconf _NPROCESSORS_CONF` reading 1 also disproved a claim in `procfuse.h` that it counts the directories. On this glibc it reads `possible`, which procfuse already served. htop reads the directory itself.

## Decision

1. **`mountns_pivot()` replaces `/sys/devices/system/cpu` with a view** when the container gets its procfuse files. The view is a tmpfs over the directory, holding one placeholder per entry of the real directory with that entry bound onto it. The tmpfs is then made read-only.
2. **The CPUs kept are the child's affinity**, read with `sched_getaffinity()`. The child is cloned into its cgroup (`CLONE_INTO_CGROUP`), so its affinity is that cgroup's effective cpuset. That is the set procfuse renders `online` from and the set `nproc` reports, so the three views cannot disagree. The runtime has no second range-list parser.
3. **Nothing is synthesised.** A kept entry is the kernel's own, bound from the container's fresh sysfs through a directory fd opened before the tmpfs covers it.
4. **A container whose affinity covers every listed CPU gets the real directory**, with nothing mounted. That is every container without a cpuset, on a host whose CPUs are all online.
5. **All or nothing.** Any failure after the tmpfs is mounted detaches it, and every bind inside it, so the container sees the real directory. An entry that is neither a directory nor a regular file aborts the view rather than being guessed at; the real directory holds only those two (measured).
6. **The same switch as procfuse.** A container with `procfuse: false` keeps the host's directory along with the host's files.

## Alternatives

- **Serve the whole directory from procfuse,** filtering `readdir` and passing every other read through to sysfs. That makes the FUSE server a proxy for a kernel tree whose shape changes between kernels, and puts a userspace round trip on reads that need none.
- **Bind an empty directory over each hidden `cpuN`.** It is still listed in its parent, and htop counts it.
- **Renumber the kept CPUs from 0.** That would contradict `/proc/stat`, `/proc/cpuinfo` and `sched_getaffinity()`, which all name real CPU numbers.

## Consequences

- `htop` in a container pinned to CPU 0 draws one meter.
- A container pinned to CPUs that do not start at 0 lists only those `cpuN` directories, and `htop` still draws the missing low ones as offline. It sizes itself from the highest CPU number: pinned to CPU 1 on .95 (0.2.57-600, 2026-10-25), it drew two meters, CPU 0 offline. Only renumbering could change that, and the alternatives above reject it.
- A container with a cpuset holds about twenty more mounts in its own mount namespace.
- **Nesting:** in a user-namespace container nested inside one that has the view, the inherited tmpfs is a locked mount over a non-empty directory. The kernel's fully-visible check for a fresh sysfs mount looks at exactly that. The procfuse file binds were already locked mounts in the same tree, so this adds a case of the same kind. It applies only when the outer container has a cpuset; build containers have none.
