# 0196 — zswap, with the daemon as the only thing that decides whether it is on

## Status

Accepted

Pairs with [ADR-0069](0069-host-swap-file.md) (the swap file itself) and [ADR-0195](0195-per-container-swap-limit.md) (the per-container swap limit). Same theme, three different questions: whether the host has swap, how much of it one container may use, and what happens to a page on its way there.

## Context

192.168.15.95 went fully unresponsive during a real `-j6` gcc bootstrap that drove it into swap thrashing. TCP still accepted connections; the daemon never answered. The disk it was thrashing had already produced two kernel Oopses in the page-cache/writeback path earlier the same session. (Issue #51 attributes that investigation to "ADR-0178"; that number belongs to an unrelated decision about package source URLs, and no ADR records the Oopses -- the evidence lives in the issue itself. Cited here as what it is rather than pointed at a document that says something else.)

zswap compresses pages in RAM before they would otherwise reach the swap device. It does not fix a bug in the disk path — it reduces how often that path is entered at all, which is a real mitigation for the failure that was actually observed rather than a general-purpose improvement.

`CONFIG_ZSWAP` was absent from this project's kernel, which is built from `allnoconfig` and enables only what is explicitly listed.

## Decision

**zswap is compiled in, and the daemon is the only thing that turns it on.**

`CONFIG_ZSWAP_DEFAULT_ON` is deliberately **not** set. Whether zswap is running is a real, operator-visible setting owned by `GET`/`PUT /v1/system/zswap`, persisted, and re-applied at every daemon start. A kernel that also decided it at boot would be a second place where one fact lives, and the two would eventually disagree without either being wrong about itself.

**Three compressors are compiled in, not one.** The compressor is a runtime module parameter, so an operator can change it without a rebuild — but only among those the kernel actually has, which makes the set compiled in the thing that decides whether that knob is real at all. LZO is the default: it is the kernel's own, and the fastest of the three, which matters on a box whose failure mode was a CPU-saturated parallel compile. LZ4 and ZSTD are there for a workload that would rather trade CPU for a better ratio.

**Configured intent and kernel state are reported separately.** The kernel silently ignores a parameter it cannot honour, so an endpoint that echoed back its own input would report success for a setting that never took. `kernel.*` is read from `/sys/module/zswap/parameters` on every GET, and disagreement with the configured values is the whole point of showing both. `available_compressors` comes from `/proc/crypto` — what this kernel was actually built with — which is why a compressor it lacks is refused before the write rather than dropped after it.

**On by default**, unlike the swap file. That file is opt-in because it consumes real disk; zswap consumes nothing until the box is already swapping, and at that point there is no reading of "off by default" that helps the operator whose box is thrashing.

**A failed apply persists nothing.** If the kernel refuses the settings, the previous ones are restored and the config file is left alone: a file describing a state the machine is not in is worse than the failure it was trying to record.

## Consequences

The order of application is load-bearing and not obvious: pool percentage and compressor are written **before** the enable flag. zswap allocates its pool when first enabled, and a compressor written afterwards applies only to pages compressed from then on — applying in the other order would leave the pool's first pages compressed with the previous setting.

**This cannot be verified end to end in this project's dev sandbox.** An LXC mounts `/sys` read-only, so writing a kernel module parameter fails regardless of privilege — confirmed directly, as real root: `echo Y > /sys/module/zswap/parameters/enabled` returns `Read-only file system`. The same class of restriction as this sandbox's other confirmed denials (`clock_settime` EPERM, no loop devices, no `/dev/kvm`). What the sandbox *does* prove is the reporting: it shows configured `enabled` against kernel `disabled`, which is exactly the divergence this design exists to surface. The apply path itself is verified on the real host, and the test asserts only what holds everywhere rather than pretending otherwise.

zswap's benefit is a reduction in disk I/O under pressure, which means it is measurable rather than assumable. Measured on the real host against a fixed, memcg-contained workload — a container capped at 128 MiB building a 400 MB anonymous buffer, so 400-700 MiB has to leave RAM — with two runs each way:

| | wall | `vda` io_time | sectors written |
|---|---|---|---|
| zswap off | 75.2s / 73.1s | 52,796 ms / 52,132 ms | 4,197 MiB / 4,197 MiB |
| zswap on | 24.3s / 24.3s | 88 ms / 24 ms | 0.3 MiB / 0.2 MiB |

Real disk writes went from 4.2 GiB to a quarter of a megabyte, and the workload finished three times faster. **The magnitude here is best case and should not be read as typical**: the buffer is a single repeated byte, which compresses about as well as anything can. What the measurement establishes is the mechanism and its direction under genuine pressure, not a ratio to expect from a compile.

The pressure was deliberately contained inside one cgroup rather than driven host-wide. Host-wide pressure on a box running nine real containers is the incident this exists to mitigate, not a test to run on purpose.
