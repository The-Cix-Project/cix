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
| GET | `/dns/records` | List all DNS records this daemon knows about |
| POST | `/dns/records` | Create a DNS record (name -> IP, persisted across restarts) |
| GET | `/dns/records/{name}` | Inspect one DNS record |
| DELETE | `/dns/records/{name}` | Remove a DNS record |
| GET | `/dns/servers` | List all registered DNS server bindings |
| POST | `/dns/servers` | Register a running container as a DNS-serving target |
| DELETE | `/dns/servers/{container}` | Unregister a DNS server binding |
| GET | `/pki/ca` | Inspect the root CA (never includes the private key) |
| POST | `/pki/ca` | Bootstrap the root CA (once; no regeneration in v1) |
| GET | `/pki/certs` | List all issued leaf certificates (metadata only) |
| POST | `/pki/certs` | Issue a leaf certificate signed by the root CA |
| GET | `/pki/certs/{name}` | Inspect one issued certificate (metadata + cert, never the key) |
| DELETE | `/pki/certs/{name}` | Remove an issued certificate |

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
- `networks` is optional: 1–64 names, each already created via `POST /v1/networks` — any unknown name is a `400`. Omit for no networking (isolated netns, only `lo` — same as before this field existed). The **first** entry is primary and gets the default route; the rest only get their own subnet's connected route.
- `dns_register` is optional, default `false` — see [DNS: records + a real dnsmasq container](#dns-records--a-real-dnsmasq-container) below. Requires `networks` to be set (`400` otherwise).

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

## DNS: records + a real dnsmasq container

DNS records are a REST resource; the actual name resolution is done by a real DNS server (dnsmasq recommended) running as a normal containerized workload — not hand-rolled, the same reasoning BIRD wasn't hand-rolled for routing (ADR-0007's "no external libraries" rule is about this project's own platform components, not about workloads a container runs).

```
POST /v1/dns/records
{"name": "db.internal", "ip": "172.31.0.5"}
```

- `name` is a hostname (dot-separated labels, `[A-Za-z0-9-]`, RFC 1035 length limits) — a different charset from network/container names, which don't allow dots.
- `ip` must be well-formed IPv4.

Once a container running dnsmasq exists (e.g. `cmd: ["/usr/sbin/dnsmasq", "-k", "-u", "root", "-p", "53", "-H", "/etc/dnsmasq-hosts", "-R", "-h"]` — `-u root` since a minimal container image typically has no `/etc/passwd` for dnsmasq's default privilege drop to resolve; `-R`/`-h` skip `/etc/resolv.conf`/`/etc/hosts`, which likely don't exist either), register it:

```
POST /v1/dns/servers
{"container": "dns1", "hosts_path": "/etc/dnsmasq-hosts"}
```

This writes every current record into `dns1`'s own `/etc/dnsmasq-hosts` (dnsmasq's `--addn-hosts` format) and sends `SIGHUP` so it reloads immediately. Every subsequent `POST`/`DELETE` on `/v1/dns/records` re-writes that file and re-signals `dns1` — genuinely live updates, not a one-time snapshot at registration.

