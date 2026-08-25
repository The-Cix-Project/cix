# 0143 — Explicit per-container `dns_servers` field for `/etc/resolv.conf`

## Status

Accepted

## Context

Investigated directly (not assumed) after the user asked, mid-session, whether a container gets DNS resolution via an explicit host mapping/binding the way some internal Cix services do, or via a standard/global `resolv.conf` — deferred to be answered at the end of the ADR-0141/0142 storage-placement work. The answer, confirmed by reading every relevant code path: **neither exists**. `create_container_from_body()` never writes or bind-mounts anything DNS-related into a container's own overlay; `POST /v1/containers` has no `dns`/`nameservers` field; `dns_server_register()` (the mechanism behind an internal `.internal`-zone DNS server like `dns-1`) only pushes a hosts file *into* the server container itself and `SIGHUP`s it — it never touches any *other* container's own resolver config. A container that needs DNS today has to bake it into its own image, or an operator has to hand-engineer it (the generic `files` staging field, ADR-0030, can already write an arbitrary `/etc/resolv.conf`, but nothing purpose-built exists, and nothing documents this as the intended path). This is a real, previously-undiscovered gap, not a deliberate prior design choice — no ADR, `docs/api/README.md` section, or code comment anywhere claimed containers had working DNS.

ADR-0076 already settled the equivalent question for the *host's own* outbound resolution: `PUT /v1/system/resolv` takes an explicit, operator-supplied nameserver list (up to `RESOLV_MAX_NAMESERVERS` = 3, matching glibc's own `resolv.conf` `MAXNS`) — no automatic discovery of upstream resolvers, no magic. The same reasoning applies here: this project has no fixed notion of "the" internal DNS server (zero, one, or several `dns_server_register()`-registered containers can exist at once, on different networks, and which one(s) a given container should use is a real operator decision, not something safe to infer).

## Decision

**New optional `"dns_servers"` array field on `POST /v1/containers`**: `["a.b.c.d", ...]`, up to `RESOLV_MAX_NAMESERVERS` (3, the same constant `resolv.c` already defines and enforces for the host's own list — one source of truth for "how many nameservers this project's resolv.conf handling ever supports," not a second cap invented here). Each entry validated with `inet_pton(AF_INET, ...)` — the same inline validation idiom every other IPv4 field in `create_container_from_body()` already uses (`routes[].via`, `networks[].ip`, etc.), no new helper needed. Omitted or empty means exactly what it means today: no resolver config staged, a container brings its own or has none.

When given, staged as a real `/etc/resolv.conf` (`nameserver a.b.c.d\n` per line, in the given order) at `<upperdir>/etc/resolv.conf` — reusing the *exact* mechanism `files[]` staging already established (written directly into the container's own upperdir before `clone3()`, so it's simply already there the moment the container's process `execve()`s), not a second file-staging code path. Explicitly rejected (`400`) if the same request *also* supplies a `files[]` entry targeting `/etc/resolv.conf` — an unresolvable ambiguity about which one should win, surfaced loudly rather than one silently overwriting the other depending on array-processing order.

**Deliberately explicit, no auto-wiring to a registered internal DNS server.** An operator who wants a container to resolve `.internal` names points `dns_servers` at that server's own real IP directly (discoverable via `GET /containers/{name}` on the DNS-server container, or `cixctl inspect dns-1`) — the same manual-but-simple posture ADR-0076 already established for the host's own equivalent case, not a new design philosophy invented just for this. A future phase automatically discovering and injecting a network's own DNS server(s) would be new, separate scope on top of this — not something this field tries to solve.

Persists and replays exactly like every other creation-time field already does — the request body is stored verbatim by `containerdef_add()` and replayed unchanged by `create_container_from_body()` on every future revival (`POST .../start`, a crash respawn, boot autostart), no separate persistence work needed, the same shape `files`/`sysctls`/`networks` already have.

## Consequences

- Closes a real, previously-undocumented gap: a container can now be given real, working DNS resolution at creation time, whether pointed at this project's own internal `.internal` zone or an external resolver, with zero new infrastructure (reuses `files[]`'s own staging primitive and `resolv.c`'s own `RESOLV_MAX_NAMESERVERS` constant).
- Deliberately does not solve "make `.internal` resolution automatic for every container" — that remains a manual `dns_servers` entry per container (or baked into an image), consistent with this project's existing no-magic-auto-discovery precedent (ADR-0076). A future ADR would be needed to change that.
- `files[]` and `dns_servers` targeting `/etc/resolv.conf` at the same time is a hard `400`, not a silent last-write-wins — an operator hitting this gets a clear, actionable error instead of debugging why their staged file got overwritten.
