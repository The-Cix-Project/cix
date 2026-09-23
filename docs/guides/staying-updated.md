# Staying updated

Keeping an installed system current. [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) covers kernel updates (build, write, reboot, confirm); this guide covers the control plane and packages, and what runs on its own.

## Two independent things to keep current

The control plane and the software installed into images update through different endpoints, on different schedules.

### The control plane (`cixd`/`cixctl`/`web/`)

```
cixctl pkg hostbuild cix --upgrade --deploy
cixctl reboot
```

`cix` is a hostbuild package. `pkg hostbuild cix --upgrade` fetches the signed artifact from the artifact cache when one is published there, and builds only when it is not ([ADR-0289](../adr/0289-a-hostbuild-package-is-consumed-from-the-cache-like-any-other.md)). `kernel` and `isotools` work the same way. `--deploy` waits for the result and writes it to the inactive A/B slot with `update`. It does not reboot.

Writing and booting are separate steps, as for a kernel update, and the newly booted daemon confirms the slot itself once it is healthy (see [how A/B updates work](kernel-build-and-ab-updates.md#background-how-ab-kernel-updates-work-here); root and kernel share the slot and its boot counter). To write a squashfs you already have, use `update` directly. An installed host has no shell, so the daemon fetches the image itself:

```
cixctl update --image-url=<URL> --image-sha256=<HEX>
```

For building the control plane on a host, see [`building-cix.md`](building-cix.md#rebuilding-cix-on-a-running-host).

**When the cache is used.** A hostbuild fetches instead of building only when both hold:

- the artifact server is configured (`cixctl pkg artifact-config show`);
- this host **trusts the key that signed the artifact**. These packages carry no per-recipe `pkg_artifact_sha256`; the signature is the approval ([ADR-0279](../adr/0279-an-artifact-carries-its-own-approval.md)).

Otherwise, and for a version never published to the cache, it builds. A host adopts trusted keys during `cixctl pkg sync`: the daemon copies the `docs/keys/` directory of the synced repository into its trusted-key store (`daemon/src/pkg.c`), and a repository with no `docs/keys/` adopts nothing. Recipes are synced from the cix-recipes repository ([ADR-0308](../adr/0308-recipes-are-their-own-repository-flat.md)), which has no `docs/keys/` directory at present. How a host syncing only from it comes to trust the release key is an open question, not covered here.

### Packages (whatever is installed into your images)

```
cixctl pkg drift
cixctl pkg update-all
```

`pkg drift` lists every installed package whose recipe is newer than what is installed. `pkg update-all` starts an upgrade for the first such package, across every image, through the same mechanism as `pkg install --upgrade`. Each call starts one upgrade; call it again once that job finishes, until it reports `nothing to update`. A package upgrade applies live to its image, with no reboot.

## What runs on its own

- **Schedules** ([ADR-0257](../adr/0257-one-scheduler-structured-schedules.md)). `cixctl schedule actions` lists what a schedule can run: fetching recipes (`pkg.sync`), refreshing upstream release data (`pkg.refresh-upstreams`), the platform backup (`system.backup`) and volume snapshots (`volume.backup`). `cixctl schedule ls` shows which are scheduled on this host.
- **Rolling images.** Publishing a recipe, by hand or through a sync, queues a rebuild of every image that tracks that package `rolling` (`cixctl pkg rebuilds` lists the queue). Containers created with `--follow-rolling` restart onto the rebuilt image, spread over the jitter window (`cixctl rolling-config show`).

Nothing updates the control plane or runs `pkg update-all` on its own, and nothing reboots the host. Run those yourself, or from any REST client on whatever cadence you choose.

## Before you update: back up

```
cixctl backup --output=backup.json
```

The bundle holds container definitions, networks, DNS records, volume definitions (not their data), package install state and recipes, and site config. See [Backup and restore](../api/README.md#backup-and-restore) for exactly what it contains and excludes. To take it on a clock, schedule the `system.backup` action; `cixctl backup-config` sets which disk it is written to.