The write itself goes through `/proc/<pid>/root/<hosts_path>` (the container's own filesystem view via the magic procfs symlink), not the container's raw upperdir directly — writing straight into a running container's upperdir does **not** reliably show up in its mounted view (confirmed empirically; the kernel documents this as unsupported/undefined for an already-mounted overlay). `/proc/<pid>/root/` correctly resolves through the container's real mount namespace without needing any new namespace-entry syscall.

### Automatic registration: `dns_register`

Instead of a separate `POST /v1/dns/records` call, a container can register its own name at creation time:

```
POST /v1/containers
{
  "name": "db",
  "image": "test",
  "cmd": ["/bin/some-binary"],
  "networks": ["internal"],
  "dns_register": true
}
```

This creates a record named `db` pointing at `db`'s IP on its primary (first) network, as part of container creation — any already-registered dnsmasq container (`POST /v1/dns/servers`) picks it up immediately via the same live-reload path as a manually-created record, so `dig db.internal.example @dns1` (or whatever domain dnsmasq is configured to answer for) resolves right away. `GET /v1/dns/records/db` shows `"owner": "db"` to distinguish it from a manually-created record (`"owner": null`). Deleting the `db` container automatically removes its record; a manually-created record is never touched by any container's deletion, even if it happens to share that container's name but wasn't the one that created it.

Registration is best-effort and non-fatal to container creation: if a record named `db` already exists (e.g. a stale one persisted from a previous container of the same name — DNS records outlive a daemon restart, containers don't), registration is silently skipped rather than overwriting it, and the container is still created successfully.

## PKI: a root CA and issued leaf certificates

A single internal root CA plus leaf certificate issuance. Actual cryptography (keypair generation, CSR signing) is done by the daemon shelling out to the system's real, unmodified `openssl` binary as a short-lived subprocess — the same "real software, not hand-rolled" reasoning BIRD and dnsmasq were chosen under (ADR-0007's "no external libraries" rule governs this project's own platform components, not real software it invokes or runs as a workload).

Bootstrap the CA once:

```
POST /v1/pki/ca
{"common_name": "Kanxeo Root CA", "days": 3650}
```

Both fields are optional (shown defaults). **The CA private key is never returned over the API, in any endpoint, ever** — it's the root of trust and must never leave the host. A second `POST /v1/pki/ca` is a `409`; there's no CA regeneration in v1.

Issue a leaf certificate:

```
POST /v1/pki/certs
{"name": "svc.internal", "sans": ["svc.internal", "svc"], "days": 365}
```

- `name` is a hostname (same RFC 1035 rules as `DnsRecord.name`) — it becomes the certificate's CN and this endpoint's REST identifier.
- `sans` is optional and defaults to `[name]` — a cert always carries at least its own name as a Subject Alternative Name.
- `days` is optional, default 365.

The `201` response (`PkiCertIssued`) is the **only** place the leaf's private key is ever returned:

```json
{
  "name": "svc.internal",
  "serial": "11EAFF2213CAE630340F388C62AE620833EBEDFF",
  "not_after": "Jul 27 10:25:19 2027 GMT",
  "sans": ["svc.internal", "svc"],
  "cert_pem": "-----BEGIN CERTIFICATE-----\n...",
  "key_pem": "-----BEGIN PRIVATE KEY-----\n..."
}
```

Neither `GET /v1/pki/certs` (list) nor `GET /v1/pki/certs/{name}` (single) ever includes `key_pem` again — save it now. Both do include `cert_pem` on the single-item view (list omits it too, to keep listing lightweight).

Deleting a cert (`DELETE /v1/pki/certs/{name}`) removes its key and cert files from disk, not just the metadata index entry.

## Current scope boundaries (v1, deliberate — see ADR-0007)

- No image build/pull endpoint yet — images are provisioned onto disk out of band.
- No log retrieval endpoint yet — the daemon doesn't capture container stdout/stderr separately.
- No authentication yet — the daemon binds to loopback only as its safety boundary for now.
- HTTP: no keep-alive/pipelining (`Connection: close` on every response), no chunked bodies.
- Routes are set-once at creation and not echoed back or introspectable afterward; modifying them on a running container would need a new "enter another netns from outside" primitive, not built yet. See `docs/ROADMAP.md`.
- DNS server bindings are in-memory only (not persisted, like the container registry itself — a binding referencing a container that dies with the daemon means nothing after a restart anyway). Only one hosts-format record type; no CNAME/MX/TXT/etc.
- PKI: no certificate revocation/CRL, no CA regeneration/rotation, no CSR-submission flow (the daemon always generates both the keypair and the cert itself) — see `docs/ROADMAP.md` Phase 9.

## Why this file exists alongside `openapi.yaml`

One Source of Truth means the *schema* lives in exactly one place (`openapi.yaml`). This page exists only so a human (or a future CLI/web implementer) can get oriented quickly without parsing YAML first — if the two ever disagree, `openapi.yaml` wins and this page is out of date and should be fixed.
