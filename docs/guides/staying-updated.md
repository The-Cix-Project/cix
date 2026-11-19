# Staying updated

Keeping an installed system current. [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) covers kernel updates (build, write, reboot, confirm); this guide covers the control plane and packages, and what runs on its own. [`rolling.md`](rolling.md) explains what rolls and how far: source policy, recipes, and what an image takes.

## Two independent things to keep current

The control plane and the software installed into images update through different endpoints, on different schedules.

### The control plane (`cixd`/`cixctl`/`web/`)

There is nothing to run. The control-plane root is the `cix-root` image's current version, and `cixd` and `cixctl` are rolling entries of it ([ADR-0380](../adr/0380-cix-is-followed-in-cix-root-and-a-release-is-a-publish.md)). A new `cix` release reaches the host in three steps:

1. it is published to a recipe source the host syncs;
2. the host builds it once, or takes the artifact from the cache, and `cix-root` moves;
3. a root-only roll assembles a new root and switches into it in place, with every container running ([ADR-0379](../adr/0379-the-host-roll-deploys-the-root-when-cix-root-has-moved.md), [ADR-0375](../adr/0375-the-control-plane-upgrades-in-place.md)).

```
cixctl boot                      # the release running, and the cix-root version its root came from
cixctl schedule show host-roll   # the last roll's line: root: cix-root X -> Y, switched in place ...
```

To take releases only in the nightly window, set the `host-roll` job's params to `{"root":"window"}`; to stop them, disable `host-roll`. A release that fails to deploy, or that you leave with `cixctl boot-next`, is not deployed again by itself until a newer one is published or the roll is run by hand. `cix` is never hostbuilt; `pkg hostbuild cix` is refused. The kernel is the one hostbuild package, and it rolls in the window ([`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md)).

A root written by hand, below, boots at the next reboot; the newly booted daemon confirms the slot itself once it is healthy (see [how A/B updates work](kernel-build-and-ab-updates.md#background-how-ab-kernel-updates-work-here); root and kernel share the slot and its boot counter). To write a squashfs you already have, use `update` directly. An installed host has no shell, so the daemon fetches the image itself:

```
cixctl update --image-url=<URL> --image-sha256=<HEX>
```

For building the control plane on a host, see [`building-cix.md`](building-cix.md#rebuilding-cix-on-a-running-host).

**When the cache is used.** A hostbuild fetches instead of building only when both hold:

- a package repository is configured (`cixctl pkg repository ls`);
- this host **trusts the key that signed the artifact**. These packages need no per-recipe `artifact_sha256`; the signature is the approval ([ADR-0279](../adr/0279-an-artifact-carries-its-own-approval.md)).

Otherwise, and for a version never published to the cache, it builds. A host adopts trusted keys during `cixctl pkg sync`: the daemon copies the `docs/keys/` directory of each synced source marked `trust_keys` into its trusted-key store (`daemon/src/pkg.c`), and a repository with no `docs/keys/` adopts nothing. Recipes are synced from the cix-recipes repository ([ADR-0308](../adr/0308-recipes-are-their-own-repository-flat.md)), which has no `docs/keys/` directory at present. How a host syncing only from it comes to trust the release key is an open question, not covered here.

### Packages (whatever is installed into your images)

```
cixctl pkg drift
cixctl pkg update-all
```

`pkg drift` lists every installed package whose recipe is newer than what is installed. `pkg update-all` starts an upgrade for the first such package, across every image, through the same mechanism as `pkg install --upgrade`. Each call starts one upgrade; call it again once that job finishes, until it reports `nothing to update`. A package upgrade applies live to its image, with no reboot.

## What runs on its own

- **Schedules** ([ADR-0257](../adr/0257-one-scheduler-structured-schedules.md)). `cixctl schedule actions` lists what a schedule can run: fetching recipes (`pkg.sync`), finding, authenticating and authoring new upstream releases (`pkg.discover`, ADR-0323), the platform backup (`system.backup`), volume snapshots (`volume.backup`) and the host roll (`system.roll`, below). `cixctl schedule ls` shows which are scheduled on this host.
- **Rolling images.** Publishing a recipe, by hand or through a sync, queues a rebuild of every image that tracks that package `rolling` (`cixctl pkg rebuilds` lists the queue). Containers created with `--follow-rolling` restart onto the rebuilt image, spread over the jitter window (`cixctl rolling-config show`).
- **The host itself** ([ADR-0327](../adr/0327-the-host-follows-its-rolling-packages-in-a-nightly-window.md)). `system.roll` builds what is newer of the kernel and cix (the two hostbuild packages above, kernel first), and stages root and kernel together on the inactive slot.
  - **Same kernel:** it switches the root in place, and every container keeps running ([ADR-0375](../adr/0375-the-control-plane-upgrades-in-place.md)).
  - **New kernel, refused switch, or work still in flight at the window's end:** it reboots into the slot inside the schedule's window.

  The boot entry falls back if the new slot does not confirm. A fresh host runs it as `host-roll`, nightly at 03:00 for 3h; `cixctl schedule ls` shows the last night's outcome, and `cixctl logs` each step. Edit the schedule to move the window, or delete it to update and reboot by hand as above.

Nothing runs `pkg update-all` on its own. Run it yourself, or from any REST client on whatever cadence you choose.

## Before you update: back up

```
cixctl backup --output=backup.json
```

The bundle holds container definitions, networks, DNS records, volume definitions (not their data), package install state, the recipes no source owns (a restore syncs the rest, ADR-0351), and site config. See [Backup and restore](../api/README.md#backup-and-restore) for exactly what it contains and excludes. To take it on a clock, schedule the `system.backup` action; `cixctl backup-config` sets which disk it is written to.
