# Reinstalling a host and restoring its state

Reinstalling wipes a Cix host's disk and puts a fresh control plane on it. It is
the route to a change the running system cannot make to itself — moving the
platform's own partitions to a different filesystem is the case this guide was
written for ([#161](https://git.home.arpa/itdlabs/cix/issues/161)).

It is **destructive and partly irreversible**, and the irreversible part is not
the container definitions — those are recoverable. It is the private keys. Read
[What is lost](#what-is-lost) before starting.

For keeping an already-installed box current *without* reinstalling, use
[`staying-updated.md`](staying-updated.md) instead. This guide is the last
resort, not the update path.

---

## What is preserved

`GET /v1/system/backup` bundles everything the platform can rebuild itself
from — container definitions, networks, DNS records, package install state,
volume *definitions*, site config, and every recipe the host holds. Save it
first, before anything else:

```sh
cixctl --host=<host> system backup > backup.json
```

Recipes also live in git, so the bundle is a convenience for them rather than
the only copy. Everything else in it is not.

## What is lost

### The private keys — no export exists, by design

Three keys live only on the host, and **none of them can be read back over the
API**. That is a deliberate design in each case, not an oversight:

| Key | Where the refusal is stated |
|---|---|
| Secure Boot signing key | `openapi.yaml` — *"The private key is never returned by any"* |
| PKI CA key | `openapi.yaml` — *"The CA private key is never returned over the API, in any"* |
| Release signing key | `daemon/include/releasekey.h` — *"Only the public key comes back"* |

`GET /v1/system/backup` says so about PKI in as many words: backing it up is
"a separate, host-level concern entirely, not partially folded into this
mechanism."

So a reinstall destroys all three. What that actually costs:

- **Release signing key.** Artifacts already published stay verifiable —
  [`docs/keys/`](../keys/) is append-only, so the retired public half remains
  published and anyone can still check old signatures without Cix software.
  What is lost is the ability to sign *new* artifacts with that key. Rotate:
  the new host generates a new pair, and its public half is committed
  alongside the old one, never replacing it.
- **PKI CA.** Every certificate it issued is orphaned. A fresh CA is generated
  at install and certificates are reissued. Anything outside the host that
  pinned the old CA has to be repointed.
- **Secure Boot key.** Regenerated at install; the machine re-enrolls it
  through MokManager on first boot, exactly as a first install does.

If any of those matters more than the reinstall does, stop here and copy them
off by a host-level route first — that is outside this API and outside this
guide.

### Volume content

`GET /v1/system/backup` carries volume *definitions*, never volume *content* —
workload data is each container's own concern, with its own export tooling.
Volume data lives under `<data-dir>/volumes/<name>` on the containers
partition, which is reformatted.

`GET /v1/containers/{name}/files` can read content out, but it returns content
only — no mode, no ownership
([#139](https://git.home.arpa/itdlabs/cix/issues/139)) — so anything executable
or permission-sensitive does not survive that route faithfully. Use the
workload's own export where one exists.

---

## Procedure

### 1. Back up, and verify the backup is real

```sh
cixctl --host=<host> system backup > backup.json
python3 -c "import json;d=json.load(open('backup.json'));print({k:(len(v) if isinstance(v,(list,dict)) else 1) for k,v in d.items()})"
```

A bundle that parses and lists the fields you expect is the check. A zero-byte
or truncated file that nobody opened is the failure this step exists to catch.

### 2. Build installer media matching the version you are on

A reinstall from older media lands the box on that older version, which then
has to be upgraded forward. Building fresh media first avoids that entirely:

```sh
curl -X POST -H "Authorization: Bearer $TOKEN" -d '{}' http://<host>/v1/system/iso
# poll until state is "ready"
curl http://<host>/v1/system/iso
```

The build needs three hostbuild artifacts present — `cix`, `kernel` and
`isotools` — and the signing key pair set. It fails with a 400 naming the
missing precondition if not.

Then publish it, so the media is fetchable from somewhere that is not the host
about to be wiped:

```sh
curl -X POST -H "Authorization: Bearer $TOKEN" -d '{}' http://<host>/v1/system/iso/publish
```

The artifact cache **refuses an ISO whose signature has not been published**
(409), so the signature goes first — the ISO build produces both, and publish
handles the ordering.

### 3. Boot the media and install

This step is not reachable over the API. An installed Cix host has no shell and
no SSH ([ADR-0034](../adr/0034-console-login-via-supervised-cixctl.md)), so
attaching installer media is a console or hypervisor action.

The installer partitions the disk as `cix-esp`, `cix-root-a`, `cix-root-b`,
`cix-config` and `cix-containers`, and formats the last two **btrfs**
(`cix-install.c`'s own `mkfs_btrfs()` calls). Confirm the partition layout it
reports before letting it write.

### 4. Restore

```sh
cixctl --host=<host> system restore < backup.json
```

Fields are independent and all optional, so a partial restore is legitimate —
container definitions alone, or networks alone. Every field is validated on the
way in.

Then let the packages rebuild from `pkg_installed`: that field is the shopping
list, not the built bytes, and everything is compiled from source.

### 5. Rotate what was destroyed

- Commit the new release public key into [`docs/keys/`](../keys/) **alongside**
  the retired one. Never replace it — artifacts signed by the old key stay
  verifiable only while its public half is published.
- Reissue any certificates that mattered outside the host.
- Re-enroll Secure Boot at first boot when MokManager prompts.

---

## Verifying the result

The point of the exercise is usually a specific property, so check that one
directly rather than checking that the box is up:

```sh
curl http://<host>/v1/disks          # cix-config and cix-containers fs_type
curl http://<host>/v1/system/boot    # build_version, slot
curl http://<host>/v1/containers     # the fleet came back
```

`GET /v1/disks` reads the superblock, so it answers what the filesystem
actually is rather than what was intended.
