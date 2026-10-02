# 0322 — A recipe declares the memory its build needs, and the operator's ceiling bounds it

## Status

Accepted by the owner for [#558](https://git.home.arpa/itdlabs/cix/issues/558): *"yes a recepie can declare how much it needs, but it cannot exceed a preallocated max"*. The language half is CPDL `resources { memory }`, filed as cix-build-system#276 and shipped in cbs v0.1.100 (commit `fd7a389`, 2026-10-02).

## Context

Every build runs under one parent cgroup, `cix-workload/cix-pkgbuild`, whose `memory.max` is the operator's `memory_max` (#85: a per-container limit multiplied by concurrency is not a budget). The default is 2 GiB.

Some packages need more than that, and nothing could say so. `node@24.21.0` thrashed at 2 GiB on 192.168.15.95 (2026-10-01): V8's `maglev-graph-builder.o` sat iowait-bound at about 1.5% CPU until the build was cancelled. At 4 GiB the same build completed, with the cgroup peaking at 3.73 GB. A build short of memory stalls rather than fails, so its first visible sign is a timeout that reads as a slow build. The requirement lived only in recipe prose. The only remedy was an operator raising `memory_max` by hand for one build and remembering to put it back.

## Decision

1. **The recipe declares its need.** CPDL `resources { memory "4GiB" }` is the aggregate memory of the whole build, all phases and processes together. It is not a per-process limit. cbs validates it and reports it in bytes as `resources.memory` in `cbs explain --json`. cixd reads that typed field (`cbs_explain_resources_memory()`) and nothing else. An opaque `metadata` key was available and was not used: a key only cixd understands, with a promise to replace it later, is a workaround.

2. **The operator pre-allocates the most any recipe may ask for.** That is `memory_max_ceiling` in `/v1/system/pkg-build-config`. It is never below `memory_max`, 0 means no ceiling, and an unlimited `memory_max` requires an unlimited ceiling. The two are validated as one pair, before anything in the request is applied, because the rule is a property of the pair. Raising `memory_max` past the ceiling therefore needs both in one request. A config saved before the ceiling existed loads with it equal to `memory_max`, so no recipe can raise anything until an operator says it may.

3. **A need above the ceiling is refused before the build starts.** The refusal comes before the build environment is composed. It names the need, the ceiling and the `cixctl` command that changes the ceiling. A resumed build gets the same check. A cache hit builds nothing, so it is never refused.

4. **A need within the ceiling raises the shared budget while that build runs.** The parent's `memory.max` becomes the larger of `memory_max` and the largest need any build in flight declared. `memory_max_effective` reports it, and the daemon logs the raise and the return.

5. **The raise is held until no build is running**, not dropped when the build that asked for it ends. Lowering `memory.max` under builds still running makes the kernel reclaim from them, and OOM-kill one if it cannot. A build that started during the raise may legitimately be using what it allowed. Holding costs a running build nothing; it only delays the return to `memory_max`.

## Alternatives rejected

- **Per-build cgroup limits sized by the declaration.** This is #85's mistake again: the per-build numbers add up past the machine.
- **Let a recipe raise without a ceiling.** Any recipe in the catalogue could then claim the whole box. The owner's direction was explicitly a pre-allocated maximum.
- **Lower the raise when its build ends.** See decision 5.
- **Apply a changed budget the moment it is PUT.** That has the same reclaim hazard as above. `memory_max` has always reached the parent at the next build start, and the ceiling only decides admission, which already reads the current value.

## Consequences

- `node` declares `resources { memory "4GiB" }`. On a host whose ceiling is below that it is refused at once, with the fix in the message, instead of stalling for an hour.
- The budget can exceed `memory_max` only while a declaring build runs, and never by more than what was admitted under the ceiling at its start.
- `test_pkg` (a floor test, so it gates releases) asserts the pair rule, a refusal naming both numbers, and a raise and its return in the daemon log.
