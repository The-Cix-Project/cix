# 0160 — host-level sysctl REST API

## Status

Accepted — every open question resolved directly with the user (see "Resolved open questions" below); implementation not yet started.

## Context

This project already has per-container sysctl support (`run --sysctl=`, `POST /containers`' own `sysctls` field) — but it is **create-time only** (no live/post-creation endpoint exists at all — confirmed by a full grep of `daemon/src/main.c`), and deliberately restricted to the `net.*` tree only (`sysctl_key_is_safe()`, `main.c:7082`, since most non-`net.*` sysctls aren't namespaced at all and a container's own netns is the only sysctl surface a container genuinely owns). It's applied by `container_net_apply_sysctl()` (`src/container_net.c`), which translates `net.ipv4.conf.all.rp_filter` → `/proc/sys/net/ipv4/conf/all/rp_filter` (dots → slashes) and directly `open()`+`write()`s it, inside the child, post-netns-setup, pre-`exec`.

The user confirmed this ADR is about something genuinely different: **host-level** sysctl — the daemon's own root namespace `/proc/sys`, live, GET/PUT via REST, with no scope restriction (unlike the container path's `net.*`-only limit, which exists specifically because most other keys aren't namespaced and therefore have no meaning scoped to a single container). The existing per-container mechanism is untouched by this design.

## Requirements (from the user directly)

- Host-level only — a new REST surface, not an extension of the existing per-container `--sysctl=` (which stays create-time-only, `net.*`-only, exactly as it is).
- Fully open passthrough — no allowlist/denylist. Host-auth write-gating is the only access control, the same posture this project already applies to disks/networking/PKI/every other host-level endpoint (trust the authenticated operator, don't second-guess them with a curated safe-list).
- A persisted "apply these sysctls automatically on every boot" list, REST-managed (paired with kernel modules' own equivalent boot-time list, ADR-0159) — not purely one-shot/ephemeral.

## Decision

**Direct file I/O against `/proc/sys`, no external tool** (unlike ADR-0159's kernel-module work, which deliberately reuses real `modprobe`/`depmod` rather than reimplementing dependency resolution — a plain sysctl read/write has no equivalent "hard, already-solved problem" to defer to; it's the exact same `open()`+`write()`/`open()`+`read()` shape `container_net_apply_sysctl()` already does for the container case, reused directly rather than shelling out to the real `sysctl` binary this project doesn't even stage). Key translation is the identical dots-to-slashes scheme the existing container path already uses (`net.ipv4.ip_forward` → `/proc/sys/net/ipv4/ip_forward`), for the same reason: consistency with the one sysctl-key convention this codebase already has, not a second, different spelling.

- **`GET /v1/system/sysctl/{key}`** — reads `/proc/sys/<translated-path>`. **Value typing, resolved**: generic, not a curated table of known keys — the raw text is split on whitespace; a single-token value is returned as a plain JSON string (`"1"`), a value with more than one token (e.g. `net.ipv4.tcp_wmem`'s real 3-tuple, `net.ipv4.ip_local_port_range`'s real 2-tuple) is returned as a JSON array of strings (`["4096", "16384", "4194304"]`). Uniform for every key, no per-key schema to maintain, and correct for any sysctl this project hasn't specifically thought about yet — the tradeoff accepted deliberately is losing the exact original whitespace character (tab vs. space), which no known real use case needs back. `404` if the path doesn't exist (an invalid or non-existent key — not namespaced/present on this kernel build).
- **`PUT /v1/system/sysctl/{key}`** — `{"value": "1"}` or `{"value": ["4096", "16384", "4194304"]}` (an array is joined with a single space before writing — the kernel's own real separator convention for every known tuple-shaped sysctl), writes it directly. `400` for a value the kernel itself rejects (`write()` failing `EINVAL` and similar — surfaced as the real `errno`, not swallowed), matching this project's own "propagate the real syscall failure, never silently succeed" convention (see `diskformat.c`'s own mkfs-failure handling for the same posture). No allowlist check, per the requirement above — a key that can destabilize the host is exactly as writable as any other; this is a deliberate, explicit design choice, confirmed with the user, not an oversight, despite the real blast radius (some `vm.*`/`kernel.*` keys can panic or badly degrade a running host).
- **`GET /v1/system/sysctl`** — every key set via this same daemon's own persisted config below, **not** a full enumeration of the entire live sysctl tree (thousands of entries under `/proc/sys`, most never touched by this project, expensive to walk and mostly noise) — matches this project's own existing "collection = what this daemon actually manages" convention (e.g. `GET /diskroles` lists assigned roles, not every disk that could theoretically get one) rather than a raw `/proc/sys` mirror. A specific key not yet touched by this daemon is still reachable directly via `GET /v1/system/sysctl/{key}` above; this collection endpoint is about the daemon's own configured set, not host discovery.

**Persisted boot-apply list** — a new, small module, `daemon/src/sysctlconfig.c`, matching `kmodconfig.c`'s own shape from ADR-0159 (same atomic-JSON-persisted table pattern `devicemap.c` already established) and reusing the exact same `PUT`/`GET`/`DELETE` shape as `GET /v1/system/sysctl` above, since "the set of keys this daemon has ever `PUT` a value for" and "the set of keys to reapply at boot" are naturally the same table with one extra field:

- Every successful `PUT /v1/system/sysctl/{key}` also persists `{key, value}` into this table (an ordinary sysctl set doubles as "remember this for next boot," no separate registration step needed) — **unless** the request body includes `"persist": false` (default `true`, resolved: persist by default, matching this project's convention elsewhere — diskrole, devicemap, etc. — opt out per-call for a genuinely one-shot debugging tweak) for a genuinely one-shot, non-persisted change.
- `DELETE /v1/system/sysctl/{key}` removes it from the persisted table too (stops reapplying at boot) — it does **not** attempt to reset the live value to any "original"/default (the kernel itself has no reliable general concept of "the value before this project touched it" to revert to; leaving the live value as whatever it currently is, while ceasing to reapply it going forward, is the only honest option).
- A new boot-time step, `apply_configured_sysctls()`, called from `main()` at a resolved position in the boot sequence (see "Resolved open questions" below).

## Resolved open questions

Decided directly with the user before implementation:

1. **Boot-time ordering**, coordinated with ADR-0159's own boot-time module steps: `load_boot_modules()` (ADR-0061, unmodified, hardware detection) → `apply_configured_sysctls()` (this ADR — a `net.*` sysctl can affect how the management network itself comes up, so it runs before that bootstrap) → `bootstrap_management_network()` → `load_configured_modules()` (ADR-0159, operator autoload, least boot-critical, runs last).
2. **Persist-by-default confirmed** — `PUT`'s implicit persistence stays the default, opt out per-call with `"persist": false`. Already reflected in "Decision" above.
3. **Value typing resolved as generic, not curated** — any multi-token `/proc/sys` value is read/written as a JSON array uniformly, no per-key schema table. Already reflected in "Decision" above.
