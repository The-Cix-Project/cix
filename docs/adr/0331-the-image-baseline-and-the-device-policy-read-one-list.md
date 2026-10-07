# 0331 — The image baseline and the device policy read one list

## Status

Accepted, 2026-10-07, closing [cix#578](https://git.home.arpa/itdlabs/cix/issues/578). It amends [ADR-0017](0017-ebpf-cgroup-device-filter-for-hardware-passthrough.md), which decided that a container's device access is governed by a `BPF_CGROUP_DEVICE` program built from what the container declares. That decision stands; what it never said is what happens to the nodes nobody declares because every image already has them.

This is a technical decision taken while fixing #578, not a product direction from the owner. It is recorded because it changes the policy a container actually runs under, which is hard to reverse quietly.

## Context

Two places decide what `/dev` holds in a Cix container, and only one of them knew about the other.

- **`pkg_seed_image_baseline()`** (`daemon/src/pkg.c`) stages a fixed set of nodes into *every* image: `null` 1:3, `zero` 1:5, `full` 1:7, `random` 1:8, `urandom` 1:9 and, since [#577](https://git.home.arpa/itdlabs/cix/issues/577), `tty` 5:0 — six char devices, plus a `ptmx` symlink to `pts/ptmx`.
- **`container_dev_bpf_attach()`** (`src/container_dev.c`) builds its program from `spec->devices`, which carries only what the create request named: one comparison block per declared device jumping to allow, then a deny epilogue for everything that falls through.

Nothing added the baseline to that list. **So a container that declared one device got a policy that denied `/dev/null`.**

Three measurements make that concrete, all on 192.168.15.95, 2026-10-07:

1. **The denied node is a real one the baseline staged.** `src/mountns.c` mounts exactly four filesystems inside a container's mount namespace — `proc` at `/proc`, `sysfs` at `/sys`, `cgroup2` at `/sys/fs/cgroup`, and `devpts` at `/dev/pts`. **`/dev` itself is never mounted**; `mountns.c:309` only `mkdir`s it when absent. `/dev` is therefore the image rootfs's own directory, made private per container by the overlay upperdir rather than by a separate mount, and `BPF_CGROUP_DEVICE` governs the open of the node sitting in it. This was the load-bearing fact #578 asserted from code-reading without establishing: had `/dev` been a fresh devtmpfs or a bind mount, the issue would have needed rewriting rather than fixing.
2. **It has never governed a running container, which is why it went unnoticed.** All twelve containers on the host declare an empty `devices` list, and `container_dev_bpf_attach()` returns with `*out_prog_fd = -1` and attaches nothing when `device_count <= 0`. So an ordinary container is unrestricted and the bug is latent rather than rare — the first operator to use passthrough in earnest is the one who finds it.
3. **The feature's own test could not have caught it.** `test_devices` declares devices deliberately and grants real, already-permitted numbers, because this dev sandbox runs under an ancestor cgroup's own device policy that permits only a standard set (CLAUDE.md records the measurement). It therefore exercises the allow path for what it declared and had no reason to open `/dev/null`.

**A third table exists and is deliberately NOT a copy.** `pkg_seed_image_baseline()`'s comment pointed at a `dev_nodes[]` in `test_image_fixture_stage_toolchain()` — in `test/test_image_fixture_host.c`, not `test/test_image_fixture.c` as that comment said. It stages **five** nodes, not six, and its own comment gives the reason: *"/dev/tty is deliberately omitted -- nothing in a batch `./configure && make && make install` sequence needs a controlling terminal."* It stages a *build* image's `/dev`, which exists so configure scripts can redirect to `/dev/null`; it is not describing what a Cix image is. Reading the five-vs-six gap as drift and converging it would add a node that fixture deliberately excludes, and would put a new `mknod` of 5:0 inside the build container the floor tests run in, where any failure but `EEXIST` fails the fixture outright. So this decision has **two** consumers, and that third table stays as it is with a comment saying why.

## Decision

**The baseline device nodes are declared once, and every consumer reads that declaration.**

1. **The list lives in `include/container.h`, as an X-macro — `CIX_BASELINE_DEVICES(X)`, taking `X(name, major, minor)`.** That is the convention [`include/controlplane_programs.h`](../../include/controlplane_programs.h) already set for a table with several consumers that each want a different shape, and this one's two want different shapes: the seeder wants `{name, major, minor}` to `mknod`, the BPF emitter wants `{major, minor}` to compare. Each expands the list into its own local array and derives its own count with `sizeof`, exactly as `mkbootroot.c` and `test_controlplane_programs.c` already do with the programs list.

   A shared `static const` array in the header was written first and rejected: a header-scope array emits a copy in every translation unit that includes `container.h`, which is most of the daemon, and risks `-Wunused` under the project's mandatory `-Werror` — unverifiable here, since this sandbox compiles nothing but `cixctl`. The X-macro emits nothing until a translation unit expands it, so including the header costs no storage and cannot warn. It also made the type question moot: `struct device_spec` would have carried a `dev_path[64]` and a `type` that every baseline entry sets identically, which is three fields of ceremony for two that matter.

   `container.h` is the only layer that works. `src/container_dev.c` is the runtime library and includes no daemon headers; the list cannot live in `daemon/src/pkg.c` and be read downward without inverting the dependency the API-First mandate establishes. A new header would be a third file that knows about device nodes.

   `container.h` is also the only layer that works. `src/container_dev.c` is the runtime library and includes no daemon headers; the list cannot live in `daemon/src/pkg.c` and be read downward without inverting the dependency the API-First mandate establishes. A new header would be a fourth file that knows about device nodes.

2. **`container_dev_bpf_attach()` emits allow blocks for the baseline before the declared ones**, in one loop over a merged view rather than two loops with the same body. The alternative — appending the baseline to `spec->devices` in `container_create()` before attaching — was considered and rejected: it is simpler, but it makes the baseline nodes appear in what a container reports as its devices, which is a contract change to `GET /v1/containers/{name}` for the sake of an implementation detail.

3. **A container that declares no devices is still governed by nothing.** The `device_count <= 0` early return is unchanged. This fix must not begin restricting the containers that have never been restricted, and the zero-device case is gated by its own assertion rather than left to inspection.

4. **The `ptmx` symlink is not a device node and is not in the list.** It is a symlink to `pts/ptmx`, whose target lives on the devpts mount the policy treats separately. It stays in `pkg_seed_image_baseline()` as the one piece of baseline staging that is not a shared node — stated here so the next person adding to the table does not reach for it.

5. **`container_dev_mknod()` is untouched.** It creates the *declared* nodes after pivot; the baseline nodes already exist in the image, and changing it is unrelated to this.

## Consequences

- The first real passthrough container — a GPU, a radio, a serial port, which is the whole point of ADR-0017 — gets a policy that permits the nodes its userspace cannot run without. Before this, it got an `EPERM` on `/dev/null` naming a device nobody had asked about.
- Adding a node to the baseline is now one edit instead of two, and a node that is staged but not permitted is no longer expressible.
- The program is **six** comparison blocks longer for any container that declares a device, one per baseline node, so a declaring container's program goes from `4 + 4n + 4` instructions to `4 + 4(n + 6) + 4`. `container_dev_bpf_attach()`'s `prog[]` dimension and its `allow_idx` arithmetic are both computed from the merged count, and `load_attr.insn_cnt` follows the emit cursor, so the jump offsets stay correct by construction rather than by a second hand-maintained constant.
- `test_devices` gates both arms: a container declaring one device can open `/dev/null`, and a container declaring none still attaches no program. The first fails before this change, which is the reintroduce-to-prove-it this project requires of a regression test.
- The baseline and the policy can still disagree about a node that the *kernel* refuses — this decision makes them agree about what Cix intends, not about what any particular host permits. An ancestor cgroup denying a node (as this dev sandbox's does) is unaffected and is correctly not something Cix tries to override.
