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
  "network": "internal"
}
```

- `name` must match `[A-Za-z0-9_-]+` — it's used verbatim as the on-disk directory name under `/var/lib/kanxeo/containers/`.
- `image` must already exist and be populated at `/var/lib/kanxeo/images/{image}/rootfs` — the daemon never creates image content itself (see ADR-0004); a missing image is a `400`, not a silently-empty container.
- `memory_max`/`pids_max` are optional cgroup v2 limits; omit for no limit.
- `network` is optional and must name a network already created via `POST /v1/networks` — any other value is a `400`. Omit for no networking (isolated netns, only `lo` — same as before this field existed).

Response (`201`):

```json
{"name": "my-container", "status": "running", "pid": 12345, "exit_status": null, "network": "internal", "ip": "172.31.0.2"}
```

## Current scope boundaries (v1, deliberate — see ADR-0007)

- No image build/pull endpoint yet — images are provisioned onto disk out of band.
- No log retrieval endpoint yet — the daemon doesn't capture container stdout/stderr separately.
- No authentication yet — the daemon binds to loopback only as its safety boundary for now.
- HTTP: no keep-alive/pipelining (`Connection: close` on every response), no chunked bodies.
- Exactly one network per container — a container attached to more than one (the router/VPN prerequisite) is Phase 7 part 2, not yet built. See `docs/ROADMAP.md`.

## Why this file exists alongside `openapi.yaml`

One Source of Truth means the *schema* lives in exactly one place (`openapi.yaml`). This page exists only so a human (or a future CLI/web implementer) can get oriented quickly without parsing YAML first — if the two ever disagree, `openapi.yaml` wins and this page is out of date and should be fixed.
