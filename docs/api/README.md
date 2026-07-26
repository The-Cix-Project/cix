# Kanxeo Host API

[`openapi.yaml`](openapi.yaml) (OpenAPI 3.0) is the **authoritative** contract — every field, schema, and status code is defined there, not here. This page is a human-friendly index into it, per the project's API-First Mandate: the REST daemon (`daemon/`, binary `kanxeod`) is the only process with direct access to the container runtime, and everything else (CLI, web dashboard) is built by reading this contract, never the daemon's source.

Default base URL: `http://127.0.0.1:7620/v1` (loopback-only by default; see `daemon/src/main.c`'s `--bind`/`--port` flags).

## Endpoints at a glance

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | Liveness check |
| GET | `/containers` | List all containers this daemon knows about |
| POST | `/containers` | Create and start a container |
| GET | `/containers/{name}` | Inspect one container |
| DELETE | `/containers/{name}` | Stop (if running) and remove a container |
| GET | `/networks` | List all networks this daemon knows about |
| POST | `/networks` | Create a network (a real bridge, persisted across restarts) |
| GET | `/networks/{name}` | Inspect one network |
| DELETE | `/networks/{name}` | Remove a network (refused if any container is still attached) |

Every error response is `{"error": "message"}` with an appropriate 4xx/5xx status.

## Creating a network

```
POST /v1/networks
{"name": "internal", "subnet": "172.31.0.0", "prefix_len": 24}
```

- `name` must match `[A-Za-z0-9_-]{1,15}` — it's used verbatim as the Linux bridge interface's name (IFNAMSIZ is 15 chars).
- `subnet` must be the exact network address for `prefix_len` (host bits zero) — `"172.31.0.5"` with `prefix_len: 24` is rejected, only `"172.31.0.0"` is valid. It must also not overlap any existing network's range.
- `prefix_len` must be in `[8, 30]`.
- The gateway is always the subnet's first host address (`.1`) — there's no separate gateway field.

Response (`201`):

```json
{"name": "internal", "subnet": "172.31.0.0", "prefix_len": 24, "gateway": "172.31.0.1"}
```

Creating a network creates its bridge immediately via rtnetlink and persists the definition to `/var/lib/kanxeo/networks.json` — unlike containers (safe to be in-memory-only, since they die with the daemon), a bridge outlives this process, so the daemon reloads and recreates every persisted network's bridge idempotently at startup.

## Creating a container

```
POST /v1/containers
{
  "name": "my-container",
  "image": "test",
  "cmd": ["/bin/some-binary", "arg1"],
  "memory_max": 67108864,
  "pids_max": 32,
  "networks": ["internal", "dmz"]
}
```

- `name` must match `[A-Za-z0-9_-]+` — it's used verbatim as the on-disk directory name under `/var/lib/kanxeo/containers/`.
- `image` must already exist and be populated at `/var/lib/kanxeo/images/{image}/rootfs` — the daemon never creates image content itself (see ADR-0004); a missing image is a `400`, not a silently-empty container.
- `memory_max`/`pids_max` are optional cgroup v2 limits; omit for no limit.
- `networks` is optional: 1–4 names, each already created via `POST /v1/networks` — any unknown name is a `400`. Omit for no networking (isolated netns, only `lo` — same as before this field existed). The **first** entry is primary and gets the default route; the rest only get their own subnet's connected route.

Response (`201`):

```json
{
  "name": "my-container",
  "status": "running",
  "pid": 12345,
  "exit_status": null,
  "networks": [
    {"name": "internal", "ip": "172.31.0.2"},
    {"name": "dmz", "ip": "172.32.0.2"}
  ],
  "ip_forward": false
}
```

## Making a container act as a router

A container attached to two networks with `ip_forward` on will actually forward packets between them — enough for a container running BIRD/FRR to do dynamic routing on top, or for pure static routing on its own:

```
POST /v1/containers
{
  "name": "router",
  "image": "test",
  "cmd": ["/sbin/some-router-process"],
  "networks": ["internal", "dmz"],
  "ip_forward": true
}
```

Other containers then need a static route pointing at the router's IP on their own network to actually reach the far side:

```
POST /v1/containers
{
  "name": "internal-host",
  "image": "test",
  "cmd": ["/bin/some-binary"],
  "networks": ["internal"],
  "routes": [{"dest": "172.32.0.0", "prefix_len": 24, "via": "172.31.0.2"}]
}
```

- `routes` is optional: 0–8 entries, each `{dest, prefix_len, via}` (all required). `dest`/`via` must be well-formed IPv4; `prefix_len` in `[0, 32]`. Only format is validated — whether `via` is actually reachable is the caller's responsibility. Set once at creation; not modifiable on an already-running container.
- `ip_forward` is optional, default `false`. Per-netns — never affects the host or other containers.

## Current scope boundaries (v1, deliberate — see ADR-0007)

- No image build/pull endpoint yet — images are provisioned onto disk out of band.
- No log retrieval endpoint yet — the daemon doesn't capture container stdout/stderr separately.
- No authentication yet — the daemon binds to loopback only as its safety boundary for now.
- HTTP: no keep-alive/pipelining (`Connection: close` on every response), no chunked bodies.
- Routes are set-once at creation and not echoed back or introspectable afterward; modifying them on a running container would need a new "enter another netns from outside" primitive, not built yet. See `docs/ROADMAP.md`.

## Why this file exists alongside `openapi.yaml`

One Source of Truth means the *schema* lives in exactly one place (`openapi.yaml`). This page exists only so a human (or a future CLI/web implementer) can get oriented quickly without parsing YAML first — if the two ever disagree, `openapi.yaml` wins and this page is out of date and should be fixed.
