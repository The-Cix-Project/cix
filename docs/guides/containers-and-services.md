# Containers and services

Running workloads on a Cix host: what a container is here, the image it
comes from, the services it declares, how it is kept running, how you
reach inside it, where its data lives, and how to describe it once as a
deployment and re-create it at will.

Task-oriented. The field-by-field contract is
[`docs/api/README.md`](../api/README.md) and
[`docs/api/openapi.yaml`](../api/openapi.yaml); every flag is in
`cixctl help` and [`cli-reference.md`](cli-reference.md). Each section
links the ADR that owns the *why*.

---

## What a container is here

A container is a set of Linux namespaces and a cgroup that `cixd`
creates directly, with no runc or other runtime in between. Every
container has:

- **an image version** it was created from — its root filesystem is a
  btrfs snapshot (or a copy) of that version, see
  [`storage.md`](storage.md#6-snapshots-and-why-container-storage-is-cheap);
- **`cix-init` as pid 1** — the platform's own supervisor, staged into
  the container by the daemon, which runs the **services** the container
  declares ([ADR-0260](../adr/0260-a-container-declares-services-not-a-command.md)).
  There is no single "command";
- **a persisted definition** — the exact create request, stored when the
  container is created. It survives daemon restarts and reboots, and
  only a delete removes it ("stop is stop, delete is delete",
  ADR-0181);
- **a user namespace by default** — `userns` absent means the host
  default (`daemon-config`'s `userns_default`, on for a fresh install).
  The resolved value is fixed at creation.

What a container is *not*: somewhere to keep data. Its rootfs is derived
from its image and is re-seeded when the image version it is pinned to
moves ([ADR-0277](../adr/0277-a-container-rootfs-is-derived-from-its-image.md)).
Data that must outlive that goes on a [volume](#volumes).

---

## Images

A container is always created from a named image. An image is its
declared packages plus a small baseline, built on the host from recipes:

```sh
cixctl image ls
cixctl image create --name=resolver                   # empty: no C library yet
cixctl pkg install --name=glibc --image=resolver      # the C library, installed explicitly
cixctl pkg install --name=dnsmasq --image=resolver    # builds from its recipe if no artifact exists
```

**A new image has no C library.** `image create` makes only the
baseline (device nodes, directories, `nsswitch.conf`), and
`POST /v1/containers` refuses an image with no dynamic loader, saying
`install a libc package (glibc)` (`daemon/src/pkg.c`,
`daemon/src/main.c`). Install `glibc` yourself: packages such as `bash`
and `coreutils` do not declare it as a runtime dependency, so installing
them does not bring it in. The `base` image exists from first boot; at
startup the daemon installs `glibc` into it only when a `glibc` recipe
is published and its artifact is already in the local cache, and logs
why when it cannot.

Every install, upgrade or uninstall produces a new, immutable image
version. A running container keeps the version it was created from
until it is re-created or follows the image with `--follow-rolling`
(see [`administration.md`](administration.md#does-installing-a-package-onto-an-image-reach-containers-already-running-from-it)).
Image recipes and manifests (`cixctl image recipe`, `cixctl image
manifest`) are how an image is declared rather than built by hand; see
[`images.md`](images.md), and [`writing-recipes.md`](writing-recipes.md)
for package recipes.

`cixctl container run` against an image that has not been built yet
fails immediately. Applying a [deployment](#deployments) against one
waits for it instead.

---

## Running a container

```sh
cixctl container run --name=resolver --image=resolver \
    --network=lan1:172.31.0.20 \
    --service="dnsmasq=/usr/sbin/dnsmasq -k -u root --pid-file=" \
    --restart=always
```

`dnsmasq -k` stays in the foreground, which is what a supervised service
must do; `--pid-file=` stops it writing `/var/run/dnsmasq.pid`, a path
the platform's minimal images do not have.

`--name`, `--image` and at least one `--service` (or `--oneshot`) are
required. Everything else is optional:

| Flag | What it sets |
|---|---|
| `--network=NAME[:IP]` | Attach to a network, repeatable; the first is primary and gets the default route. Omit `:IP` to have one allocated. See [`networking.md`](networking.md) |
| `--route=DEST/PREFIX:VIA` | A static route inside the container, up to 8 |
| `--ip-forward` | Forward between this container's own networks (per-netns) |
| `--dns-server=A.B.C.D` | Resolvers for this container, repeatable |
| `--dns-register` | Register the container's name in the platform's DNS records |
| `--interface=IFNAME` | Move a host NIC (or a radio's whole PHY) into the container, exclusively |
| `--memory-max=BYTES` `--memory-swap-max=BYTES` `--pids-max=N` `--cpu-max="QUOTA PERIOD"` `--cpuset=0-1,3` | cgroup v2 limits. `--memory-swap-max=0` forbids swap |
| `--disk=NAME` | Put its storage on a disk with the `container-storage` role ([`storage.md`](storage.md#3-the-roles-you-assign)) |
| `--disk-quota=BYTES` | Kernel-enforced limit on its own storage |
| `--volume=NAME:/path[:ro]` | Mount a [volume](#volumes), up to 8 |
| `--file=CONTAINER_PATH=LOCAL_PATH[:MODE]` | Stage a local file into the container before it starts; `--file-owner=CONTAINER_PATH:UID:GID` sets its owner |
| `--env=KEY=VALUE` | An environment variable, up to 32. A container gets only these, never the daemon's environment |
| `--sysctl=KEY=VALUE` | A `net.*` sysctl inside its own netns, up to 32 |
| `--device=ID` / `--optional-device=ID` | Pass a host device through; see [`administration.md`](administration.md#device-hotplug) |
| `--cap-add=CAP_NAME` | Keep a capability that is dropped by default |
| `--userns` | Run in a user namespace explicitly (the default already does) |
| `--ksm` | Volunteer its memory to kernel samepage merging |
| `--capture-output` | Keep its recent output on the container object; see [Output](#output-and-logs) |
| `--restart=…` `--restart-delay=N` `--depends-on=NAME` | See [Staying up](#staying-up-restart-policy-and-lifecycle) |
| `--follow-rolling` `--follow-rolling-jitter-seconds=N` | Re-seed onto each new image version automatically |
| `--pki-issue` / `--pki-cert=NAME`, `--ldap-client`, `--ldap-provision`, … | Certificates and LDAP accounts; see [`security.md`](security.md) |

**Quoting.** `--service=`, `--oneshot=`, `--console=` and `--ready=…command:`
split their command on spaces and do no shell-style quoting inside it.
Put the whole flag in quotes so the shell passes it as one argument, as
above. An argument that itself contains a space cannot be written this
way; declare that container as a [deployment](#deployments), where `cmd`
is a real JSON array.

Some things have no `container run` flag and exist only in the create
body: a network attachment's `ifname` and `container_bridge`, a staged
file's inline `content`, per-service `stop_signal`, `uid`/`gid`,
`restart_delay_seconds`, and the server-role fields (`dns_server`,
`ntp_server`, `syslog_target`, `ldap_server`). Use a deployment for
those.

Check the result:

```sh
cixctl container ls
cixctl container inspect resolver
cixctl container stats resolver
```

---

## Services

A container declares what it runs as a list of services. `cix-init`
starts them in dependency order, supervises them and reports their
state ([ADR-0260](../adr/0260-a-container-declares-services-not-a-command.md)).

```sh
cixctl container run --name=jump --image=jumpbox \
    --oneshot="hostkeys=/usr/bin/ssh-keygen -A" \
    --service="nslcd=/usr/sbin/nslcd -d" --ready=nslcd:socket:/run/nslcd/socket \
    --service="sshd=/usr/sbin/sshd -D -e" --ready=sshd:tcp:22 \
    --after=sshd:hostkeys,nslcd \
    --on-exit=sshd:fail-container
```

- **`--service=NAME=/path args`** declares a **daemon**: long-running
  and supervised. `argv[0]` must be an absolute path; it is exec'd
  directly, with no shell.
- **`--oneshot=NAME=/path args`** declares a **oneshot**: it runs to
  completion and must exit 0. This is how setup work happens without a
  shell script in the image.
- **`--after=NAME:DEP[,DEP]`** starts `NAME` once every `DEP` is
  **ready**. A daemon is ready when its probe passes (or when it has
  started, if it has no probe); a oneshot is ready when it exits 0.
  Declaration order does not matter. A cycle or an unknown name is
  refused with `400`.
- **`--ready=NAME:tcp:PORT`**, **`--ready=NAME:socket:/path`** or
  **`--ready=NAME:command:/path args`** says how to tell `NAME` is ready.
  A TCP probe tries `127.0.0.1` and then each of the container's
  network addresses. A probe that has not passed within its timeout
  (30 s by default) is reported as a failure, and the service is treated
  as ready so its dependents still start.
- **`--on-exit=NAME:restart|stop|fail-container`** says what happens
  when `NAME` exits. Daemons default to `restart` (after 2 s), oneshots
  to `fail-container`. `fail-container` stops the other services in
  reverse order and ends the container with this service's exit status,
  which the container's [restart policy](#staying-up-restart-policy-and-lifecycle)
  then acts on. A daemon that restarts does so alone; its dependents are
  not restarted with it.

A container allows up to 16 services, each with up to 32 argv entries.
Attribute flags (`--after`, `--ready`, `--on-exit`) must come after the
`--service`/`--oneshot` they name.

**Reading state.** `cixctl container inspect NAME` shows `ready` (every
probed service passed and every oneshot exited 0) and each service's
`state` — `pending`, `starting`, `running`, `exited`, `restart-wait`,
`stopped` or `failed` — with its pid, restart count and last exit.

**The container's exit status** is the status of the service that ended
it (`128+signal` for a signal), `0` after an orderly shutdown, `125` if
`cix-init` could not read its service table, and `140+errno` if a
service's `execve()` failed. A container whose services have all exited
with nothing left to restart simply exits, so a oneshot-only container
is a job.

### Controlling one service

```sh
cixctl container service stop jump sshd
cixctl container service start jump sshd
cixctl container service restart jump sshd
```

These act on one service and leave the rest of the container running.
A stop is an **override, not an edit**: the service is held stopped
until the container next starts, when the declaration applies again.
Nothing here changes the stored definition.

---

## Staying up: restart policy and lifecycle

`--restart=` decides only whether a container comes back **on its own**;
every container is kept until it is deleted, whatever its policy.

| `--restart=` | Comes back after it exits | Comes back at daemon start |
|---|---|---|
| `no` (default) | No — kept with its exit status | No |
| `on-failure` | Only after a non-zero exit or a signal | Yes |
| `always` | Yes, even after exit 0 | Yes |
| `unless-stopped` | Yes, even after exit 0 | Yes, unless you stopped it |

Restarts are delayed by `--restart-delay=N` seconds (default 2), backing
off on repeated failures; the exact rule is in
[`docs/api/README.md`](../api/README.md#persisted-containers-and-the-restart-policy).

`--depends-on=NAME` orders start-up at daemon boot: `NAME` is started,
and waited for until ready (derived from its services), before this
container starts. It is best-effort and never blocks a boot.

```sh
cixctl container stop resolver       # stop it, keep it
cixctl container start resolver      # start a stopped or exited container
cixctl container pause resolver      # freeze every task (cgroup freezer)
cixctl container unpause resolver
cixctl container drift          # containers that should be running and are not; exits 1 if any
```

Stop and delete of a running container are asynchronous: the container
shows `stopping` or `deleting` until the kernel confirms the exit, and a
same-name create is refused with `409` until then.

---

## Consoles: reaching inside

A container is reachable only through the consoles it **declares**
([ADR-0261](../adr/0261-reaching-inside-a-container.md)). There is no
`exec` and no way to name a program at attach time.

```sh
cixctl container run --name=jump --image=jumpbox \
    --service="sshd=/usr/sbin/sshd -D -e" \
    --console="shell=/usr/bin/bash -l" \
    --console="logs=/usr/bin/tail -F /var/log/messages"

cixctl container console jump                  # the first declared console
cixctl container console jump --console=logs   # by name
```

- Up to four consoles, each a name and a real argv. The first declared
  is the default.
- A container that declares no console has none, and says so. That is
  the right answer for an image holding one static binary and no shell.
- The console is a real terminal: it gets your window size and `$TERM`
  and follows resizes, so `htop` and `vim` work.
- To debug with something new, add it to the container's consoles
  (`container edit`, or the deployment) and re-create or restart it.

The web dashboard offers the same consoles.

---

## Files, environment and live file changes

`--file=`, `--env=` and `--sysctl=` are part of the stored definition
and are applied at every start. Two limits worth knowing:

- a staged file under `/run` is refused, because `/run` is a fresh tmpfs
  at every container start and would hide it;
- the platform's minimal images have no `/tmp` and no `/bin/bash`
  (`/usr/bin/bash` is the real path).

To read or change a file in a container that already exists:

```sh
cixctl container files get resolver --path=/etc/dnsmasq-hosts --output=hosts
cixctl container files put resolver --path=/etc/dnsmasq-hosts --file=hosts --mode=0644
```

`files put` is live and ephemeral: it is not part of the definition, a
declared `--file=` at the same path is staged over it at the next start,
and it is lost when the rootfs is re-seeded. Put anything permanent in
the definition. `files get` returns content only, with no mode or
ownership, and may answer from the image version's tree rather than the
container's own; the `X-Cix-Source` response header says which tree
answered (#394). To know what a running container really sees, look
from one of its consoles.

---

## Volumes

A volume is a named piece of storage whose lifetime is independent of
any container ([ADR-0183](../adr/0183-persistent-volumes.md)). Deleting
or re-seeding a container never touches its volumes, which is what makes
them the home for `/home`, a database, or anything else worth keeping.

```sh
cixctl volume create --name=jump-home --owner-uid=1000 --owner-gid=1000
cixctl container run --name=jump --image=jumpbox --volume=jump-home:/home \
    --service="sshd=/usr/sbin/sshd -D -e"
```

- **Create it first.** An unknown volume name is refused with `400`; it
  is never created implicitly.
- **Owner.** A volume created without `--owner-uid`/`--owner-gid`
  belongs to root, which a non-root workload cannot write to. Change it
  later with `cixctl volume owner NAME --uid=N --gid=N [--recursive]`.
- **Placement.** `--disk=` on `volume create` places it on a disk with a
  role, the same naming a container's `--disk=` uses; `cixctl volume
  migrate NAME --disk=DISK` moves it (refused while a container using it
  is running).
- **Read-only.** `--volume=NAME:/path:ro`. If the read-only remount
  fails, the container fails to start rather than coming up writable.
- **A volume that fails to mount stops the container** (exit 125), so a
  workload never runs silently without its storage.
- **Size limit.** `cixctl volume quota NAME BYTES` (0 removes it) sets a
  kernel-enforced limit; `cixctl volume usage NAME` measures what it
  holds now.
- **On an existing container:** `cixctl container volume attach NAME
  --volume=VOLUME --path=/mount/point [--read-only]` and `container
  volume detach NAME VOLUME` edit the definition; the response says
  whether the mount applied now or applies on next start.
- **Deleting.** `cixctl volume rm NAME` is refused while any container
  definition still references the volume.

**Backups** of a volume's contents are opt-in per volume and go to a
disk with the `backup` role ([`storage.md`](storage.md#3-the-roles-you-assign)):

```sh
cixctl volume backups jump-home --enable --retain=7 --while-running=pause
cixctl volume backup jump-home                      # one now
cixctl volume backups jump-home                     # list snapshots
cixctl volume restore jump-home 20260822T030000Z    # replaces the contents
```

`--while-running` decides what happens when a container using the
volume is running: `refuse` (the default, which for an always-on
container means never), `pause` (freeze every container using it for a
consistent copy) or `allow` (copy live, crash-consistent). Restore is
refused while a container using it is running. The shared interval is
`PUT /v1/system/volume-backup-config`, which has no CLI command; the
web dashboard sets it. Full rules:
[`docs/api/README.md`](../api/README.md#backing-up-what-a-volume-holds).

Volume contents are not in `cixctl backup`; the bundle carries only the
volume registry, and a restore recreates volumes empty.

---

## Networks

Attach at creation with `--network=NAME[:IP]`, or later:

```sh
cixctl container network attach resolver --network=dmz --ip=172.32.0.20
cixctl container network detach resolver dmz
```

A live attach is not added to the definition, so it is gone after the
next restart; put a permanent attachment in the definition. `detach`
works only on a network attached this live way — one from the
definition is refused with `409` — and `container inspect` marks each
network entry `live` so you can tell which is which. Creating
networks, VLANs, routing between networks and giving a container a
radio are in [`networking.md`](networking.md).

---

## Output and logs

Every container's stdout and stderr goes to the consolidated log store,
always, with no opt-in. Each line is prefixed with the service that
wrote it (`cix-init:` for the supervisor itself):

```sh
cixctl logs --container=resolver --tail=100
cixctl logs --container=resolver --regex='error|fatal'
```

`--capture-output` (`capture_output: true`) additionally keeps the last
4096 bytes of output on the container object itself, as
`captured_output` in `cixctl container inspect`. Use it for a service
that starts and then exits: the text survives the exit and sits next to
the exit status. `captured_output` is `null` when capture was not
requested and `""` when it was but nothing was written yet. Details:
[`docs/api/README.md`](../api/README.md#diagnosing-a-container-that-starts-but-exits-on-its-own-capture_output).

---

## Deployments

A deployment is a container's create body stored under its name, so the
container can be re-created from it at any time
([ADR-0151](../adr/0151-container-recipes.md)). It is the complete form:
every field of `POST /v1/containers`, with `cmd` as real JSON arrays and
nothing lost to flag syntax. The platform's own deployments are in the
`cix-recipes` repository as `recipes/deployment/<name>@<version>.json`.

A real one, `syslog-1@1.2.0.json`:

```json
{
  "name": "syslog-1",
  "image": "syslog",
  "services": [
    {
      "name": "syslogd",
      "cmd": ["/usr/sbin/syslogd", "-F", "-K", "-n", "-H",
              "-P", "/run/syslogd.pid", "-C", "/run/syslogd.cache",
              "-f", "/etc/syslog.conf"],
      "on_exit": "fail-container"
    }
  ],
  "networks": [{"name": "services", "ip": "192.168.150.107"}],
  "restart": "always",
  "files": [{"path": "/etc/syslog.conf", "content": "*.*\t\t\t\t\t-/var/log/messages\n"}],
  "routes": [{"dest": "0.0.0.0", "prefix_len": 0, "via": "192.168.150.254"}]
}
```

```sh
cixctl deployment add --name=syslog-1 --file=syslog-1.json   # store or replace it
cixctl deployment ls
cixctl deployment show syslog-1                               # the raw, unsubstituted text
cixctl deployment apply syslog-1                              # create the container
cixctl deployment rm syslog-1
```

- The file's own `"name"` must equal `--name`, or `add` is refused. Its
  content is not otherwise checked until it is applied.
- **`apply` creates.** It goes through the same create path as
  `POST /v1/containers`, so a container with that name must not already
  exist; to re-create, `cixctl container rm NAME` first.
- **Secrets.** A `{{SECRET:KEY}}` token anywhere in the text is replaced
  at apply time from `--secret=KEY=VALUE`, correctly escaped:

  ```sh
  cixctl deployment apply ar-1 --secret=WIFI_PASSPHRASE='a real passphrase'
  ```

  The stored deployment keeps the token, so it can live in git. A token
  with no matching `--secret` is left as literal text and a warning is
  logged. The container's own stored definition is the rendered body,
  secret included.
- **LDAP values.** `{{LDAP:URI}}`, `{{LDAP:BASE_DN}}`, `{{LDAP:BIND_DN}}`
  and `{{LDAP:BIND_PASSWORD}}` are filled from the host's LDAP client
  configuration, so a deployment needs no copied values for them.
- **An unbuilt image waits.** If the image exists but has not been built,
  `apply` returns `202`, queues the image build, and creates the
  container once the image is ready, across a reboot if need be. Watch
  it in `cixctl pipeline`. See
  [`administration.md`](administration.md#deploying-against-an-image-that-has-not-been-built-yet).
- **Server roles.** `dns_server`, `ntp_server`, `syslog_target` and
  `ldap_server` in the body register the container as that kind of
  server when it is created, so re-creating it from the deployment
  restores the registration too.

---

## Editing a container

```sh
cixctl container edit resolver --json='{"env": {"TZ": "UTC"}}'
```

Merges the given top-level fields into the stored definition: a field
given replaces that field, a field set to `null` removes it, and the
rest is unchanged. It applies at the **next start**; the output says so,
and says when the container is running and needs a restart.

- `name`, `restart`, `restart_delay_seconds`, `depends_on`,
  `follow_rolling` and `follow_rolling_jitter_seconds` are refused with
  `400`; re-create the container to change them.
- The merged body is stored without being validated; a mistake shows up
  as a failed start (`handle_container_patch()`, `daemon/src/main.c`).
- The merge is top-level only, so changing one entry of `files` or
  `services` means sending the whole list.

Anything more than a small change is better made in the deployment and
applied again.

---

## Deleting

```sh
cixctl container rm resolver
```

Removes the container, its stored definition and its storage. It does
not remove its volumes, and it removes certificates and LDAP accounts
the container owned (`--pki-issue`, `--ldap-provision`); a certificate
given with `--pki-cert` is left alone. Deleting a running container is
asynchronous; wait for `cixctl container inspect NAME` to report that
it no longer exists before creating one with the same name.

---

## Where the decisions live

| Topic | ADR |
|---|---|
| Services, not a command | [ADR-0260](../adr/0260-a-container-declares-services-not-a-command.md) |
| Consoles are declared; no exec | [ADR-0261](../adr/0261-reaching-inside-a-container.md) |
| A container's rootfs is derived from its image | [ADR-0277](../adr/0277-a-container-rootfs-is-derived-from-its-image.md) |
| Persistent volumes | [ADR-0183](../adr/0183-persistent-volumes.md) |
| Deployments (container recipes) and secrets | [ADR-0151](../adr/0151-container-recipes.md) |
| A deployment waits for its image | [ADR-0270](../adr/0270-a-deployment-waits-for-its-image-rather-than-being-refused.md) |
| Rolling containers | [ADR-0124](../adr/0124-pkg-redesign-part5-rolling-containers-and-restart-jitter.md) |
| btrfs substrate and user namespaces by default | [ADR-0207](../adr/0207-btrfs-storage-substrate-userns-by-default.md) |
