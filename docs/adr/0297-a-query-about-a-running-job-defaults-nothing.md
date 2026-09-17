# 0297 — A query about a running job defaults nothing: an absent `image` means "whichever one"

## Status

Accepted. Issue [#476](https://git.home.arpa/itdlabs/cix/issues/476). Refines one detail of [ADR-0157](0157-parallel-package-builds-design.md) Phase 2, which introduced `?name=`/`?image=` on `GET /pkg/build/log`; nothing else about that decision changes.

## Context

`cixctl pkg build-log --name=cix` answered `404 {"error":"no build in progress"}` for the whole of a running `cix` host build. Measured on 192.168.15.95, 2026-09-16, during the v2.57.195 host build: the 404 was returned on every attempt, while `GET /v1/pkg` reported the `cix` job in `building` and the build completed normally afterwards.

The cause is one line of normalisation. A build chain is filed under the image it builds for, and `pkg_chain_index_for_target(name, image)` resolves a query by matching both:

```c
const char *norm_image = normalize_image(image);   /* "" -> PKG_DEFAULT_IMAGE, i.e. "base" */
```

`normalize_image()` is correct and load-bearing for every other caller in `pkg.c`. Those callers are *installing* something, and an install with no image has to pick a destination; defaulting it in one place is why nothing downstream carries an "is this empty" special case. But a host build's chain is filed under `PKG_HOSTBUILD_IMAGE` — the internal `__hostbuild` — so the lookup asked for `cix@base`, found nothing, and reported that as a fact about the system.

Two things made it read as a dead endpoint rather than as a lookup miss:

- **The message made a claim it had not established.** "No build in progress" is a statement about the platform, produced from the result of one failed match. An operator watching a build they started was told there was no build.
- **`__hostbuild` is the one image name an operator has no reason to know.** It is internal bookkeeping. `--name=cix --image=__hostbuild` does work, and nothing documents it; it was found by reading the daemon's source.

The same hole applies to any non-default image — a `pkg install --image=myimage` in flight is equally unreachable by name alone. A host build is only how it surfaced, because it is the one build an operator runs by a command that never mentions an image.

## Decision

**On a query about a job that already exists, an absent `image` means "whichever image is building that package", not the default image.**

`pkg_chain_index_for_name()` answers that question: it returns the chain when exactly one in-flight chain carries the name, and reports the match count so the caller can distinguish nothing from several. `pkg_chain_index_for_target()` is untouched, and so is `normalize_image()` — changing either would change what `pkg install` with no image installs into, which is a different decision entirely and a correct one.

The asymmetry is deliberate and is the whole of this ADR: **defaulting is for a request that must choose something; a query about an existing job must not choose, because the job already did.** Two images genuinely building the same package at once is therefore a `400` naming `image` as the way to disambiguate, never an arbitrary pick.

**And an error message about a lookup says what the lookup found, not what the system is.** The 404 for a name that matched nothing now names the builds that *are* running (`pkg_active_chain_names()`, which #246 added for exactly this class of question). The second 404 — the chain exists but has no build container at this instant, during the source fetch, between chain steps, or while the artifact is finalized — gets its own text saying to retry, because it shared the first one's words while meaning something transient and entirely different.

## Consequences

`cixctl pkg build-log --name=cix` attaches to a running host build, which is the command an operator reaches for and the one this endpoint exists to serve.

A caller that relied on `?name=X` with no `?image=` meaning `X@base` specifically would now attach to `X` in some other image if that is the only one building. Nothing does: the only callers are `cixctl pkg build-log` and the dashboard, both of which forward what the operator typed, and both of which want the running build.

**The image-crossing half of this is not gated by a test that runs.** `test_pkg_build_log` is not in `SELFTESTS` — it creates a real build container, which a composed build container cannot do (#224) — and reproducing the actual bug there would need a second image bootstrapped with its own toolchain, for a lookup rule three lines long. What that test does gate, for anyone who runs it, is that the name-only path resolves and that the 404 names what is building.

The image-crossing case is measured on a real host instead, by `recipes/package/probe-buildlog-image/1` — the convention this project already uses for a gate that cannot run where the tests do (`probe-missing-tool/1`, `probe-build-clock/1`). It installs into `cix-builder`, sleeps 90 seconds so there is a window to attach to, and then fails deliberately so no image manifest is touched. Measured on 192.168.15.95, 2026-09-17, under v2.57.197:

```
nothing building,  --name=probe-buildlog-image  -> 404 no build in progress
fetching,          --name=probe-buildlog-image  -> 404 that build has no live output right now ...
building,          --name=probe-buildlog-image  -> 101, streaming "marker-0 ... marker-4"
building,          --name=nosuchpackage         -> 404 ... building now: probe-buildlog-image@cix-builder
build finished,    --name=probe-buildlog-image  -> 404 no build in progress
```

The third line is this decision: the chain is filed under `cix-builder` and the request named no image. The last line is the stale-slot case — a finished job gets the plain 404, not the transient one.
