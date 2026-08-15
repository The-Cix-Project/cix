# 0088 — CONFIG_VETH was never enabled: every container network attachment on a real box has always failed

## Status

Accepted

## Context

Found live on 192.168.15.95 while building the first real DNS containers this project has attempted (a direct user request: two `dnsmasq` containers on the `management` network). `POST /v1/containers` with any `networks` attachment failed outright with a bare `"Operation not supported"` (500) — reproduced identically for the `management` network (which has a real physical uplink) and for `chicken` (a pure isolated bridge, no uplink), ruling out anything network-specific, IP-specific, or upstream-connectivity-specific as the cause.

Reading `src/container_net.c` directly: `rtnl_veth_create()` is the *only* mechanism any container has ever gotten a network interface through — every `networks` attachment, on every image, on every phase of this project. It issues a plain `RTM_NEWLINK` for a `veth` pair via rtnetlink. `ENOTSUP`/`EOPNOTSUPP` on that specific call is the kernel's own answer when it has no driver registered for the requested link kind.

`git log --all --oneline -p -- image/kernel/qemu-part1.config | grep -B5 -i veth` returns zero hits, at any point in this file's history. `CONFIG_VETH` has never been enabled by this project's own tracked kernel config fragment — not disabled by a later change, never present at all. `CONFIG_BRIDGE=y` *is* enabled (and has been since early in the project), but a bridge on its own has nothing to attach: it needs either a real physical NIC or a veth pair enslaved to it, and this kernel has only ever been able to provide the former.

This had never been caught because every prior "container networking works" test in this project's history — going back through VRRP/OSPF router pairs (Phase 24), the console/exec WebSocket work, disk/PKI/DNS testing, all of it — ran directly on this dev sandbox's own host kernel, which has `CONFIG_VETH` natively (as any ordinary distro kernel does), never on a kernel actually built from `image/kernel/qemu-part1.config` and then asked to attach a container to a network for real. This may be the first time in the project's history that a real multi-container network attachment has been attempted on a kernel built from this project's own tracked config, on real hardware.

This is the same failure shape as the `CONFIG_OVERLAY_FS` gap (ADR fixed earlier this project) and the `CONFIG_SWAP`/`CONFIG_SMP` gaps (ADR-0082): a kernel-config symbol this dev sandbox's own rich host kernel always provided for free, silently masking its total absence from the one config fragment that actually matters for a real install.

## Decision

Add `CONFIG_VETH=y` to `image/kernel/qemu-part1.config`, immediately after the existing `CONFIG_BRIDGE=y`/`CONFIG_VLAN_8021Q=y` block, with a comment documenting the finding so a future reader doesn't have to re-derive it. Rebuild the kernel from a clean `make allnoconfig` forward (this project's own standard procedure — a cached, already-`allnoconfig`'d source tree from the prior CONFIG_SMP work was reused, saving that step), then deploy via the established local-build-plus-scratch-recipe-LAN-serve mechanism (`docs/guides/remote-development.md`) rather than a full ISO reinstall.

## Consequences

- Every container network attachment on a real thinC install has been broken since this project's very first kernel build — not a regression, a gap that was always there and never exercised end-to-end until now.
- Fixed and live-verified on 192.168.15.95: a real container (`vethtest`, image `base`, attached to `management`) was created successfully post-deploy, receiving a real veth-backed interface and its assigned IP (`192.168.15.1`), where the identical request previously failed with `"Operation not supported"`.
- Deployed via a kernel-only update (`thincctl update --image=... --kernel=...`, both supplied together per the documented one-sided-update footgun, task #671) — the control-plane root was rebuilt locally via `build/mkbootroot` to match what was already running (no `thincd` code changes bundled into this deploy), landing on the box's inactive slot A and confirmed booted (`slot: "a"` in `GET /system/boot`) before testing.
- No test in the existing automated suite exercises a real kernel built from `qemu-part1.config` attaching a container to a network — `test_container_net.c` and friends all run against this dev sandbox's own host kernel, so they could not have caught this and still can't catch a future regression of the same kind. Not fixed in this pass (would need a real from-source kernel build wired into CI, out of scope for a single-config-symbol fix); worth remembering as a category of gap this project has now hit three times (`CONFIG_OVERLAY_FS`, `CONFIG_SWAP`/`CONFIG_SMP`, now `CONFIG_VETH`).
