# 0301 — A container's disk usage is named for the question it answers, not for the mechanism that used to answer it

## Status

Accepted. Issue [#475](https://git.home.arpa/itdlabs/cix/issues/475). **Supersedes the `disk.upper_*` field naming in [ADR-0054](0054-host-side-container-stats.md)**; the measurement decisions in ADR-0054, [ADR-0267](0267-a-volumes-size-comes-from-the-kernel-when-the-kernel-is-counting.md) and [ADR-0293](0293-an-unbounded-measurement-is-served-from-a-cache.md) all stand unchanged.

## Context

`GET /containers/{name}/stats` reported three flat fields: `disk.upper_bytes`, `disk.upper_measured_at`, `disk.upper_source`.

`upper_` was accurate when ADR-0054 chose it. A container's writable layer really was an overlayfs **upperdir**, with its image's rootfs as the lowerdir, and "how big is the upper" was both the question and the mechanism. [ADR-0207](0207-btrfs-storage-substrate-userns-by-default.md) then moved the substrate to btrfs, where a container's writable tree is a **subvolume seeded from its image** — no upper layer, no lower layer, no overlay at all. The prefix outlived the thing it named.

That is not merely untidy, and #475 is the proof: the field went on *measuring* a tree as though it were a diff, reported image-sized figures for years, and its own documentation asserted it excluded "the shared, read-only image layer beneath it" — a layer that no longer existed. The name kept a mental model alive that the storage layer had abandoned, and the wrong number survived underneath it. #475's measurement fix (prefer the subvolume's qgroup, report which measurement answered) landed first; this is the naming half the same issue asked for.

## Decision

**`disk.usage`, an object: `{bytes, source, measured_at}`.** The name states what the value is — how much this container's writable tree holds — and says nothing about how it was obtained, which is now two different mechanisms and may later be more. `source` is where the mechanism belongs, because it varies per response.

**Nested, not flattened.** `bytes`/`source`/`measured_at` as siblings of the existing `read_bytes`, `write_bytes`, `read_ios` and `write_ios` would put a bare `bytes` next to two other `*_bytes` fields, where it reads as their total. It is not: those are cumulative I/O counters since the container started, and this is a gauge. Nesting keeps a gauge and a set of counters visibly separate.

**The same name and shape as `GET /volumes/{name}/usage`.** That endpoint answers the same question about a different resource, already returns `{bytes, source, measured_at}`, and already shares the qgroup-or-walk decision (ADR-0267) and the helper implementing it. One question, one answer shape, on both resources.

**A clean cut-over: no alias, no deprecation period.** The old names are gone in the same commit as the new ones, in the daemon, the CLI, the dashboard, the tests and the contract. This project does not ship compatibility shims, and a field that exists under two names is two sources of truth for one number.

## Alternatives rejected

**Leave the name.** Offered to the owner when #475's measurement fix was closed, with the argument that the qgroup figure makes `upper_bytes` *more* accurate than it was — exclusive extents are roughly what an upper layer would have held. Rejected: "roughly what the old mechanism would have held" is a coincidence, not a meaning, and the next reader still has to know a retired storage design to parse the field. The owner asked for the rename.

**Flat `bytes`/`source`/`measured_at`.** Matches the volume endpoint most literally, and is wrong here for a reason that does not apply there: a volume's response has no `read_bytes`/`write_bytes` to be confused with.

**`exclusive_bytes`, after the qgroup's own term.** Precise on the qgroup path and false on the walk path, which reports the whole tree. Naming a field after one of its two sources is how the current problem was created.

**Rename in place, keeping the fields flat (`disk.usage_bytes`, `disk.usage_source`, …).** Fixes the vocabulary and keeps the shape divergent from the volume endpoint for no gain; the prefix repetition is also what nesting exists for.

## Consequences

`disk` gains one level of nesting, so every consumer of the three old fields changed: `daemon/src/main.c`'s writer, `cixctl container stats`, the dashboard's disk chart and label, `test_container_stats.c`, and the contract in `openapi.yaml` and `docs/api/README.md` — updated together, as the documentation map requires.

**The CLI guards a missing `usage`; the dashboard does not, deliberately.** A locally built `cixctl` is routinely one deploy ahead of the daemon it queries (#476), so it prints "not reported -- this daemon predates the field" rather than a zero, the same distinction #481 had to draw on assembly status. The dashboard is *served by* the daemon it queries, so the two are always the same build and a guard there would be dead code asserting an impossible state.

**Three earlier ADRs name the old fields in their bodies and keep doing so.** ADR-0054, ADR-0130 and ADR-0293 are the record of what was decided when `upper_bytes` was the right name; each gains a one-line pointer in its Status to this ADR. Rewriting their bodies would make them lie about their own moment, which is what append-only means.

**No automated gate protects the rename.** `test_container_stats` is not in `SELFTESTS` (#224, #480), and neither `test_apigen` nor `test_api_surfaces` cross-checks a schema's `required:` fields against the daemon's JSON writer — read rather than assumed. So a leftover `upper_*` would not fail a build: the check is that `grep -rn 'upper_bytes\|upper_measured_at\|upper_source'` finds only deliberate historical references, plus the live read after deploy.
