# 0089 — root-netns net.ipv4.ip_forward was never enabled: no container could ever originate a connection past the host

## Status

Accepted

## Context

Found immediately after ADR-0088's CONFIG_VETH fix unblocked container network attachment at all: `dns-1`/`dns-2` (real `dnsmasq` containers, `--server=1.1.1.1 --server=8.8.8.8` upstream forwarders) resolved `.internal`-style records fine but timed out recursing any real public domain.

Isolated with controlled, before/after tests against a throwaway container (`cix-hosttools`, which has a real `curl`) on the same `management` network:
- Container -> the daemon's own management address (same bridge, directly addressed): succeeded.
- Container -> the real upstream LAN gateway one hop further (192.168.15.254): failed, and (once retested against a port that's actually listening, avoiding a red herring from an unrelated "nothing on port 80" false lead) DNS recursion itself was the clean, decisive before/after signal -- failed pre-fix, succeeded post-fix on the identical path.

Root cause: `struct container_spec.ip_forward` / `container_net_enable_ip_forward()` (`src/container_net.c`) only ever writes `/proc/sys/net/ipv4/ip_forward` **inside a container's own netns**, entered via `--ip-forward` -- a mechanism built for a container acting as its own router between two networks it's attached to (Phase 24's BIRD/keepalived pairs). Nothing anywhere in this codebase, including `image/src/cix-install.c`, has ever written that same file in the **root netns** -- confirmed by a full grep across every `.c` file that touches `sysctl`. Per ADR-0067's own reasoning, a network's own "address" is deliberately the target its containers route through as a default gateway -- but the host's root netns was never actually enabled to *act* as that gateway. A container's own default-route packet reaches the host's IP stack over the bridge and is silently dropped there, since forwarding transit traffic between interfaces requires this sysctl regardless of how the packet arrived.

Like ADR-0088, this had never been caught because no container before `dns-1`/`dns-2` had ever needed to originate a connection leaving the host at all -- every prior "container needs the internet" case (package installs) was always a `cixd`-side `curl`, never container-initiated.

## Decision

Enable `net.ipv4.ip_forward=1` in the daemon's own root netns once, at startup, immediately after `network_init()` succeeds (`daemon/src/main.c`) -- unconditionally, not gated behind `--init-mode`, since a dev/test daemon can attach containers to networks too and the same routing model applies. Implemented as a plain inline `open()`/`write()`/`close()` on `/proc/sys/net/ipv4/ip_forward`, matching `container_net_enable_ip_forward()`'s own shape but deliberately not sharing that function directly -- it lives in `include/internal.h`, the container-child-process-only header, and pulling it into `main.c` (which only includes the public `container.h`) would blur that layering for a ten-line primitive. Failure is fatal (daemon startup aborts), matching this codebase's existing fail-fast posture for `network_init()`/`bootstrap_management_network()` -- a host that can't enable forwarding can't deliver on the routing model ADR-0067 already committed to.

## Consequences

- Every ordinary (non-router) container's outbound-to-anything-beyond-the-host-and-its-bridge-peers traffic now actually works, not just traffic to the host itself or other containers on the same bridge.
- Live-verified: `dns-1`/`dns-2`'s real recursive resolution against `1.1.1.1`/`8.8.8.8` succeeds post-fix, reproducibly, where it timed out identically pre-fix on the same box, same containers, same upstream forwarders.
- Deployed in the same build as the already-committed pipe-deadlock fix (ADR-0087) and the not-yet-committed container-create use-after-free fix (see the accompanying commit) -- all three landed together on 192.168.15.95's inactive slot via the established root-only redeploy (kernel unchanged from ADR-0088's own deploy).
- No automated test exercises this path (same category of gap ADR-0088 already flagged for kernel-config coverage) -- a future regression here would only surface the same way this one did, via a real container making a real outbound connection on a real box.
