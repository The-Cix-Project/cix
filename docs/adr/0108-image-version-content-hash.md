# ADR-0108: image versions are a manifest-content hash, not a sequential integer

## Status

Accepted -- supersedes part of [ADR-0107](0107-package-image-versioning.md) (the
"an image version is a plain sequential integer" decision only; every other
part of ADR-0107 -- version-keyed recipes, the natural-sort comparator, the
persisted manifest, per-version immutable rootfs, container pinning, rolling
auto-rebuild -- is unchanged and this ADR builds directly on top of it).

## Context

ADR-0107 originally decided an image version identifier is a plain
sequential integer (1, 2, 3, ...), reasoning it was "simpler,
human-readable, matches this project's existing sequential-id conventions."

Revisited before any `image.c` code was written against that decision (only
`pkg.c`'s own recipe-versioning half of ADR-0107 existed yet -- no manifest
or per-version rootfs code had landed), for two real reasons:

- **Consistency, not just convenience.** A sequential counter is
  process-local bookkeeping -- it says nothing about *what* a given version
  actually contains. Two independently-triggered rebuilds of the same
  image, resolving to the exact same manifest (e.g. an operator re-running
  a rebuild that changed nothing, or two nodes in a future multi-daemon
  setup independently rebuilding the same image), would mint two different
  sequential numbers for byte-identical content -- a real discrepancy this
  design should not allow, given the whole point of ADR-0107 is giving an
  operator a trustworthy, unambiguous handle on "what a container is
  actually running."
- **A content-derived identifier is naturally self-describing and
  collision-safe.** If the identifier itself is a hash of exactly what's in
  the image, "two images claim to be version 7 but disagree on content" is
  structurally impossible -- there is no version number to disagree about,
  only a hash that either matches or doesn't.

## Decision

**An image version identifier is `sha256(canonical manifest string)`**, not
a sequential integer. The canonical string is the image's fully *resolved*
manifest -- every `{package, resolved_version}` pair (pinned resolves to
itself; rolling resolves to the actual highest-qualifying version chosen at
build time), sorted by package name, joined as `name@version,name@version,...`
with no other metadata folded in. Computed via the real `sha256sum` binary
(`PKG_SHA256SUM_BIN`, `pkg_run_capture_sha256()`'s own existing mechanism),
never hand-rolled -- the same "never hand-rolled crypto" rule already
governing every other checksum in this codebase.

**Scope is manifest identity, not rootfs byte content.** Hashing the actual
built file tree (every file's bytes) was considered and rejected: it would
require hashing potentially gigabytes on every install, fight directly
against the copy-forward/hardlink incremental-rebuild design ADR-0107
already committed to, and would treat two builds as "different" for
meaningless reasons (embedded build timestamps, mtimes, on-disk inode
order) even when the declared package set -- the thing an operator actually
reasons about -- is identical. Manifest identity is the right granularity:
"these are the exact packages, at these exact versions" is precisely what
ADR-0107 exists to make trustworthy.

**Directory layout becomes `IMAGES_DIR/<image>/<hash>/rootfs`**, replacing
the sequential `<image>/<N>/rootfs` ADR-0107 originally specified. `struct
registry_entry`'s new field (ADR-0107's `image_version`) is a hash string,
not an int; the persisted create-request body and `GET /containers`'
echoed field follow the same shape.

**Deduplication falls out for free.** If a rebuild resolves to a manifest
whose hash already exists as a version of that image, there is nothing new
to create -- the image's `current_version` pointer is left exactly as it
is. This replaces the separate "detect a no-op rebuild and skip it"
comparison logic ADR-0107 implied would be needed; with content-addressed
identity, "already exists" and "is a no-op" are the same check.

**No more implicit "highest directory name" for current version.** A
sequential integer's own max was, by construction, the current version;
hashes have no ordering. Each image's `manifest.json` (ADR-0107's own new
persisted state) gains an explicit `current_version` field (the hash
string) updated only when a rebuild actually produces a *new* hash, plus a
`versions` array recording every hash this image has ever produced.

**Each version's manifest entry gets a real, explicit `created_at`
timestamp** (Unix epoch seconds, `time(NULL)` captured once, at the moment
that version is first created) -- not derived from filesystem mtime.
Directory mtimes are a fragile proxy here: the copy-forward step
(`cp -al`, hardlinking the previous version's tree forward before applying
an incremental delta) touches directory metadata in ways that would make
"stat the rootfs dir" an unreliable stand-in for "when was this version's
content first resolved." An explicit field in the one already-authoritative
persisted document (`manifest.json`) is One Source of Truth; a second,
implicit, filesystem-derived value alongside it would not be.

**Recipe versions also gain a `created_at`, but via a different, correct
mechanism: real filesystem mtime, not a new persisted field.** Unlike image
version directories, a recipe version's own file
(`pkg/recipes/<name>/<version>/recipe.sh`) is written exactly once via
`persist_atomic_write()` and is never touched again -- ADR-0107's own
immutability rule. Nothing in this codebase's recipe pipeline ever
copies-forward, hardlinks, or otherwise mutates an already-published
recipe file's metadata, so its mtime *is* an accurate, permanent "first
published" timestamp with zero extra persisted state -- reading it back
(`stat()`) is the smaller, more honest design than duplicating a value the
filesystem already preserves correctly. `pkg_recipe_get()` and
`pkg_write_json_recipes()` both surface it as `created_at` in their JSON
output.

## Consequences

- ADR-0107's "N a plain sequential integer, starting at 1" sentence (under
  "Each image version gets its own immutable rootfs") is superseded by this
  ADR; that section of ADR-0107 is left as written (not edited) per this
  project's own ADR mutability rule -- a reversed decision gets a
  superseding ADR, never an in-place rewrite.
- Every other part of ADR-0107 (version-keyed recipes, the natural-sort
  comparator, per-image persisted manifests, per-version immutable rootfs,
  container pinning, rolling auto-rebuild) is unaffected and remains the
  governing design.
- A `manifest.json` now carries two kinds of state instead of one:
  operator *intent* (the `{package, mode}` list ADR-0107 already
  specified) and build *history* (`current_version` + the `versions` array
  with each entry's `created_at`) -- both belong in the same file (one
  persisted document per image, One Source of Truth), not split across two.
- Hash-named version directories are less immediately readable in a bare
  `ls` than sequential integers were; this is mitigated by `created_at`
  being real, queryable data (via REST/CLI, task #721) rather than
  something an operator would ever need to infer from a directory listing
  in the first place.
