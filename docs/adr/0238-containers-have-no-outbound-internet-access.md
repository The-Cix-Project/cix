# 0238 — Containers have no outbound internet access, deliberately

## Status

Accepted

## Context

Found by measurement while looking for somewhere on a Cix host to run `go mod vendor` for glauth's prepared source (#262, #263). Two separate facts, easily conflated:

- A **build** container has no network at all. Measured from inside one (`recipes/package/probe-tls`): `curl` exit 7 (could not connect) to the artifact cache's own literal LAN address, exit 6 (could not resolve) for any hostname. This is long-standing and deliberate — a recipe's shell code runs there, and `daemon/include/pkg.h`'s own header comment and ADR-0007 both state it. Every source a build needs is fetched host-side, by the daemon, before the container starts.
- A **run** container reaches the LAN and stops there. Measured from a container on `management` at 192.168.15.103: `http://192.168.15.31:8080/` returns 200, `http://93.184.215.14/` gives `curl` exit 7, and `/proc/net/route` shows a default route via 192.168.15.95 — the host itself. The host's own default route is 192.168.15.254. Grepping `netplane/src`, `daemon/src` and `src` for `masquerade`/`MASQUERADE`/`SNAT` returns nothing: there is no NAT anywhere in the data plane.

So container packets reach the host and are dropped. No container has ever needed otherwise: every one that exists is a LAN-facing service (`dns-1`/`dns-2`, `ldap-1`/`ldap-2`, `ntp-1`/`ntp-2`, `syslog-1`/`syslog-2`, `jump`), and each is *reached* rather than reaching out.

This had never been written down. #262 assumed the opposite — that a prepared source could now be assembled "on a Cix host, in a container with a network attachment" — and that assumption cost a session's work before it was measured. An undocumented absence reads as an oversight, and the next person to need it will either rediscover this the same way or build NAT without anyone deciding that NAT should exist.

## Decision

**Containers do not get outbound internet access, and this is a decision rather than a gap.**

The reasons are specific to what this platform is, not general caution:

1. **The workloads do not need it.** Every service this platform runs is something the LAN connects *to*. A capability nothing uses is still a capability that has to be designed, secured, documented and maintained.
2. **The container network is the real LAN, not a private one.** `management` is 192.168.15.0/24 with real addresses on the real segment; containers are bridged onto the same L2 the rest of the site is on. Egress here is not a private range needing translation to exist at all — it is a routing and policy question on someone's actual network, and answering it silently inside this project would be answering it on the operator's behalf.
3. **The data plane is ours to keep small.** Everything here is hand-written C talking to the kernel through rtnetlink. NAT is connection tracking, not a routing table entry: state per flow, timeouts, helpers, and a new class of failure on a shell-less box. That is a real subsystem, and it should be built when something needs it, with its own ADR, rather than arriving as a side effect of one build problem.
4. **Source preparation has a home already.** The case that raised this is solved differently and better: a prepared source is assembled once and published to cix-cache, where its bytes are checksum-gated and every host can reach it ([#262](https://git.home.arpa/itdlabs/cix/issues/262), `recipes/README.md`). Preparation is not building — it downloads and arranges source and compiles nothing — so the Build Provenance Mandate is untouched: the build still runs on a Cix host with Cix's own toolchain.

**What stays possible without any change.** A container reaches everything on its own LAN segment today, which covers the artifact cache, the git forge, this platform's own DNS and NTP, and any mirror an operator chooses to run on-site. An operator who genuinely wants a specific container to reach the internet can already route it there themselves with the existing `--route` flag on `POST /v1/containers` — the container is on the real L2, so this needs no new mechanism, and it is their network policy to set rather than this platform's default.

**This is reversible and cheaply so.** If a real workload needs egress, that is the moment to design it — as an explicit, per-container opt-in with a stated policy, not a default. Nothing here forecloses that; it records that today's absence is chosen.

## Verification

Both measurements above are reproducible in-tree rather than described:

- `recipes/package/probe-tls` builds in a real build container and reports the CA bundle's presence alongside four connectivity results (LAN plain HTTP, internet by literal IP, hostname resolution, and TLS both with the system trust store and an explicit `--cacert`). It exits non-zero on purpose: a build container's stdout only reaches the log store when the build fails, and `pkg build-log` is live-only, so a probe that succeeds is a probe whose measurements nobody can read. Its first revision proved exactly that by discarding its own answers.
- The run-container measurement was taken through `POST /v1/containers/{name}/exec` against a container on `management`, reading `/proc/net/route` directly rather than inferring the gateway.

## Consequences

- **Anything needing an upstream fetch is prepared outside the platform and published to cix-cache.** That is now a written convention (`recipes/README.md`, `docs/guides/writing-recipes.md`) with `glauth` as the worked example, rather than a thing each person rediscovers.
- **`docs/guides/writing-recipes.md` already said a build container has no network.** What was missing was the run-container half, which is where the wrong assumption formed. Both are stated now.
- A future NAT or egress-policy subsystem supersedes this ADR rather than contradicting it — the decision recorded here is that the absence is deliberate, not that egress must never exist.
