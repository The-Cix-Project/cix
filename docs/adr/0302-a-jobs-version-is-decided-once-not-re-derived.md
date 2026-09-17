# 0302 — A job's version is decided once, at the start, and never re-derived at the end

## Status

Accepted. Issue [#326](https://git.home.arpa/itdlabs/cix/issues/326). Refines the pinned/rolling recipe resolution of [ADR-0107](0107-package-image-versioning.md) and uses the single failure funnel of [ADR-0272](0272-a-pipeline-run-is-a-log-entry-not-join-state.md); neither decision is reversed.

## Context

`GET /v1/pkg` reported, for the `jumpbox` image, a record whose three fields could not all be true at once:

```
name=glibc  image=jumpbox  state=installed  version=""  error="build failed (exit status 1)
(build container preserved for debugging: __pkgbuild-0 ...)"
```

The state said installed. The version was empty. The error described a build that failed for a recipe revision no longer published. The package really *was* installed — `image materialize jumpbox` reported `glibc already present` and returned ready — so only the record was wrong, and nothing would clear it: re-materializing does not touch an already-present package, `pkg install` answers `409 package is already installed`, and deleting the container the error names removes the container but not the sentence citing it. The only reset was uninstalling a working package.

**The cause is that the version a job installs was decided twice.** `start_fetch_for()` parses the recipe and knows the version at fetch time. `pkg_build_completed()` then threw that away and looked it up again:

```c
if (find_recipe_path(e->name, current_fetch_effective_version(chain_idx), recipe_path,
                      sizeof(recipe_path)) == 0 &&
    parse_recipe(recipe_path, &recipe) == 0) {
        strncpy(e->version, recipe.version, sizeof(e->version) - 1);
        strncpy(e->depends, recipe.depends, sizeof(e->depends) - 1);
}
/* ... a dozen lines later ... */
e->state = PKG_STATE_INSTALLED;
```

Two defects in that pair, and the second does not need the first:

1. **Conditional write, unconditional state.** If the lookup or parse failed, version and depends were left as they were — while the entry was marked INSTALLED regardless. An earlier `memset()` in `start_fetch_for()` (taken whenever the entry was not INSTALLED at chain start, i.e. after any previous failed attempt) had already emptied version. So `INSTALLED` with no version was reachable, and a later failed upgrade of that entry then took the `keep_installed` branch — `state = INSTALLED`, error written, version still empty — which is precisely the measured record.

2. **The second lookup is not the same question as the first.** `current_fetch_effective_version()` returns NULL for everything but an explicit top-level pin, and NULL means *resolve to highest available*. So the completion-time lookup asked "what is the highest available revision now?", not "what did this job build". A newer revision published while a build was in flight would have been parsed instead, and its version written onto an entry that installed a different one — a confidently wrong record rather than a missing field.

Notably, #326's own reproduction depends on withdrawing a revision, and there is no API that withdraws one (a published `(name, version)` recipe is immutable under ADR-0107's versioning model, and `/pkg/recipes/{name}` carries only a GET). Defect 2 makes the analysis independent of that: the re-derivation is wrong whenever the answer can change, and publishing is enough to change it.

## Decision

**A job's resolved version and dependency list are captured once, in `start_fetch_for()`, at the one moment the recipe is certainly present and certainly the right one — and read from there at completion.** They live on `struct pkg_chain` (`fetch_resolved_version`, `fetch_resolved_depends`), which is memory-only, so nothing about the entry's persisted shape changes.

**Every path that reaches a completion sets them.** The two `chain_alloc()` entry points (`pkg_install_start()`, `pkg_hostbuild_start()`) both go through `start_fetch_for()`. `pkg_resume_build()` does not — it reuses a slot by index, deliberately, because `pkg_build_completed()` keys on the chain index the container name encodes — so it sets them from its own parsed recipe, which is the recipe the resumed build runs. Without that, this change would have been a worse bug than the one it fixes: a stale value from a previous job in that slot, written confidently.

**`chain_alloc()` clears them on handout**, for the reason it already clears `fetch_pid`: slots are reused constantly, and a future third start path that forgets to set them should produce the old, visible failure (a missing version) rather than another package's version on this entry.

**The invariant is asserted, not assumed.** `pkg_record_outcome()` — ADR-0272's single funnel every failure and cancellation reaches — logs an error if it ever records INSTALLED with an empty version. It logs rather than repairs: a silent correction would hide whichever new path reopened the hole.

**The build log is named from the captured version too.** `pkg_build_log_open()`'s own comment says the version must be passed in rather than read off the entry, because naming it from `e->version` produced `<name>-unknown-<time>.log` "for exactly the builds most worth finding again". It was then passed `current_fetch_effective_version()`, which is NULL for every unpinned build — so the fallback still landed on `unknown` for the common case. The comment was right about the goal and the argument never reached it.

## Alternatives rejected

**Restore `version` on the failure path.** The narrow fix: have the `keep_installed` branch put back what is installed. It treats the symptom at one of the two sites that can produce it, leaves defect 2 entirely, and keeps two answers to "what version is this job installing" in the code for the next person to pick the wrong one.

**Make the completion-time lookup unconditional and fail the install if it cannot resolve.** Turns a wrong record into a failed install — for a build that actually succeeded, because a recipe elsewhere changed. It punishes the job for something that is not about it.

**Move `error` off the package record onto the run record.** #326's "Expected" also asks for this, and ADR-0272 already made the run the home for per-attempt outcomes, so it is plausible. Not folded in: it is a contract change that alters what `GET /pkg` shows for every failed package on every host, and the issue itself names "an operator and the dashboard read this to decide whether a host is healthy" — that workflow is correct for a package that failed and was never installed. It deserves its own assessment rather than riding along with a bug fix.

## Consequences

An entry's version now always names what that job installed, including when the recipe set changes mid-build, and `depends` follows the same rule for the same reason. Build log filenames carry the real version instead of `unknown` for every unpinned build, which is most of them.

**No automated gate covers this.** `test_pkg` is not in `SELFTESTS` (#224, #480), and `test_pkg_recipe_approval` — which is in it — tests recipe immutability rather than install outcomes; read rather than assumed. The reproduction in #326 additionally needs a recipe withdrawal that no API performs. So the verification is that the platform's own hostbuild exercises the capture path end to end on every deploy: if the captured version failed to reach the entry, the `cix` package would report an empty version after installing, and `GET /v1/pkg` across every record would show the inconsistency the new assertion also logs.
