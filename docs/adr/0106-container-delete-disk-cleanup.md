# ADR-0106: DELETE /containers actually removes on-disk state (upper/work/merged), and unmounts first

## Status

Accepted

## Context

`DELETE /v1/containers/{name}` (`daemon/src/main.c`'s `handle_delete()`) only
ever called `registry_remove()` and `containerdef_remove()` -- neither one
touches the filesystem. No code anywhere in this codebase ever removed a
container's own `<container_base>/upper`, `/work`, or `/merged`
directories (confirmed by grep for any teardown-context tree-removal
call outside `image.c`'s own image-deletion path). This is a real,
pre-existing disk-space leak for every deleted container, on the OS disk
by default and now potentially worse on an operator-chosen
`"container-storage"` disk too (Phase D, ADR-0102 -- smaller, more easily
filled). Discovered while implementing ADR-0102 and deliberately deferred
to keep that change's scope to disk *selection*, tracked separately as
task #738.

`daemon/include/quotamap.h`'s own doc comment had cited a reason this was
supposedly deliberate: *"ADR-0054's pre-existing backup/restore
design -- a deleted container's files stay on disk, recoverable, until
something else genuinely wipes them."* Checked directly before writing any
fix, per this project's "diagnose before working around" discipline:
ADR-0054 is entirely about host-side per-container stats (CPU/memory/
disk/network) and says nothing about backup/restore at all. The real
backup/restore ADR is ADR-0033, which explicitly scopes workload data
("a container's own persistent data") as *"each container's own concern,
not this endpoint's,"* and reconstructs a restored container via a fresh
`containerdef` replay at the next boot -- never by resurrecting old
upperdir content. No real design anywhere actually depended on this
retention; the citation was stale and incorrect, and the underlying
behavior was a genuine oversight, not a deliberate tradeoff.

## Decision

### Recursive removal, extracted to a shared `persist_remove_tree()`

`image.c` already had exactly this primitive (`remove_tree()`/
`remove_tree_cb()`, a physical post-order `nftw()` walk) for image
deletion, private to that file. Rather than write a second, parallel copy
for container deletion -- exactly the duplication "No Parallel
Implementations" forbids -- it's extracted into `daemon/src/persist.c` as
`persist_remove_tree()` (the natural counterpart to that file's own
existing `persist_mkdir_p()`), and `image.c` now calls the shared version
instead of its own private one. `handle_delete()` resolves the right root
via `container_root_for()` (ADR-0102's own disk-selection-aware
resolution), so cleanup works identically whether the container lived
under the default `CONTAINERS_DIR` or an operator-chosen disk.

### The mount has to come down before the tree does -- a real hazard, not just ordering hygiene

Investigating this surfaced a second, related gap: `overlay_create()`'s
`mount("overlay", ov->merged, "overlay", ...)` runs in the *daemon's own*
root mount namespace (`create_container_from_body()` calls it directly,
before `clone3()` ever forks the container into its own separate
namespace) -- and nothing anywhere in this codebase ever unmounts it
again. The only `umount2()` calls that exist at all are
`mountns_pivot()`'s own old-root cleanup, which runs entirely inside the
container's own child process, a completely different mount namespace
that gets torn down for free when that process exits; it has no bearing
on the daemon's own separate reference to the same mount.

This matters immediately for `persist_remove_tree()`: `nftw()` without
`FTW_MOUNT` freely crosses into a mounted subdirectory. Running the
recursive removal while `merged` was still a live overlay mount would
have walked straight into that live view and started unlinking through
it -- silently mutating (or, for a container that was still running at
the moment of deletion, actively corrupting) the upperdir via the overlay
itself, then failing on the mountpoint's own `rmdir()` (`EBUSY`) partway
through. A real correctness hazard, not a cosmetic one.

`handle_delete()` now unmounts `merged` (`umount2(..., MNT_DETACH)`,
tolerating `EINVAL`/`ENOENT` -- not fatal) *before* calling
`persist_remove_tree()`, and both happen *after* `registry_remove()`
(which `SIGKILL`s the process if it's still running, thawing it first if
paused so the signal can actually be delivered). This ordering --
kill, then unmount, then remove -- is the one that's actually safe.

### Best-effort, never blocks the delete itself

A cleanup failure (unmount or removal) is logged to stderr but never
turns `DELETE` itself into an error response: the registry/`containerdef`
state is the one source of truth for whether a container exists, and
leaving a container definition half-deleted because a directory removal
hit a transient failure would be strictly worse than a lingering
directory. Matches this project's own established "best-effort, never
fatal" posture for cleanup-adjacent operations elsewhere (e.g.
`container_net_teardown_interfaces()`'s own tolerance).

### Scope: a still-mounted `merged` from *before* this fix is not retroactively fixed

A container deleted by an older `kanxeod` build (before this ADR) has a
`merged` overlay mount that was already leaked into the daemon's own root
mount namespace with no registry entry left to find it by -- this fix
only prevents new leaks going forward, on the delete path. A live,
already-leaked mount surviving a daemon *restart* is a separate, real
question (does the mount survive process exit if it's not marked
`MS_PRIVATE`? almost certainly yes for a typical shared-propagation root)
that would need its own investigation (e.g. a boot-time sweep of
`/proc/mounts` for orphaned `container_base/*/merged` entries with no
matching live container) -- not addressed here, and not something this
fix's own test setup (a fresh daemon lifetime, `--data-dir` isolated) can
exercise or verify either way.

## Consequences

- `DELETE /v1/containers/{name}` finally does complete teardown: process
  killed, overlay unmounted, `upper`/`work`/`merged` all removed. Closes
  a real, previously-unbounded disk-space leak.
- `daemon/include/quotamap.h`'s own doc comment, which cited the
  now-confirmed-incorrect ADR-0054 justification, is corrected to point
  at this ADR instead, with the real "why quota project ids are never
  reclaimed" reasoning (recreating a container under the same name later
  should get its old id back, regardless of whether an intervening
  delete happened) kept unchanged -- that part of the reasoning was
  always independently correct, only the *citation* was wrong.
- `image.c` loses its own private `remove_tree()`/`remove_tree_cb()` in
  favor of the shared `persist_remove_tree()` -- one fewer parallel
  implementation, no behavior change to image deletion itself.
- Verified against a real daemon: `test/test_daemon.c`'s existing
  "delete a genuinely still-running container" scenario (`c2`, already
  present to verify process teardown) now also asserts the container's
  own on-disk directory is gone afterward -- the scenario that most
  directly exercises the unmount-before-remove ordering, since a
  stopped-before-delete container's `merged` might already be in a less
  contended state.
- The historical "a mount leaked by an older build survives a daemon
  restart" question remains open, tracked as a follow-up, not silently
  dropped.
