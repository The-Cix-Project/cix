# ADR-0107: package + image versioning model (pinned/rolling, per-version immutable rootfs)

## Status

Accepted

## Context

Today, an "image" (`daemon/src/image.c`) is pure filesystem state: a single
directory, `IMAGES_DIR/<image>/rootfs`, with no persisted metadata of its
own -- an image *is* its directory. `pkg install` (`daemon/src/pkg.c`)
merges a built package's files directly into that one shared, mutable
rootfs. A recipe (`pkg/recipes/<name>.recipe`) is one file per package
name; installing a newer `pkg_version=` overwrites the recipe file in
place (upsert) and merges the new build over the old one's files in the
image.

This has a real, serious correctness problem, the one that started this
whole redesign: `overlay_create()`'s lowerdir is `IMAGES_DIR/<image>/rootfs`
directly, and overlayfs does *live* directory lookups against its
lowerdir, not a frozen snapshot taken at mount time. A container created
last week, still running right now, shares that exact same live directory
as its own lowerdir -- so installing (or upgrading) a package into that
image today silently changes what that already-running container sees,
with no isolation between "what a container was created against" and
"whatever the image currently contains." There is no way, today, to
upgrade a package for future containers without also, invisibly,
upgrading it out from under every container already running against that
image.

User-confirmed design (2026-08-09), refined across a clarifying round
before implementation:

- **A package version is the real upstream version string** (e.g. curl's
  own `"8.21.0"`), not an internal recipe-revision slot number -- an
  operator expressing "pin curl at 8.20" or "roll curl forward, but never
  below 8.20" means the real upstream version, and needs real ordering
  comparison to make "rolling >= floor" mean anything.
- **An image version is a plain sequential integer** (1, 2, 3, ...) per
  image, incrementing on every rebuild -- not a content hash. Simpler,
  human-readable, matches this project's existing sequential-id
  conventions (registry slots, disk roles) rather than introducing a new
  identifier shape.

## Decision

### Recipes become version-keyed: `pkg/recipes/<name>/<version>/recipe.sh`

One directory per package name, one file per version, named after the
recipe's own real `pkg_version=` field (e.g.
`pkg/recipes/curl/8.21.0/recipe.sh`) -- the directory-per-version shape
rather than `<name>/<version>.recipe` as a flat file, because a future
version may need more than one file alongside the recipe script itself
(this project has no such need yet, but a flat `<version>.recipe` name
would make adding one a breaking rename later; the directory form costs
nothing extra now and avoids that). The filename-parent directory and the
recipe's own internal `pkg_version=` must agree -- the exact same
"filename and `pkg_name=` agree" invariant `pkg_recipe_add()` already
enforces for the package name, extended to the version.

**Recipe versions are immutable once added, matching the image-version
immutability this whole design is built around.** `POST /v1/pkg/recipes`
adding a `(name, version)` pair that already exists is now a real error
(`PKG_ERR_DUPLICATE`), not a silent overwrite -- a deliberate behavior
change from today's upsert-by-name semantics. Fixing a mistake in an
already-published recipe version means publishing a new version, the
same "an already-built artifact doesn't get edited in place, a new one
gets built" discipline this design applies to images too. No migration
shim for the old overwrite behavior -- clean cut-over, per this project's
own standing convention.

**Version comparison is real dotted-numeric ordering**, not opaque string
compare: split on `.`, compare each component as an integer, a shorter
version treated as zero-padded (`"8.2"` < `"8.2.1"`). Every `pkg_version=`
across this project's own 60+ existing recipes is already pure
dotted-numeric (`"1.47.4"`, `"7.1"`, `"3.0.5"`, ...) -- no need for a full
semver parser (pre-release suffixes, build metadata) this project has no
real recipe using today; this can grow if a real recipe ever needs it,
not speculatively now.

**Existing recipes migrate to their own already-published version as a
one-time move**, not a new "version 1" fiction -- `curl.recipe`'s own
`pkg_version="8.21.0"` becomes `pkg/recipes/curl/8.21.0/recipe.sh`
verbatim. A real, scripted migration pass (not hand-editing 60 files),
run once as part of this change landing.

### Images gain a real, persisted manifest

New persisted state per image (`daemon/src/image.c`, a new
`IMAGES_DIR/<image>/manifest.json`, following this project's existing
`persist_atomic_write()`-based JSON persistence convention, ADR-0012) --
a list of `{package, mode}` where `mode` is either `{"pinned": "8.21.0"}`
or `{"rolling": "8.20.0"}` (the floor). This is a genuine architectural
shift for `image.c` -- images stop being "pure filesystem state, no
persisted registry" (this file's own prior doc comment) the moment they
gain an operator-declared *intent* about what should be in them, distinct
from what happens to be built right now.

