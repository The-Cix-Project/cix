# ADR-0270: A deployment waits for its image rather than being refused

- Status: Accepted
- Date: 2026-09-10
- Issue: [#371](https://git.home.arpa/itdlabs/cix/issues/371) (Stage 3, "Deploy B")
- Supersedes nothing. Builds on [ADR-0256](0256-the-pipeline-is-the-model.md) (the stage/status model),
  [ADR-0269](0269-one-pipeline-model-for-four-kinds.md) (that model generalised to four kinds),
  [ADR-0151](0151-container-recipes.md) (what a deployment is),
  [ADR-0124](0124-pkg-redesign-part5-rolling-containers-and-restart-jitter.md) (`follow_rolling`),
  and [ADR-0181](0181-persist-all-containers-restart-decoupled-from-existence.md) (every container has a persisted definition).

## Context

Applying a deployment whose image has not been built refuses:

```c
} else if (image_current_version(image, resolved_image_version, ...) != IMAGE_OK) {
        snprintf(err_msg, err_msg_size, "image rootfs does not exist");
        return 400;
}
```

`daemon/src/main.c:12252`. The operator's next move is always the same: go and realize the image,
then come back and apply the deployment again. The platform knows which image is named, knows
whether it is realized, and owns the machinery that realizes one — so the round trip is work the
operator is doing on the daemon's behalf.

ADR-0269 recorded the asymmetry that makes this stand out. **image → package already forks**:
installing a package into an image builds it from its recipe when no artifact exists. That is the
owner's "(build) forks", real and working. **deployment → image does not.** Two of the three
recipe kinds compose automatically and the third does not, for no reason anyone chose.

The owner's decision (2026-09-10) is to close the asymmetry: auto-fork.

## What "fork the image build" actually means, measured

The obvious reading — "start the image build job" — describes something that does not exist.
`pkg_image_recipe_apply_start()` builds nothing. Its own contract says so:

> Applying a recipe (`pkg_image_recipe_apply_start()`) is synchronous bulk-declare:
> `image_manifest_set()` for every entry, exactly what N manual
> `PUT /v1/images/{name}/manifest` calls would do. Packages still need real `pkg_install()`
> calls afterward to actually build, same as any other manifest edit.

`daemon/include/pkg.h:1430`. Applying an image recipe writes a *declaration*. What converges a
declaration into a real rootfs is a different machine, and it already exists: the rebuild queue
(`rebuild_queue_enqueue()`) drained by `pkg_try_start_queued_rebuild()`, which respects the job-slot
budget, drops an image whose manifest is already fully satisfied without starting anything, and is
called from every job-completion path in `main.c` plus once per event-loop pass whenever
`pkg_rebuild_queue_depth() > 0`.

So the decision is not "write a build driver". It is **record the intent, put the image on the queue
that already exists, and replay the create when the image is realized.** The build is the queue's
problem, as it already is for a rolling package publish.

## Decision

### 1. The fork decision lives in the apply handler, never in `handle_create`

`POST /v1/containers` with a missing image still returns 400, immediately, unchanged. That path is a
primitive: it is what `containerdef_autostart_all()` replays at boot, what
`handle_restart_timer_event()` drives on a crash, and what every test and client calls directly.
Making a primitive wait would turn a fast, honest failure into a silent hang in five places that
never asked for one.

`POST /v1/deployments/{name}/apply` is the declarative surface — the place where "this is what I
want to exist" is the whole meaning of the call. Waiting belongs there and only there.

### 2. "The image is missing" is four different conditions, and only two fork

| Condition | Test | Answer |
|---|---|---|
| No `manifest.json` at all | `image_manifest_read()` → `IMAGE_ERR_NOT_FOUND` | 400, naming both absences |
| Unrealized, manifest has entries | current version == `image_empty_manifest_version()` | **fork**: enqueue |
| Unrealized, manifest empty, image recipe exists | above, plus `entry_count == 0` | **fork**: apply the recipe, then enqueue |
| Unrealized, manifest empty, no image recipe | above, no recipe | 400, naming both absences |
| Version directory named but absent on disk | `stat(lowerdir)` fails | 400 — corruption, not an unbuilt image |

Two things in this table were nearly got wrong, and both would have shipped.

**The wait condition is `empty-manifest version`, and nothing else.** The first draft also forked when
the manifest had unsatisfied entries. That is the *drain* criterion — what
`pkg_try_start_queued_rebuild()` uses to decide whether an image needs a job — and borrowing it here
would have made waiting the common case: every deployment naming a perfectly working image with a
rolling entry that has a newer recipe available would have returned 202 and waited, instead of
creating against the version that exists. Upgrading an already-created container is what
`follow_rolling` (ADR-0124) is for. The apply asks one question — *is there anything here yet* — and
`image_empty_manifest_version()` is exactly that question:

> The version every image is born with: the hash of its own empty manifest, written by
> `image_create()` before anything has been put in it. Callers that need to tell "this image exists"
> apart from "this image has been filled" compare against this rather than against a hardcoded
> digest — the two questions are genuinely different, and conflating them is how a failed
> build-environment composition once left behind an empty image that every later build accepted as
> ready (issue #109).

**`image_current_version()` returning `IMAGE_OK` does not mean the image is realized.** Every image is
born with a valid current version and a real rootfs directory containing nothing. Had the fork
condition been "no current version", a deployment naming a freshly created empty image would have
sailed past the check and created a container against an empty rootfs — #109 again, one layer up.

**An empty manifest needs the recipe declared first, or it waits forever.** Image at the empty
version with zero manifest entries is the *most common* fresh-image shape: image created, recipes
synced from git, nothing declared yet. Enqueuing it alone deadlocks quietly — the drain sees a
manifest that is trivially satisfied, drops the image without starting a job, and the pending
deployment sits at `acquire`/`blocked` with nothing that will ever unblock it. So for that shape the
apply declares the image's own recipe into the manifest first (`pkg_image_recipe_apply_start()`,
which is precisely bulk-declare and nothing more) and enqueues after. With no recipe there is nothing
to declare toward and no honest way to wait, so it is a 400 that names both absences.

Convergence is still driven from the **manifest**, not the recipe: a manually authored manifest with
no recipe behind it is a legitimate image, and the queue already converges exactly that. The recipe,
where one exists, only populates the manifest.

**A name already in use is a 409, not a wait.** `handle_create()`'s duplicate-name check sits at
`main.c:12299`, *after* the image check at 12252, so a re-apply of an already-running deployment
would go pending rather than conflict if the fork branch ran first. The apply handler therefore
consults `registry_find()` before deciding to fork: an existing container falls through to today's
path and gets its 409.

### 3. The pending record is a `container_def`, not a new registry

A deployment waiting for its image is a container that has been declared and not yet created.
`struct container_def` already holds precisely that: the verbatim create body, the restart policy,
the dependency list, and `follow_rolling`. It is persisted through `save_state()`, replayed at every
daemon start by `containerdef_autostart_all()`, and already read by `pipelineview.c`'s
`write_deployments()`.

So the pending state costs **one persisted field**, `awaiting_image`, and inherits persistence,
restart replay and pipeline visibility rather than reimplementing all three. A second registry for
"deployments that are waiting" would be a parallel implementation of a definition list that already
exists — the thing the maxims exist to prevent.

**`awaiting_image` holds the image's name, not a flag.** Both readers need the name and neither
should have to parse the create body to get it: convergence must know which image each pending def
is waiting on, and `write_deployments()` must emit `blocked_on: {kind: "image", name: X}`. An empty
string means "not waiting", so it is still one field and still one test.

The persisted form is JSON with named keys, so the new key is additive: a state file written by an
older daemon simply has no `awaiting_image` and parses as empty, which is the correct meaning. No
migration.

**Replay of a pending def ignores `restart_policy`, and this is the one place the inherited machinery
does not fit unchanged.** `containerdef_autostart_all()` skips `restart:"no"` defs unconditionally,
correctly — ADR-0181 defines that policy as "never auto-restart, boot included". But a pending
deployment has never been created, so restarting is not what is being asked for: its policy has not
come into play yet and cannot be the thing that suppresses its first create. The boot pass therefore
tests `awaiting_image` **before** the policy check and before `create_container_from_body()` — a
pending def whose image is still unrealized re-enqueues that image and moves on, rather than
attempting a create that is known to fail and reading the 400 back. Once it converges it is an
ordinary def and every existing rule applies to it normally.

**Promotion is explicit, because neither replay path persists a definition.** This was nearly got
wrong on an assumption. `containerdef_add()` looks like the thing that would clear the wait — it is
what stores a definition after a successful create — but it lives in `handle_create()`, and *neither*
replay path goes through `handle_create()`. Both call `create_container_from_body()` directly, and
that function does not touch the definition list at all. Left as assumed, a converged deployment
would have run correctly while still reporting itself blocked on an image it already had, in both
the boot path and the convergence path.

So a successful replay calls `containerdef_promote_pending()`, which clears `awaiting_image` and
writes the policy fields from `create_container_from_body()`'s own out-params. It is deliberately not
`containerdef_add()`: that frees the stored body before copying the one it is handed, and both
callers pass that same body straight back — a use-after-free. Nothing about the body changes on
promotion anyway; only the fields around it do.

That is also why the apply stores the policy fields at their **defaults** rather than parsing them
out of the rendered body first. The authority on what `restart` means is
`create_container_from_body()`, and promotion carries its answer across; parsing them a second time
at apply would be two places deciding the same thing, which is how the two come to disagree. Nothing
reads those fields while a definition is waiting — both replay passes test `awaiting_image` before
they reach policy, and `pipelineview` reports the waiting state rather than the policy.

The pinned `image_version` follows the same shape. `containerdef_add()`'s caller splices the resolved
version into the persisted body, and a pending def has no resolved version because there is nothing
yet to resolve — so it is persisted without one, and `containerdef_patch_image_version()` (which
already exists for exactly this kind of after-the-fact pin) applies it on promotion. That is correct
rather than a special case: `image_version` pins a container to the version it was *created against*,
and a container that has not been created has not been pinned to anything.

### 4. Convergence reuses the one hook that already exists

`try_start_queued_pkg_rebuild()` is called after every package-job completion, and its own comment
already states the principle this decision follows:

> this same "a pkg job just finished" moment is exactly when an image's `current_version` might have
> just moved (a rolling rebuild completing is itself one more pkg job), so it's the natural single
> hook for reconciling any `follow_rolling` container against it too — one call site, not two
> independently-triggered mechanisms.

Replaying pending deployments is the same reconciliation against the same moved version, so it joins
`apply_rolling_container_restarts()` at that one call site. There is no new timer, no new queue and
no callback from the image build back to the deployment.

### 5. Failure propagates at read time, because ADR-0256 says status is derived

Nothing stores "this deployment failed because its image failed". A pending deployment is reported at
stage `acquire` with status `blocked` and `blocked_on: {kind: "image", name: X}`, and becomes
`failed` when X's own pipeline reports a failed package. Both are computed by the same read-time join
ADR-0256 established and ADR-0269 generalised. The edge that carries the failure is the
deployment → image edge, which ADR-0269 already made structural — it exists because the deployment
*names* the image, not because something went wrong, so there is nothing to create at failure time.

**A replay can also fail for a reason that has nothing to do with the image** — a name taken in the
meantime, a network that no longer exists, an env key the renderer produced that validation rejects.
The image is realized, so waiting longer changes nothing, and retrying every event-loop pass forever
would be the silent wedge-adjacent shape this change exists to avoid. So: the def **stays** pending,
its last replay error is kept (in memory, alongside `consecutive_failures`, which is already
deliberately not persisted for the same reason — a stale failure from before a restart is not
meaningful), and `write_deployments()` reports it at `acquire`/`failed` with that message rather than
`blocked`. Replay is attempted only when the image's version actually moves, not on every pass, so a
permanently broken def costs one attempt per image change and stays visible instead of spinning. A
re-apply retries it deliberately.

### 6. Apply returns 202, never blocks

The response is `202 Accepted` with the pending state, immediately. An apply that used to fail fast
must not become an apply that holds a connection open across a package build — that is the one way
this change could wedge a box, and it is the reason #371 gave Deploy B its own deploy and its own
rollback.

## Consequences

- An operator declares a deployment once and the platform realizes it, matching what installing a
  package into an image has always done.
- A deployment can now exist in a state where it has never run. `GET /v1/deployments` and the
  pipeline page both show it as `blocked` on a named image, so it is visible rather than merely
  absent.
- **The rebuild queue is in-memory** (`static char g_rebuild_queue[][]`, `daemon/src/pkg.c:676`) and
  does not survive a daemon restart. This change leans on that queue, so a pending def re-enqueues
  its image during the startup replay. Whether the queue should instead be re-derived at startup for
  *every* unsatisfied image is a real and larger question — a rolling rebuild queued by a recipe
  publish is silently lost across a restart today, with nothing to notice — and is filed as [#373](https://git.home.arpa/itdlabs/cix/issues/373)
  rather than fixed here.
- **The vestigial `pkg_any_job_busy()` check in `pkg_image_recipe_apply_start()` is removed as part of
  this change** (`daemon/src/pkg.c:11663`). Measured: the manual `PUT /v1/images/{name}/manifest`
  path calls `image_manifest_set()` with no busy check at all (`handle_image_manifest_set()`,
  `daemon/src/api_image.c:409`), and the two do exactly the same thing — declare entries into a
  manifest. The check is a leftover from the whole-rootfs artifact fetch ADR-0209 removed, back when
  apply really did fetch and extract; nothing it guards remains. Leaving it would have forced this
  change to choose between two bad options: refuse an apply whenever any package job happens to be
  running, or hand-roll the same `image_manifest_set()` loop in the apply handler — a parallel
  implementation of a function that already exists. Removing one stale condition is the smaller and
  more honest change, and it makes the recipe path and the manual path agree, which they should have
  all along.

## Alternatives considered

**Refuse, as today.** Rejected by the owner's decision. It also leaves the platform in the odd
position of forking one composition edge and not the other.

**Fork inside `handle_create`.** Rejected: it changes a primitive that four other callers depend on
failing fast, including the boot-time replay, where a wait would delay every subsequent container.

**A separate persisted `pending_applies` registry.** Rejected: `container_def` already is that
registry, with persistence and restart replay working. A second one would be a parallel
implementation and a second source of truth for what deployments exist.

**Block the apply until the image is ready.** Rejected: it is the wedge #371 named, and a package
chain can run for tens of minutes.

**Fork from the image recipe rather than the manifest.** Rejected: an image can legitimately have a
manifest and no recipe, and the manifest is what the convergence machine already reads.
