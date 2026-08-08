# Developing against a real remote box, with no SSH

How to push local (or even entirely server-compiled) changes onto a real, already-installed Kanxeo host and verify they actually took effect — without SSH, without a general shell, and without reinstalling from an ISO each time. This is the operational counterpart to [`building-kanxeo.md`](building-kanxeo.md) (which covers compiling) and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)/[`staying-updated.md`](staying-updated.md) (which cover the update endpoints themselves) — this page is the "how do I actually get a file onto the box in the first place, and prove it landed" walkthrough neither of those needed to be, because a real Kanxeo install genuinely has no other way in.

## Why this needs a trick at all

A real installed Kanxeo host has no SSH server and no general shell (ADR-0034, confirmed directly — `nc -z <box> 22` closed on a fresh install). `POST /system/update`'s `image_path`/`kernel_path` fields, and `kanxeoctl update --image=PATH`, are **paths the daemon reads from its own local disk** — not an upload endpoint. `kanxeoctl`'s `update` subcommand just forwards whatever string you give it straight into the JSON body (`cli/src/main.c`'s `cmd_update()`); it never reads the file itself. So a locally-built artifact needs a real way to land on the *remote* daemon's own filesystem before `system/update` has anything to point at.

## The core trick: reuse the package-fetch pipeline as a generic file-transfer primitive

`pkg install`'s own fetch step already does exactly what's needed — the daemon `curl`s a URL host-side, verifies it against a `pkg_sha256`, and lands the raw download at a real, predictable local path (`<data-dir>/pkg/sources/<name>-<version>-0.src`) before ever trying to build anything. A throwaway scratch recipe pointed at an arbitrary file, served from your own dev machine, turns this into a general "get this file onto the remote box's disk" mechanism — the eventual build/install step is expected to fail (the file usually isn't a real tarball), but by then the only part that matters, the fetch + checksum, has already succeeded.

1. **Serve the artifact over your LAN**, from wherever you built it:
   ```sh
   cd /path/to/artifacts && python3 -m http.server 8904 --bind <your-lan-ip>
   ```
2. **Compute its real checksum** and write a scratch recipe:
   ```sh
   sha256sum kanxeod-root.squashfs
   ```
   ```
   pkg_name="scratch-deploy"
   pkg_version="1"
   pkg_source="http://<your-lan-ip>:8904/kanxeod-root.squashfs"
   pkg_sha256="<the real sha256 above>"
   pkg_depends=""

   pkg_build() { true; }
   pkg_install() { true; }
   ```
3. **Push the recipe and trigger a fetch** (see [`writing-recipes.md`](writing-recipes.md) for the full recipe format):
   ```sh
   kanxeoctl --server=http://<box>:7620 recipe add scratch-deploy.recipe
   kanxeoctl --server=http://<box>:7620 pkg install scratch-deploy
   ```
   Poll `GET /pkg/scratch-deploy` until it leaves `fetching`/`building`. `state: "failed"` with `"could not prepare the build container (extract source tarball failed)"` is the **expected, harmless** outcome for a non-tarball artifact — it means the fetch and checksum verification already succeeded, which is all this step is for.
4. **Read back the real local path**: `<data-dir>/pkg/sources/scratch-deploy-1-0.src` (the default data dir is `/var/lib/kanxeo`).
5. **Clean up** once you're done: `kanxeoctl recipe rm scratch-deploy` (a package left in `PKG_STATE_FAILED` can't be `DELETE`d via the package endpoint — that's expected, not a bug; the recipe delete is what matters).

## Writing it to the inactive slot, and a real gap to know about

```sh
kanxeoctl --server=http://<box>:7620 update \
  --image=/var/lib/kanxeo/pkg/sources/scratch-deploy-1-0.src \
  --kernel=/var/lib/kanxeo/pkg/sources/<kernel-scratch-path>
kanxeoctl --server=http://<box>:7620 reboot
```

**Always supply `--image=`/`--kernel=` together, even when only one actually changed.** `POST /system/update` updates only whichever of the two files is actually given — a one-sided update can leave the *other* file in that slot stale from an earlier cycle, a real gap discovered the hard way: a root-only update landed in a slot whose kernel predated a since-fixed config, and the very next boot regressed to a bug that had already been fixed elsewhere. If the kernel genuinely hasn't changed, resupply the same known-good kernel path anyway (fetch it once via the trick above, reuse that same on-disk path across updates) rather than omitting `--kernel=`.

See [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md#background-how-ab-kernel-updates-work-here) for the full A/B mechanics this write triggers — same mechanism, root and kernel share one slot and one Automatic Boot Assessment counter.

## Confirming a deploy actually took effect

```sh
kanxeoctl --server=http://<box>:7620 health
```

```json
{"status":"ok","build_version":"v1.6.0-9-gc59e482-dirty","build_time":"2026-08-08T00:52:00Z","slot":"b"}
```

`build_version` is `git describe --tags --always --dirty` **at build time** — check it against the commit you actually intended to ship, not just that the daemon answered. `slot` is the direct answer to "did I actually boot into the slot I just wrote." A bare `200` on its own proves only that *some* daemon answered — it doesn't prove it's the one you just deployed, especially right after a reboot where a stale connection or a fallback-to-the-old-slot could both look identical from the outside. Poll through at least one connection failure during the reboot itself (a `curl` timeout or connection-refused) before trusting the recovery — a suspiciously instant reply can mean you never actually lost the old connection.

## When something goes wrong: read the real diagnostics, not just the exit code

`GET /system/logs?source=kanxeod&tail=N` is the log store (ADR-0070) — every internal `kanxeod` diagnostic, not just the audit trail, reaches it. For a failed `pkg install`/`pkg hostbuild`, this includes the real captured stdout/stderr of the build container itself (not just its exit status) — a build failure's *actual* error text, e.g.:

```
"pkg kanxeo@__hostbuild: build output: /usr/bin/bash: error while loading
shared libraries: libtinfo.so.6: cannot open shared object file: No such
file or directory"
```

This is genuinely load-bearing: an exit status alone is often ambiguous (exit 127 in particular means either "this project's own container-launch diagnostic ran out of encodable errno range" or "a real command inside the build script genuinely wasn't found" — indistinguishable without the real text). Don't stop at the exit-code decode message in `error`; always check the log store for the accompanying `build output:` entry logged alongside it.

## The endpoint, not just the mechanism

None of this is Kanxeo-specific tooling beyond the recipe format itself — every step above is a plain REST call any HTTP client can make (`curl`, a script, `kanxeoctl`, or the web dashboard). See [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) and [`docs/api/README.md`](../api/README.md#host--package-updates) for the full contract, [`docs/api/README.md`](../api/README.md#a-consolidated-log) for the log store's own query parameters, and [`cli-reference.md`](cli-reference.md) for every `kanxeoctl` subcommand used above.

## Going further: no local build at all

Everything above still assumes you cross-compiled `kanxeod` locally and are pushing the *result*. The [hostbuild mechanism](writing-recipes.md#the-hostbuild-variant) (ADR-0056) removes even that step: `POST /pkg/hostbuild {"name": "kanxeo", "build_image": "<an image with a working toolchain>"}` compiles `kanxeod`/`kanxeoctl`/`web/` **entirely on the remote box itself**, from its own currently-tracked source (`kanxeo.recipe`'s `pkg_source`), harvesting the result to a real host artifact directory — see [`building-kanxeo.md`](building-kanxeo.md#from-a-running-kanxeo-host-self-hosted-rebuild) for the full self-hosted rebuild walkthrough. Combined with the LAN-serve trick above (used here to get the *source snapshot* onto the box instead of a compiled artifact — a scratch recipe whose `pkg_source` is a `tar czf`'d copy of your own working tree, since a real install typically can't reach a real git server either), this closes the loop completely: develop locally, push source, compile server-side, deploy the result, all over plain REST, with no ISO reinstall anywhere in the cycle.
