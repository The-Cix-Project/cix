# 0357 — An uninstall is a package job, produced in a helper child

## Status

Accepted, 2026-10-26, for #642. The owner said "Let's do 642?" to the issue, which named the park and the converge change. The mechanism below was not put to them separately. Supersedes two parts of [ADR-0356](0356-an-installs-image-version-is-produced-in-a-helper-child.md): where a version's package snapshot is written (its decision 1), and the 409 a delete answered while its image was producing (its decision 4).

## Context

ADR-0356 moved an install's image version into a helper child and left a delete on the event loop. `pkg_delete()` produced its version with `image_produce_new_version()`, so the snapshot, the unlinks and the placement ran in the daemon, and while an install's child produced the same image a delete answered 409 (`PKG_ERR_IMAGE_PRODUCING`).

Measured on 192.168.15.95 at 0.2.57-601 (`GET /v1/system/stalls`, 2026-10-25), deleting `node` from a scratch image:

```
DELETE /v1/pkg/node@probe617c (825 ms), slowest step image version: write its package snapshot (555 ms), most in unlink old files (59 ms, 1932 calls), then claim index (2 ms)
```

Two things follow from that line. The unlinks were 59 ms, so moving only the build half would have left most of the 825 ms on the loop. And 555 ms went to writing the version's package snapshot (#398), which ADR-0356 kept in the daemon's half, `image_version_commit()`, for installs too. The write is `persist_atomic_write()`, which fsyncs. Why that fsync took 555 ms was not measured. No install on 0.2.57-601 or -602 recorded a slow pass, but the threshold is 750 ms, so that says only that no install's commit crossed it.

The rolling drain calls the delete too. `converge_uninstall_strays()` (ADR-0330) uninstalled every stray in a followed image in one loop inside `pkg_try_start_queued_rebuild()`.

## Decision

1. **An uninstall is a chain job.** `pkg_delete_start()` claims a package job slot for it, `is_delete` set and `produce_e` the entry, and starts its producer with `image_version_fork()` and `delete_mutate()`. A chain job, rather than a second record of uninstalls, because every question about what is in flight already reads the chain slots. One producer per image (`image_has_producer()` and the wait), the drain's `image_has_job_in_flight()`, the retention pass's and `POST /v1/images/gc`'s `pkg_active_chain_indices()`, a second job for the same package (`pkg_job_in_flight_for()`), and `chain_reap_stale()`'s skip all see it with no change.
2. **It takes the highest free slot, and the build ceiling does not bound it.** It builds nothing, so `max_concurrent_jobs` is not its limit. Taking the highest slot means it stands in a build's place below that ceiling only when every slot above is held. `active_jobs` can therefore exceed `max_concurrent_jobs` while one runs, and the contract says so.
3. **It waits for another producer in its image, as an install does.** `image_producer_wait()` is the one wait for both: a waiter child on the other producer, and the job's start again when the waiter exits. A delete no longer answers 409 for that reason.
4. **The daemon's entry stays `installed` until the version is recorded.** The child unlinks the files from its staging copy and then clears its own copy of the entry, so the version is named from the set without it. The daemon clears its entry when the child has exited, just before recording, and puts it back if the record fails. This is ADR-0356's rule from the other side: `installed` means the image's current version holds the package.
5. **A version's package snapshot is written with the tree, in the half the child runs.** `image_version_build()` writes it once the tree is in place, from the set it hashed; `image_version_commit()` only records. This covers installs as well as uninstalls. The snapshot can now exist for a placed tree that the daemon then fails to record. It still describes that tree truthfully, and the old rule (written after the record) existed only so it never described a version that "failed to become real", which a placed tree already is.
6. **The request waits on the job.** `DELETE /v1/pkg/{name}` parks through ADR-0340's record, as a new kind, `PARK_PKGDELETE`, never queued, because pkg.c bounds the jobs. `pkg_delete_ask()` makes the record before the job starts, so no job exists with no record for a request that is still there. When the producer exits, `pkg_produce_completed()` names the finished uninstall and its outcome, `pkg_delete_answer()` serves the request again, and that pass answers 204, the job's error, or 500. A client that leaves does not stop the job: an uninstall that finishes is what was asked for. A failed entry is still cleared at once and answered on the first pass.
7. **The drain uninstalls one stray at a time.** `converge_uninstall_strays()` starts the first stray it can and returns; `pkg_try_start_queued_rebuild()` returns 2 for it, and main.c watches it as an install's producer. The image stays queued and its roll grant unspent, as for an install, and the drain comes back to it when the job is over. A stray whose uninstall fails is marked `stray_refused` and skipped for the rest of that convergence, so a failure is tried once, as it was when one pass tried them all. The marks are cleared when the image leaves the queue.
8. **A hostbuild's artifact directory is removed in the child**, first, as it was first in the daemon (#165). If it cannot be removed, nothing else is and the package stays installed.

## Alternatives

- **A table of uninstalls beside the chains.** Every check in decision 1 would need a second source, and the retention pass would collect a version an uninstall had placed and not yet recorded unless someone remembered to add it there. The chains are already the answer to "what is in flight".
- **Run the uninstall as a park helper (`helper_run_pid()`), as a verification is.** The same invisibility: the child would be the park's, and nothing in pkg.c would know the image had a producer.
- **Move only the build half, and keep the snapshot write in the daemon.** The measured stall was mostly that write.
- **Keep the build ceiling as an uninstall's limit.** A delete would be refused while every build slot is busy, which it never was before.

## Consequences

- An uninstall no longer unlinks, snapshots or fsyncs on the event loop. What stays there is reading the child's report, clearing the entry, recording the version and saving the package state.
- An install's commit no longer writes the snapshot either.
- `DELETE /v1/pkg/{name}` answers when the version is recorded, so its latency is the child's, and a delete into an image another job is producing waits for that job. The 409 it gave for that is gone; it still answers 409 while a job for the same package is in flight, and when every slot is held.
- An uninstall appears in `active_jobs` and `active_job_names` while it runs.
- The drain takes one pass per stray, each after the previous uninstall's exit, rather than all in one pass.
- After a request's uninstall, the retention pass now runs for its image when no job is left, as it does after an install. Before, the mark a delete left waited for the next job to end.
- Clearing a failed hostbuild's partial artifact directory still runs on the loop. It was not measured.
- Renaming an image while a job for it is producing is not refused, as it was not for an install under ADR-0356. Read from the code and not run: the job records by the old name, so it should fail with the entry left installed.
