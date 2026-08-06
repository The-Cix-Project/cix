# Staying updated

Keeping an already-installed system current, day to day. This is the operational counterpart to [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) — that guide is about kernel updates specifically (build → write → reboot → confirm); this one covers the root control-plane and packages, and how the two update mechanisms relate.

## Two independent things to keep current

Kanxeo separates "the control plane itself" from "the software installed into images" — they update through two different endpoints, on two different schedules, because they're genuinely different kinds of change.

### The control plane (`kanxeod`/`kanxeoctl`/`web/`)

```
kanxeoctl update --image=<path-to-kanxeod-root.squashfs>
kanxeoctl reboot
```

Writes a fresh control-plane squashfs to the inactive A/B slot; same write-then-reboot-separately shape as a kernel update, same automatic self-confirmation once the newly-booted daemon reaches a healthy state (see [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md#background-how-ab-kernel-updates-work-here) for exactly how that works — it applies identically here, root and kernel share the same A/B slot and the same Automatic Boot Assessment counter). The squashfs itself comes from either a dev-machine build or a [self-hosted rebuild](building-kanxeo.md#from-a-running-kanxeo-host-self-hosted-rebuild) — see [`building-kanxeo.md`](building-kanxeo.md).

### Packages (whatever's installed into your images)

```
kanxeoctl pkg update-all
```

Finds the first installed package (across every image) whose recipe's `pkg_version=` has drifted from what's actually installed, and starts an upgrade for it — reusing the exact same install mechanism as any fresh `pkg install`, just with `upgrade: true` implied. Starts **at most one job at a time** (the same v1 single-install-in-flight constraint every install path shares) — call it again once that job finishes to pick up the next drifted package, repeating until it reports `{"status": "nothing to update"}`. This takes effect immediately, live, with no reboot — a package upgrade merges straight into its target image's rootfs the same way any install does.

`GET /pkg/{name}` shows `available_version` for any installed package whose recipe has since changed, if you want to check what's drifted before triggering anything.

## What triggers these

**Nothing does, automatically.** Both are on-demand, operator- or cron-invoked — there is no background updater, no scheduled check, and no automatic "update then reboot" chaining anywhere in this platform (a deliberate v1 scope boundary, not an oversight — see [`docs/api/README.md`](../api/README.md#current-scope-boundaries-v1-deliberate--see-adr-0007)). If you want periodic updates, that's a cron entry (or a container with network reachability to `kanxeod`) calling `kanxeoctl pkg update-all` and/or fetching+writing a fresh control-plane squashfs on whatever cadence you choose — the same "a plain REST client, nothing special" posture `kanxeoctl backup` already documents for scheduled backups.

## Before you update: back up

```
kanxeoctl backup --output=backup.json
```

Bundles container definitions, networks, DNS records, package install state + recipes, and site config — cheap insurance before any update that might go sideways. See [`docs/api/README.md`](../api/README.md#backup-and-restore) for exactly what's in (and deliberately not in) this bundle, and its own real disaster-recovery sequence.
