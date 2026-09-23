# Developing against a real remote box, with no SSH

How to get a change onto a real, installed Cix host, prove it took effect, and debug a build there — without SSH, without a general shell, and without reinstalling from an ISO. This is the operational counterpart to [`building-cix.md`](building-cix.md) (which covers the build itself) and [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)/[`staying-updated.md`](staying-updated.md) (which cover the update endpoints).

## Why everything goes through the API

An installed Cix host has no SSH server and no general shell ([ADR-0034](../adr/0034-console-login-via-supervised-cixctl.md)). Every step below is a REST call, made here with `cixctl --host=<box>`.

`POST /system/update`'s `image_path`/`kernel_path` fields, and `cixctl update --image=PATH`/`--kernel=PATH`, are **paths the daemon reads from its own disk** — not uploads. `cixctl update` forwards the string into the JSON body (`cmd_update()` in `cli/src/main.c`) and never reads the file itself.

## Getting a new build onto the box

The normal route builds on the box itself: tag this repository, publish a `cix` recipe for the tag, and run

```sh
cixctl --host=<box> pkg hostbuild cix --upgrade --wait --deploy
```

which compiles, assembles the control-plane root, and writes it to the inactive slot — see [`building-cix.md`](building-cix.md#rebuilding-cix-on-a-running-host). A kernel is the same shape: `pkg hostbuild kernel`, then `update --kernel=` with the path the hostbuild reports ([`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)). In both cases the file is already on the box's own disk.

A control-plane image that is **not** on the box's disk — one produced by another Cix host, served over HTTP — is fetched by the daemon itself:

```sh
cixctl --host=<box> update --image-url=http://<server>/cixd-root.squashfs --image-sha256=<64 hex>
cixctl --host=<box> reboot
```

`--image-sha256=` is mandatory, and the image is verified before it is written to a boot slot. The daemon resolves the URL itself, so on a host with no working resolver only a literal IP works ([`docs/api/README.md`](../api/README.md#host--package-updates)). There is no URL form for the kernel; `--kernel=` takes a path on the box.

`--image=` and `--kernel=` can be supplied independently: whichever is omitted is filled from the *active* slot's running copy ([ADR-0095](../adr/0095-update-one-sided-footgun.md)). See [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md#background-how-ab-kernel-updates-work-here) for the A/B mechanics this write triggers.

## Confirming a deploy actually took effect

```sh
cixctl --host=<box> health
cixctl --host=<box> boot
```

`boot` reports `build_version`, `build_time`, `slot` and `kernel_version` (plus the platform device list). `build_version` is `git describe --tags --always --dirty` **at build time** — check it against the commit you intended to ship, not just that the daemon answered. `slot` answers "did I boot into the slot I just wrote", and `kernel_version` (`uname -r`) confirms the running kernel. A bare `200` from `health` proves only that *some* daemon answered. Poll through at least one connection failure during the reboot itself before trusting the recovery — a suspiciously instant reply can mean you never lost the old connection.

### A deploy is not verified until the new daemon has built something

`health` and `boot` prove the daemon you meant is running and answering. They do not prove it can still build. A release that changes the build path is itself built by the daemon it replaces, so the first real exercise of the new build code is always the build *after* the deploy. Finish every deploy by building a real package, from the `probe-selftest-one@1.sh` recipe in cix-recipes:

```sh
cixctl --host=<box> pkg recipe add --name=probe-selftest-one --file=probe-selftest-one@1.sh
cixctl --host=<box> pkg install --name=probe-selftest-one
cixctl --host=<box> pkg build-logs --last
```

(The `recipe add` answers `409` if that version is already published on the box, which is fine.) The build fails on purpose — the log is the product — and the line to read is `BUILD-PATH RESULT`. Reaching it means the daemon composed a build environment, created a build container and ran something in it.

If the box is already on a daemon that cannot build, the recovery is API-only and does not need console access: install a previous `cix` version whose artifact is already in the cache (a cache hit does not need a working build), let the assembly run, `update --image=` the assembled root and reboot — or, if the previous slot still holds a good root, arm it for one boot with `cixctl --host=<box> boot-next <slot>` (`a` or `b`). Delete any containers whose declaration the older daemon predates first: a daemon that cannot load its own persisted state is the one failure the API cannot recover from.

## When something goes wrong: read the real diagnostics, not just the exit code

`cixctl --host=<box> logs --source=cixd --tail=N` (`GET /system/logs`, [ADR-0070](../adr/0070-consolidated-log-store.md)) is the log store — every internal `cixd` diagnostic reaches it. For a failed `pkg install`/`pkg hostbuild` it includes the build container's own captured output as a `build output:` entry, not just its exit status:

```
"pkg cix@__hostbuild: build output: /usr/bin/bash: error while loading
shared libraries: libtinfo.so.6: cannot open shared object file: No such
file or directory"
```

An exit status alone is often ambiguous (exit 127 means either a container-launch diagnostic or a command inside the build script not found), so always read the `build output:` entry beside the decoded error.

## Debugging a remote build in depth (no shell on the box needed)

- **The `build output:` entry in the log store is a 3800-byte TAIL** (`PKG_BUILD_OUTPUT_CAPTURE_MAX`, `daemon/src/pkg.c`), because the error is usually last. The **whole** log of every build is retained: `cixctl pkg build-logs` lists them with sizes, `--last` prints the most recent and `--file=NAME` prints one. The size column is itself a diagnostic — a log of a few dozen bytes means the build phase never started. `cixctl pkg build-log [--name=NAME]` (singular) is live-only: it tails the in-flight build over a WebSocket (`GET /v1/pkg/build/log` — a plain `curl` will not work) and answers `404` once the build has ended.
- **`--keep-on-failure` + the files API is your post-mortem** ([ADR-0175](../adr/0175-pkg-build-keep-on-failure.md)): `cixctl pkg install ... --keep-on-failure` preserves the failed build container (named `__pkgbuild-N`; `cixctl container ls --all` shows it). Any file in it is then one REST call away — `cixctl container files get __pkgbuild-N --path=/build/src/config.log`, including a crashed binary itself, pulled locally for a debugger. `cixctl pkg resume` continues a preserved build without redoing fetch+extract ([ADR-0177](../adr/0177-pkg-build-resume.md)). Delete the container (`cixctl container rm __pkgbuild-N`) when done; each concurrent build runs in its own `__pkgbuild-N` container. How many builds run at once is `cixctl pkg-build-config show` (`max_concurrent_jobs`, 1-10); only one hostbuild runs at a time, whatever it builds.
- **Detecting a silent hang vs. a slow build**: `GET /v1/pkg/{name}` reports a running build's `last_output_seconds_ago` and its `build_container` ([`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs)). A long silence is not by itself a hang — a large link or gcc's bootstrap goes quiet for minutes — so combine it with `cixctl host-stats` (`GET /v1/system/stats`) showing load near zero across two checks. `cixctl process ls` lists every process on the box correlated to its container, which finds the stuck one. `cixctl pkg cancel --name=NAME` stops a build that will never finish and releases its slot (`POST /pkg/cancel`).
- **`pkg sync` never re-fetches an already-seen `name@version`**, even when the repository's content for it has changed: a sync only adds versions the host does not have ([ADR-0121](../adr/0121-pkg-redesign-part2-configurable-repo-and-sync.md), [ADR-0107](../adr/0107-package-image-versioning.md)). Any content change therefore **requires a new version**. `cixctl pkg recipe show <name> --version=<v>` shows what the daemon holds — trust that, not the repository.
- **Recipes that fetch from the private Gitea** put the literal `{{REPO_TOKEN}}` in the source URL where the credential goes (`https://osakka:{{REPO_TOKEN}}@git.home.arpa/...`); the daemon substitutes its stored repo token (`cixctl pkg repo-config set --token=...`) at fetch time (issue #60). The recipe is committable as-is and the token never reaches the catalog or a log.

## The endpoint, not just the mechanism

Every step above is a plain REST call any HTTP client can make (`curl`, a script, `cixctl`, or the web dashboard). See [`docs/api/README.md`](../api/README.md#package-manager-source-based-asynchronous-installs) and [`docs/api/README.md`](../api/README.md#host--package-updates) for the full contract, [`docs/api/README.md`](../api/README.md#a-consolidated-log) for the log store's query parameters, and [`cli-reference.md`](cli-reference.md) for every `cixctl` subcommand used above.
