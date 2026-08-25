# Developing against a real remote box, with no SSH

How to push local (or even entirely server-compiled) changes onto a real, already-installed Cix host and verify they actually took effect — without SSH, without a general shell, and without reinstalling from an ISO each time. This is the operational counterpart to [`building-cix.md`](building-cix.md) (which covers compiling) and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)/[`staying-updated.md`](staying-updated.md) (which cover the update endpoints themselves) — this page is the "how do I actually get a file onto the box in the first place, and prove it landed" walkthrough neither of those needed to be, because a real Cix install genuinely has no other way in.

## Why this needs a trick at all

A real installed Cix host has no SSH server and no general shell (ADR-0034, confirmed directly — `nc -z <box> 22` closed on a fresh install). `POST /system/update`'s `image_path`/`kernel_path` fields, and `cixctl update --image=PATH`, are **paths the daemon reads from its own local disk** — not an upload endpoint. `cixctl`'s `update` subcommand just forwards whatever string you give it straight into the JSON body (`cli/src/main.c`'s `cmd_update()`); it never reads the file itself. So a locally-built artifact needs a real way to land on the *remote* daemon's own filesystem before `system/update` has anything to point at.

## The core trick: reuse the package-fetch pipeline as a generic file-transfer primitive

`pkg install`'s own fetch step already does exactly what's needed — the daemon `curl`s a URL host-side, verifies it against a `pkg_sha256`, and lands the raw download at a real, predictable local path (`<data-dir>/pkg/sources/<name>-<version>-0.src`) before ever trying to build anything. A throwaway scratch recipe pointed at an arbitrary file, served from your own dev machine, turns this into a general "get this file onto the remote box's disk" mechanism — the eventual build/install step is expected to fail (the file usually isn't a real tarball), but by then the only part that matters, the fetch + checksum, has already succeeded.

1. **Serve the artifact over your LAN**, from wherever you built it:
   ```sh
   cd /path/to/artifacts && python3 -m http.server 8904 --bind <your-lan-ip>
   ```
2. **Compute its real checksum** and write a scratch recipe:
   ```sh
   sha256sum cixd-root.squashfs
   ```
   ```
   pkg_name="scratch-deploy"
   pkg_version="1"
   pkg_source="http://<your-lan-ip>:8904/cixd-root.squashfs"
   pkg_sha256="<the real sha256 above>"
   pkg_depends=""

   pkg_build() { true; }
   pkg_install() { true; }
   ```
3. **Push the recipe and trigger a fetch** (see [`writing-recipes.md`](writing-recipes.md) for the full recipe format):
   ```sh
   cixctl --host=<box> pkg recipe add --name=scratch-deploy --file=scratch-deploy.recipe
   cixctl --host=<box> pkg install --name=scratch-deploy
   ```
   Poll `GET /pkg/scratch-deploy` until it leaves `fetching`/`building`. `state: "failed"` with `"could not prepare the build container (extract source tarball failed)"` is the **expected, harmless** outcome for a non-tarball artifact — it means the fetch and checksum verification already succeeded, which is all this step is for.
4. **Read back the real local path**: `<data-dir>/pkg/sources/scratch-deploy-1-0.src` (the default data dir is `/var/lib/cix`).
5. **Clean up** once you're done: `cixctl --host=<box> pkg recipe rm scratch-deploy` (a package left in `PKG_STATE_FAILED` can't be `DELETE`d via the package endpoint — that's expected, not a bug; the recipe delete is what matters).

## Writing it to the inactive slot, and a real gap to know about

```sh
cixctl --host=<box> update \
  --image=/var/lib/cix/rebuildable/pkg/sources/scratch-deploy-1-0.src \
  --kernel=/var/lib/cix/rebuildable/pkg/sources/<kernel-scratch-path>
cixctl --host=<box> reboot
```

**`--image=`/`--kernel=` can be supplied independently -- a one-sided update no longer leaves the other file stale (ADR-0095).** This used to be a real footgun, discovered the hard way: a root-only update once landed in a slot whose kernel predated a since-fixed config, and the very next boot regressed to a bug that had already been fixed elsewhere. `POST /system/update` now auto-fills whichever half is omitted from the *active* slot's own currently-running copy (already booted, already known-good) rather than leaving the inactive slot's own prior, possibly-stale content in place -- so `--kernel=` alone updates only the root, paired with a fresh copy of the kernel that's actually running right now, and vice versa. Supplying both explicitly still works exactly as before and is unaffected; this only changes what happens when one is omitted.

See [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md#background-how-ab-kernel-updates-work-here) for the full A/B mechanics this write triggers — same mechanism, root and kernel share one slot and one Automatic Boot Assessment counter.

## Confirming a deploy actually took effect

```sh
cixctl --host=<box> health
cixctl --host=<box> boot
```

```json
{"status":"ok"}
{"build_version":"v1.6.0-9-gc59e482-dirty","build_time":"2026-08-08T00:52:00Z","slot":"b","kernel_version":"6.18.40"}
```

`build_version` is `git describe --tags --always --dirty` **at build time** — check it against the commit you actually intended to ship, not just that the daemon answered. `slot` is the direct answer to "did I actually boot into the slot I just wrote," and `kernel_version` (`uname -r`) confirms the running kernel matches what you just wrote too. A bare `200` from `health` on its own proves only that *some* daemon answered — it doesn't prove it's the one you just deployed, especially right after a reboot where a stale connection or a fallback-to-the-old-slot could both look identical from the outside. Poll through at least one connection failure during the reboot itself (a `curl` timeout or connection-refused) before trusting the recovery — a suspiciously instant reply can mean you never actually lost the old connection.

## When something goes wrong: read the real diagnostics, not just the exit code

`GET /system/logs?source=cixd&tail=N` is the log store (ADR-0070) — every internal `cixd` diagnostic, not just the audit trail, reaches it. For a failed `pkg install`/`pkg hostbuild`, this includes the real captured stdout/stderr of the build container itself (not just its exit status) — a build failure's *actual* error text, e.g.:

```
"pkg cix@__hostbuild: build output: /usr/bin/bash: error while loading
shared libraries: libtinfo.so.6: cannot open shared object file: No such
file or directory"
```

This is genuinely load-bearing: an exit status alone is often ambiguous (exit 127 in particular means either "this project's own container-launch diagnostic ran out of encodable errno range" or "a real command inside the build script genuinely wasn't found" — indistinguishable without the real text). Don't stop at the exit-code decode message in `error`; always check the log store for the accompanying `build output:` entry logged alongside it.

## Debugging a remote build in depth (no shell on the box needed)

Everything below was learned running real, multi-hour toolchain builds against a live box — each item closes a gap where the basic flow above goes quiet exactly when you need detail most.

- **The captured `build output:` in the log store is a 3800-byte TAIL, not the whole log** (`PKG_BUILD_OUTPUT_CAPTURE_MAX`, `daemon/src/pkg.c` — deliberate: the error is usually last). A build whose final phase prints hundreds of lines of noise (gmp's configure is a real offender) pushes the actual error out of the window entirely. The reliable pattern for a long build: redirect inside the recipe itself — `make -j6 all > /build/make.log 2>&1` — and on failure `tail -n 100 /build/make.log`, so the captured window always holds the real failure point. The full log stays retrievable afterward (next bullet).
- **`--keep-on-failure` + the files API is your post-mortem** (ADR-0175): `cixctl pkg install ... --keep-on-failure` preserves the failed build container (named `__pkgbuild-N` — check `cixctl ps`). Any file in it is then one REST call away: `curl "http://<box>/v1/containers/__pkgbuild-N/files?path=/build/make.log"` — including crashed binaries themselves, pulled locally for a real gdb session (this exact flow root-caused a corrupted-jump-instruction GCC codegen bug this project hit). `cixctl pkg resume` can continue a preserved build without redoing fetch+extract. Delete the container (`cixctl container rm __pkgbuild-N`) when done — **it holds the single build slot; a new install can't start while it exists**, and a *hung* (not crashed) build likewise holds the slot forever until you `rm` it.
- **Live streaming**: `cixctl pkg build-log` tails the in-flight build's output live and untruncated (WebSocket under the hood — a plain `curl` on `/v1/pkg/build/log` won't work, use the CLI).
- **Detecting a silent hang vs. a slow build**: `GET /v1/system/stats` load near zero for two consecutive checks *plus* a make.log (pulled via the files API — readable live, not just after failure) whose line count hasn't grown between checks = genuinely hung, not slow. `cixctl container console __pkgbuild-N` gives an interactive shell inside the still-running container to inspect the process tree — note the dev image's own `ps` binary is currently broken (issue #47); walk `/proc/[0-9]*/cmdline` + `stat` field 22 by hand instead to find which process is stuck and for how long.
- **`pkg sync` never re-fetches an already-seen `name@version`, even when the repo content for it has genuinely changed** — the per-version cache is keyed on name+version alone, and the first sync wins permanently (confirmed live several times, including a version whose *first* sync caught placeholder content: every later fix pushed to the same version was silently ignored despite `state=success` syncs). Any content change therefore **requires bumping the version string**, and anything that must be live in the repo at fetch time (see next bullet) must be in place *before* that version's first-ever sync. `cixctl pkg recipe show <name> --version=<v>` shows what the daemon actually holds — trust that, not the repo.
- **Recipes that self-fetch from the private Gitea** (e.g. the kernel recipe pulling its own `.config` via the authenticated raw-content API): put the literal `{{REPO_TOKEN}}` in the `pkg_source` URL where the credential goes (`https://osakka:{{REPO_TOKEN}}@git.home.arpa/...`) and commit it in that final form — the daemon substitutes its own stored repo token (`cixctl pkg repo-config set --token=...`) at fetch time (issue #60). No live-substitution dance, no temp tokens, no revert: the recipe is committable as-is and the token never touches the catalog or any log. (Historical note: before #60, this required committing a `REPLACE_WITH_REAL_TOKEN` placeholder, live-substituting a real scoped token on Gitea's HEAD *before the version's first sync*, then reverting and deleting it after the fetch — the per-version sync cache made every step order-sensitive. `{{REPO_TOKEN}}` replaces all of it.)

## The endpoint, not just the mechanism

None of this is Cix-specific tooling beyond the recipe format itself — every step above is a plain REST call any HTTP client can make (`curl`, a script, `cixctl`, or the web dashboard). See [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) and [`docs/api/README.md`](../api/README.md#host--package-updates) for the full contract, [`docs/api/README.md`](../api/README.md#a-consolidated-log) for the log store's own query parameters, and [`cli-reference.md`](cli-reference.md) for every `cixctl` subcommand used above.

## Going further: no local build at all

Everything above still assumes you cross-compiled `cixd` locally and are pushing the *result*. The [hostbuild mechanism](writing-recipes.md#the-hostbuild-variant) (ADR-0056) removes even that step: `POST /pkg/hostbuild {"name": "cix", "build_image": "<an image with a working toolchain>"}` compiles `cixd`/`cixctl`/`web/` **entirely on the remote box itself**, from its own currently-tracked source (`cix.recipe`'s `pkg_source`), harvesting the result to a real host artifact directory — see [`building-cix.md`](building-cix.md#from-a-running-cix-host-self-hosted-rebuild) for the full self-hosted rebuild walkthrough. Combined with the LAN-serve trick above (used here to get the *source snapshot* onto the box instead of a compiled artifact — a scratch recipe whose `pkg_source` is a `tar czf`'d copy of your own working tree, since a real install typically can't reach a real git server either), this closes the loop completely: develop locally, push source, compile server-side, deploy the result, all over plain REST, with no ISO reinstall anywhere in the cycle.