### Each image version gets its own immutable rootfs

`IMAGES_DIR/<image>/<N>/rootfs` (N a plain sequential integer, starting
at 1) replaces the single `IMAGES_DIR/<image>/rootfs`. Building version N
means: create a fresh `<N>/rootfs`, run `pkg_seed_image_baseline()` on it
(unchanged), then for every `{package, mode}` in the manifest, resolve
the target version (`pinned` = exactly that version; `rolling` = the
highest available version `>=` the floor) and run the *existing*
fetch/build/merge pipeline (`pkg_fetch_completed()`/`pkg_build_completed()`,
structurally unchanged) merging into `<N>/rootfs` instead of a shared
path. A rebuilt image version is written once and never mutated again --
the property that actually fixes the original bug: a container's own
overlay lowerdir, once resolved, points at a specific, permanently-frozen
directory tree that nothing will ever write into again.

### Containers pin to the exact image version they were created against

`create_container_from_body()` resolves the image's *current* version at
creation time (the image's own persisted "current version" pointer --
itself just "the highest version directory that exists," no separate
pointer file needed) and uses `IMAGES_DIR/<image>/<N>/rootfs` as the
overlay lowerdir. That resolved `N` is recorded in `struct registry_entry`
(a new `image_version` field) for the live process, **and** written back
into the persisted create-request body `containerdef_add()` stores (a
server-computed field alongside whatever the operator's own request
contained -- the client never sets this directly, the same trust-boundary
shape `cmd`/other server-derived fields already have). This matters for
restart replay and `POST .../start`: both reuse the *stored* body
(ADR-0033's own "the raw create request is a complete reconstruction
source" design, and ADR-0045's `.../start` replay) -- without the pinned
version traveling with that stored body, a restart would silently
re-resolve to whatever is *now* current, defeating the whole point of
pinning. `pkg_get_one()`/`GET /containers` echo `image_version` so an
operator can see exactly which version a given container is actually
running against.

### Rolling images auto-rebuild automatically, queued through the existing single-job constraint

The moment a new recipe version is added (`POST /v1/pkg/recipes`) for a
package that some image's manifest tracks as `rolling` with a floor at or
below the new version, that image is queued for a rebuild -- automatic,
not manual, per explicit user confirmation. Also hooked into the
hostbuild-completion path the same way, for consistency (a hostbuild that
happens to publish a new recipe version -- not a case this project uses
today, but the same trigger point, not a special case). Multiple images
qualifying at once are queued (a small FIFO, reusing this daemon's
existing "only one fetch/build job in flight" v1 constraint rather than
inventing parallel job tracking) and drained one at a time as the single
in-flight slot frees up.

### Bare `pkg install NAME` (no manifest involved) targets the newest version

`pkg_install_start()` gains an optional explicit version parameter (used
by manifest-driven image builds); when omitted (every existing caller:
direct CLI/REST install, hostbuild, tests), it resolves to the highest
available version for that name -- matching the "rolling" default posture
and keeping every pre-existing, non-manifest-aware call site working
unchanged.

## Consequences

- Closes the real bug this design exists to fix: a package upgrade can
  never again silently change what an already-running container sees.
  Verified directly (task #722): two containers created against the same
  image name at different versions (one pinned, one after a rolling
  rebuild) see different package contents, proven live.
- `image.c` goes from "no persisted state, pure directory scan" to owning
  real persisted manifests -- a genuine new category of daemon-owned
  state, backed by the same `persist_atomic_write()` primitive every
  other persisted resource already uses, not a new mechanism.
- Recipe and image-version immutability both mean disk usage only grows
  over time within this design's own v1 scope -- no automatic pruning of
  old recipe versions or old image-version rootfs trees is built here.
  An operator can still remove an old image version's directory by hand
  if disk space matters (best-effort, not a REST-exposed capability yet);
  automatic retention/pruning policy is a real, separate, future decision,
  not silently assumed.
- `POST /v1/pkg/recipes` behavior changes for an existing `(name, version)`
  pair: was a silent overwrite, is now a rejected duplicate. Any external
  tooling relying on the old upsert-by-name-only behavior needs updating
  -- flagged here explicitly, not silently changed.
- Every existing recipe is migrated to a real, already-published version
  directory as part of landing this change -- no recipe keeps working
  under its old flat-file path.
