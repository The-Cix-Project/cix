# 0183 — Persistent volumes: storage whose lifetime is independent of any container

## Status

Accepted

## Context

Everything a container writes at runtime lives in its overlay upper layer, and `DELETE` removes that layer outright (ADR-0106). Until now there was no way to keep **any** container-generated data across a recreate — only a recipe's declared `files[]`, which are re-staged identically for every instance and therefore cannot hold anything a user or workload actually produced.

That is a sharp edge for anything an operator lives in, and it was found the honest way — by an operator hitting it. Logging into the jump box gave no shell prompt; the fix (staging `/etc/profile` from the recipe) solved the shared half, and immediately raised the real question: *"if I create a `~/.bashrc`, does it get deleted on restart or recreation?"*

The answer was yes, and worse than it sounds. `jump` has `follow_rolling: true`, so an ordinary image rebuild recreates it automatically; a recipe edit or package install does too. During a single working session it was recreated roughly a dozen times. Shell history, dotfiles, scratch notes — all silently gone each time, with nothing anywhere indicating that would happen.

## Decision

A **volume** is a named directory whose lifetime is independent of any container using it.

**Deleting a container never deletes its volumes.** That is the entire point, so it follows that a volume needs its own explicit delete — and that delete is refused (409) while **any container definition** still references it, not merely while one is running. A stopped container will come back and expect its data; removing it underneath would be a data-loss bug that only surfaces later.

**Volumes are not a second storage story.** Placement reuses the existing disk-role mechanism container overlays already use (`volume_host_path()` mirrors `container_root_for()` exactly, resolved fresh every time rather than cached), so a `container-storage` disk serves both and there is one answer to "where does container data live". A volume's data directory is a direct child of the base dir, alongside `CONTAINERS_DIR` rather than inside it — nesting storage that deliberately outlives containers *under* container storage would be exactly the wrong lifetime association.

**Mounting happens in the child, before `pivot_root`.** The bind mount must land in the container's own mount namespace, which only exists between `clone3()` and the pivot; doing it there also means the host never accumulates mounts, since they are torn down with the namespace. The mount point is created if missing, so a volume can be mounted at a path the image has no concept of (`/home` on a minimal rootfs) rather than only where an image happened to anticipate one.

**Two failures are deliberately fatal rather than tolerated:**

- A volume that fails to mount kills the container (exit 125). A container that silently started *without* its persistent storage would write to the overlay upper layer instead, look completely healthy, and lose that data on its next recreate — precisely the failure volumes exist to prevent, made harder to notice.
- A `read_only` volume that cannot be remounted read-only also fails, rather than coming up writable. A false guarantee is worse than a refused start.

**A container naming an unknown volume is a hard 400, never an implicit create.** A typo silently producing a brand-new empty volume is exactly how someone loses data and then concludes persistence "didn't work".

The runtime is handed resolved host **paths**, never volume names — `container_spec` knows nothing about the volume registry, keeping the runtime library independent of daemon-side bookkeeping.

## Consequences

- The reported problem is solved: a jump-host `/home` is now a volume, and survives the recreates that `follow_rolling` performs routinely.
- A volume delete is genuinely destructive with no undo. That is why the in-use check keys on *definitions* rather than running containers, why the CLI usage text says so plainly, and why the web delete button is a confirmed danger action.
- Verified end-to-end by `test/test_volume.c` against real containers: data written by one container survives that container's deletion and is read back by a **different** container mounting the same volume — plus every guard rail (invalid name, unknown volume, in-use delete refusal).
- **Backup carries the registry, not the contents.** This line originally said volumes were out of the backup bundle entirely, on the grounds that they are workload data — which conflated two different things and was wrong in a way that broke restore. Container definitions reference volumes *by name*, and an unknown name is a hard 400 by the rule above, so a bundle carrying the definitions but not the registry restores onto a box where every container with a volume fails to start. That is a broken restore, not a partial one. The registry — which volumes exist, and where they are placed — is configuration and is in the bundle; a volume's **contents** are workload data and stay out, which is the boundary ADR-0033 actually draws (it excludes image content on the same grounds). A restore recreates the volumes empty; refilling them is the operator's own concern, exactly as it is for image content. Corrected in place rather than by a superseding ADR, since the decision itself (content is workload data) never changed — only a factual claim about what that implied.
- **Not** included, deliberately, and each worth its own decision rather than a silent default: per-volume quotas (container overlays already have them; a volume is currently an unbounded way to fill a disk), volumes shared between containers concurrently (nothing prevents it today, but no locking or coordination is offered and none should be assumed), and any mechanism for backing up or snapshotting a volume's actual contents.
- A first real bug this design caught in review is worth recording: the create-time parse initially ran *before* `container_spec`'s own `memset()`, so `volume_count` was silently zeroed and containers came up with no volume and **no error at all**. It surfaced only because the test asserted on real data rather than on the API returning 201 — a reminder that for a persistence feature, "the call succeeded" proves nothing.
