# 0253 — A build output tree is created fresh, never inherited

## Status

Accepted

## Context

[ADR-0251](0251-a-package-artifact-carries-what-the-platform-runs.md)
defined what a package artifact *contains*. It did not say where the
tree that becomes an artifact comes from, and three build output trees
were being reused across builds. Each produced an incident, and each
looked like a different bug:

| tree | what happened |
|---|---|
| `<artifacts>/<name>` | `mkbootroot` reads `<artifacts>/cix/web`. **No `cix` recipe has ever staged it.** Every assembly worked anyway, on a `web/` left behind by an older build that did. Reinstalling the box swept the leftovers away and assembly failed immediately with `No such file or directory`. |
| ISO `stage_dir` | `cp -a src dst` treats an existing directory as "copy INTO here", so the previous build's `payload/seed` became this build's *parent*: `payload/seed/.seed`. The ISO shipped the seed **twice** — 64.6 MiB wanted plus 69.9 MiB duplicated, of 217.9 MiB total — and `cix-install` copied the **outer, stale** tree. A box installed from that media received recipes four days older than the media itself (#307). |
| bootroot `image_root` | A control-plane root carrying `libc.so.6` from one build and `libm`/`libpthread`/`libresolv` from another. Those share a private, version-locked interface (`GLIBC_PRIVATE`), so pid 1 dies instantly and the kernel panics. Cost two resets of a real host. |

A fourth had already been fixed without the rule being stated: build
output left in the artifact directory rode along into the published
package, so every host pulling `cix` downloaded 22 MB to use 1.5 MB of
it (#178). The fix moved the output elsewhere rather than saying that
the directory should have been empty.

The common shape is not "stale files". It is that **absence became
invisible**: a requirement no recipe met was silently satisfied by
history, so the gap could not be observed until something swept the
history away. That is the same defect ADR-0251 names, one level up — it
was about what a tree contains, this is about where the tree comes from.

Note which trees were already correct, because it shows the rule was
understood and simply not applied uniformly: `$PKG_DESTDIR` is
`rm -rf`'d and recreated before every package build, and the ISO seed
directory is `rmtree`'d before every ISO build. Two of five sites had
it. The three that did not are the three that failed.

## Decision

**A build output tree is created fresh. Anything a build produces
starts empty, so what the tree holds afterwards is exactly what that
build put there.**

Expressed as one operation, `persist_fresh_output_dir()`, and not as a
remove-then-create at each site. That is deliberate: the sites that got
this wrong got it wrong **by omission**, and an omission is invisible in
review. A named call that is present or absent is not.

Applied at all three sites that lacked it — the hostbuild artifact
harvest, `mkinstalleriso`'s staging tree, and `mkbootroot`'s image root.
The two that already did it keep their existing, equivalent handling
rather than being churned.

"Fresh" means empty afterwards, so a path that never existed is already
fresh and is not an error; a path occupied by something that is not a
directory is replaced.

## Consequences

- **#307 is fixed at its cause.** The ISO carries one seed because the
  staging tree it is copied into starts empty, not because the copy was
  made more careful. An earlier fix for the same bug had already made
  the copy more careful — it removed the pre-creation of the destination
  — and that was insufficient precisely because it left the inherited
  directory in place.
- **A missing recipe step now fails immediately** instead of being
  masked until an unrelated reinstall. That is the intended cost: the
  `web/` gap existed for an unknown number of releases and was invisible
  for all of them.
- A rebuild can no longer be made cheaper by reusing a previous tree.
  Nothing did this deliberately; recording it in case someone is tempted.
- `test_fresh_output_dir` asserts the property, including a path that
  does not exist and a path occupied by a regular file. Both failure
  modes were reintroduced to prove the gate catches them: creating
  without removing (the original bug in all three tools) and removing
  without recreating.

## Alternatives considered

**Clear only the sub-paths known to be a problem** (`payload/seed`,
`web`). Rejected: that is a list of the failures already found, and each
of these was invisible until it was not. The whole tree is what a build
owns.

**A lint or a test asserting each site calls a remove.** Rejected as the
primary mechanism for the reason above — a gate reports an omission a
human must then fix, where one named operation makes the omission
visible at the call site. The test exists, but it tests the operation,
not the discipline of remembering to use it.
