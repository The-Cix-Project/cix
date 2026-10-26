# 0356 — An install's image version is produced in a helper child

## Status

Accepted, 2026-10-25, for #617, with two parts superseded by [ADR-0357](0357-an-uninstall-is-a-package-job-produced-in-a-helper-child.md): decision 1's package snapshot, now written with the tree in the child, and decision 4's 409 for a delete, which now waits. The owner priced it at three to four release cycles in the issue and said "go ahead with #617". The mechanism below was not put to them separately.

## Context

An install ends by producing a new version of its image ([ADR-0107](0107-package-image-versioning.md)): a snapshot of the current version, the package's files merged into it (on an upgrade the old version's files are unlinked first), a hash of the installed set to name it, and a rename into place. All of it ran in `pkg_build_completed()`, on the event loop.

Measured on 192.168.15.95 (`GET /v1/system/stalls`, #617):

- 2026-10-23 20:00:09Z, `hibr@0.99.306-1` upgrading in `jumpbox`: the container exit held the loop 1103 ms. The slowest step was the mutate step, 862 ms, of which 834 ms was 203 `unlink()` calls.
- 2026-10-20 00:59:48Z, the same step: 1219 ms over 386 calls, while `jumpbox` held 256+ versions. [ADR-0343](0343-an-image-keeps-a-bounded-history-and-a-version-leaves-it-off-the-loop.md) bounded the history after that, and the cost of a call fell, but did not reach zero: the 2026-10-23 calls cost about 4 ms each.

The cost grows with the package's file count, so a package with thousands of files would hold the loop proportionally longer. Trimming the unlinks does not help: `merge_tree()` unlinks every destination again before it copies, so skipped removals would move there.

A build environment's composer already produced its version in a forked child (#238), with the fd sweep and the held log entries (#606).

## Decision

1. **Producing a version is two halves.** `image_version_build()` builds and names the tree: snapshot, mutate, hash, placement. `image_version_commit()` records it as the current version and writes its package snapshot (#398). The record is what readers are told about ([ADR-0339](0339-the-dashboard-is-told-what-changed.md)) and what the retention pass keys on, so only the daemon runs it. `image_produce_new_version()` keeps both halves together for the callers that still produce in the daemon: a delete, the toolchain bootstrap, and the build sandbox's one-time migration at start.
2. **One producer child, two callers.** `image_version_fork()` runs the build half in a child and reports through a file (`produce_report`): the reason for a failure, the version's name, how long it took, the held log entries, and, for an install, the files the merge recorded. A composer and an install's producer both use it. The composer no longer records its version in the child.
3. **An install's completion resumes on the child's exit.** `pkg_build_completed()` starts the producer and returns 2. The daemon watches the child's pidfd (`CONN_PKG_PRODUCE`), and `pkg_produce_completed()` records the version, applies the file list to the entry and finishes the install: the same checks, cache, run close, state save and chain advance as before, now in `install_after_produce()` and `install_finish()`. `pkg_completion_followup()` is the one follow-up for every completion, and knows a producer from a fetch.
4. **One image has one producer at a time.** Two would snapshot the same version, and whichever was recorded second would drop the other's files. An install reaching production while another chain is producing the same image starts a waiter on that producer, as #618 does for a composer, and starts its own when the waiter exits. A delete in that image and a toolchain bootstrap of the build sandbox answer 409 (`PKG_ERR_IMAGE_PRODUCING`) and change nothing.
5. **The #389 and #553 gates run where production begins**, after any wait. Run as the build completed, a second install into an image would be checked against owners whose files had not been recorded yet.
6. **An ordinary install becomes INSTALLED when its version is recorded.** Until then the entry stays in its build's state, in the install stage (#424's `current_stage`), so anything that waits for "installed" can use the image straight away. The child marks its own copy INSTALLED, because the version is named from the set with the package in it. A hostbuild is marked as before. While the producer runs, `chain_reap_stale()` leaves the chain alone and a cancel answers that no build is in flight.
7. **A failed production keeps an upgrade installed.** The merge runs in the child, so the daemon's entry still describes the version that is in the image, and the #570 rule that applies to the gates applies here too. This replaces "not kept installed", which held only because the merge used to rewrite the entry's file list in the daemon.
8. **The staging tree has one name per image**, `.staging`. It carried the daemon's pid, which a child does not share, so a leftover would never have been found again. The start-up sweep removes any `.staging*` it finds, the old spelling included.
9. **`test_blocking_waits` counts the producer's call sites**, as it counts the other moves off the loop (#612, #630, #631, #633).

## Alternatives

- **Trim the unlinks** to the paths the new version does not ship. `merge_tree()` unlinks every destination before copying, so the removals move rather than go.
- **Record the version in the child**, as the composer did. The retention mark is in-memory, so ADR-0343's pass would never run for that image, and the image's state file would change with no announcement.
- **Keep the entry INSTALLED from the build's end**, as the synchronous code did. The window would now be as long as the child takes, and `cixctl pkg install --wait`, a test, or a converge reading "installed" would use the old image.
- **Make a delete wait** for the producer, parked as a request (ADR-0340's park). That moves the delete off the loop too and is the right end state, but `converge_uninstall_strays()` calls it in a loop inside the rolling drain, which would need the same treatment. Left for its own change, #642; until then the refusal is the correct answer, and the only cost is a retry.

## Consequences

- The mutate step's cost leaves the event loop for an install, upgrade or cache hit into any image. Reading the report and applying the file list stay on the loop; both are proportional to the file count but contain no unlinks or copies.
- The checks after recording (`installed_files_missing()`, `header_includes_unresolved()`) still run on the loop. Neither was measured slow.
- A delete is still produced on the loop (#642). A delete in an image whose install is producing gets a 409 for as long as the child runs.
- A daemon restart while a producer runs loses that install, as a restart during a build always has: the entry was in flight and not persisted. The staging is swept at the next start.
- An install's log gains one line naming the version and how long the child took.
