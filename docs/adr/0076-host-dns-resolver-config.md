# 0076 — Host DNS resolver config (GET/PUT /v1/system/resolv) + real bind-mount fix

## Status

Accepted

## Context

Confirmed directly, earlier this project: a real installed Cix host has no outbound DNS resolution mechanism at all (`cixd`'s own `curl` subprocess fails immediately with `CURLE_COULDNT_RESOLVE_HOST` against any real hostname). Worked around, repeatedly, with an ad-hoc LAN-mirror trick (`docs/guides/remote-development.md`) — real, but a per-session hack, not a fix.

The user pushed back on an earlier draft of this ADR that proposed bind-mounting a file managed entirely inside `main.c`'s own boot path with no persisted, API-visible state: "we already write stuff to disk... where are they all being written to?" — correctly pointing out that every other piece of host config (`daemon_config.json`, `site_config.json`, PKI keys, disk roles) already has a real, `g_base_dir`-rooted, REST-visible home, and this should too, not a special case.

Two more decisions came out of the same discussion:
- **Don't try to patch `curl` itself** to read a custom resolver path — checked directly (`ldd`/`curl --version`), this project's own staged `curl` isn't built with `c-ares` (no `--dns-servers` support), and the alternative (glibc's own resolver) has `/etc/resolv.conf` hardcoded at compile time (`_PATH_RESCONF`), not overridable per-process without patching glibc itself, which every dynamically-linked binary on the box depends on. Fixing the one canonical path benefits every current and future host-level tool (`curl`, `git`, `openssl`, anything shelled out to later) for free; patching tools one at a time would need repeating forever.
- **A container can already pull the full DNS record set itself** via the existing `GET /v1/dns/records` (real, working, unauthenticated-by-position REST endpoint) and reformat it however its own server software needs — a genuine, already-available pattern, not something this ADR needed to add.

## Decision

**`<g_base_dir>/resolv.conf`** — persisted in literal `nameserver A.B.C.D` format (not JSON-wrapped), the same "the persisted file IS the served file" convention `dns_write_hosts_file()` already established for the DNS-server hosts-file mechanism. New `daemon/src/resolv.c`/`include/resolv.h`, one small dedicated module, matching this project's own "one job per file" precedent (`swap.c`, `logstore.c`, `diskrole.c`).

**A real, first-class REST resource** — `GET`/`PUT /v1/system/resolv`, `PUT {"nameservers": [...]}` (up to `RESOLV_MAX_NAMESERVERS` = 3, matching glibc's own `MAXNS`) replaces the full list and takes effect **immediately** — `resolv_set()` writes straight to the original `g_base_dir` path, not through the bind-mounted alias, so no reboot is needed and no rename-onto-a-mountpoint subtlety applies. Deliberately generic (a plain list of IPs, no notion of "which container is my DNS server") — covers pointing at one of this platform's own DNS containers (resolve its IP once, `PUT` it) and pointing at a real external resolver with the exact same mechanism.

**Bind-mounted onto `/etc/resolv.conf` once at real `--init-mode` boot**, right after `g_base_dir` itself is mounted. `mkbootroot.c` now stages an empty placeholder file at `etc/resolv.conf` in the control-plane squashfs (this image has no `/etc` at all otherwise, confirmed) — `mount(2)`'s `MS_BIND` requires the target to already exist. Best-effort: a failed bind mount leaves the host with today's status quo (no outbound DNS), not a boot-blocking condition.

**`dnsmasq.recipe`'s documented container invocation** gains real upstream forwarders (`--server=1.1.1.1 --server=8.8.8.8` alongside the existing `-R`) — previously purely authoritative for `.internal`, no recursion for anything else.

**`cixctl resolv [show]` / `resolv set [--nameserver=A.B.C.D ...]`.**

## Consequences

- Closes the real gap this whole investigation started from: once an operator points `/v1/system/resolv` at a real upstream-forwarding resolver (their own network's, or a DNS container built per this same ADR), `pkg_source` fetches, `pkg bootstrap --toolchain-url=`, and `pkg hostbuild`'s git fetch can all use real hostnames — no more LAN-mirror workaround needed, on any box with genuine internet routing.
- `boot_init()`'s new bind-mount code only runs under real `--init-mode` — confirmed inert for this project's entire test suite (no test invokes that flag) — real verification is necessarily live, on an actual reboot, not unit-testable in this sandbox.
- Purely additive: new module, new route, no existing schema touched. `resolv_init()` at daemon startup just loads whatever's already persisted (best-effort, a missing file is not an error) — it never itself performs the bind mount, keeping "read persisted state" and "make it take effect on the real host filesystem" as two separate, independently-reasoned-about operations (`boot_init()`'s own job, `--init-mode`-gated).
