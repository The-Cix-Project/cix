# Reinstalling a host and restoring its state

Reinstalling wipes a Cix host's disk and puts a fresh control plane on it. It is
the route to a change the running system cannot make to itself — moving the
platform's own partitions to a different filesystem is the case this guide was
written for ([#161](https://git.home.arpa/itdlabs/cix/issues/161)).

It is **destructive**. The part that needs planning is not the container
definitions, which a backup carries; it is the private keys and volume data.
Read [What a backup does not carry](#what-a-backup-does-not-carry) before starting.

For keeping an already-installed box current *without* reinstalling, use
[`staying-updated.md`](staying-updated.md) instead. This guide is the last
resort, not the update path.

---

## What a backup carries

`GET /v1/system/backup` bundles what the platform rebuilds itself from:
container definitions, networks, DNS records, volume *definitions*, package
install state, every recipe the host holds, and site config. Save it first:

```sh
cixctl --host=<host> backup --output=backup.json
```

`--output=` writes the response byte-for-byte, which is what `restore` expects.
Recipes also live in git, so the bundle is a convenience for them rather than
the only copy. Everything else in it is not.

## What a backup does not carry

### Private keys

No endpoint ever returns a private key, and the backup bundle holds none. Each
key has its own route across a reinstall:

| Key | Across a reinstall |
|---|---|
| PKI (root CA, intermediate, every leaf) | `cixctl pki export` writes the whole store encrypted under a passphrase; `cixctl pki import` restores it on the new install ([ADR-0281](../adr/0281-the-ca-leaves-the-box-encrypted-or-it-is-lost.md), [Carrying the CA across a reinstall](security.md#carrying-the-ca-across-a-reinstall)) |
| Secure Boot signing pair (used by `iso build`) | Installed by the operator with `cixctl signing-keys set --key=PATH --cert=PATH` ([ADR-0212](../adr/0212-signing-keys-over-rest.md)). Install it again from your own copy |
| Release-signing key (signs published artifacts) | Installed by the operator with `cixctl release-key set --key=PATH` ([ADR-0220](../adr/0220-a-separate-release-signing-key.md)). Install it again from your own copy |

All three live in the daemon's state directory on the `cix-config` partition
(`/config/state`; the signing and release keys under `keys/`, the PKI under
`pki/`). By default the installer **keeps** an existing `cix-config` that holds
a CA instead of formatting it (#415); `--wipe-config` formats it. See
[`installing.md`](installing.md). Do not rely on that for a disk being replaced
or repartitioned: export the PKI, and hold off-box copies of the two signing keys.

If a release key is lost anyway, artifacts it already signed stay verifiable:
[`docs/keys/`](../keys/) is append-only, so its public half stays published.
Install a new key with `release-key set` and commit its public half **alongside**
the old one, never replacing it.

The machine's Secure Boot enrollment is separate from the host's signing pair:
the installed boot chain is signed with the key the installer media was built
with. A machine that enrolled that certificate trusts media built with the same
pair; media signed with a different pair must be enrolled through MokManager on
first boot, as on a first install ([Secure Boot](installing.md#secure-boot)).

### Volume content

The backup carries volume *definitions*, never volume *content*. Volume data
lives under `/var/lib/cix/volumes/<name>` on the `cix-containers` partition,
which the installer formats. Copy it out with the workload's own export tooling.

`GET /v1/containers/{name}/files` can read content out, but it returns content
only — no mode, no ownership
([#139](https://git.home.arpa/itdlabs/cix/issues/139)) — so anything executable
or permission-sensitive does not survive that route faithfully.

---

## Procedure

### 1. Back up, export, and verify

```sh
cixctl --host=<host> backup --output=backup.json
cixctl --host=<host> pki export --out=pki.bundle
```

`pki export` asks for a passphrase (or reads it from `--passphrase-file=`).
Open `backup.json` and check that it parses and lists the fields you expect: a
zero-byte or truncated file that nobody opened is the failure this step exists
to catch.

### 2. Build installer media matching the version you are on

A reinstall from older media lands the box on that older version, which then
has to be upgraded forward. Build fresh media first:

```sh
cixctl --host=<host> iso build --wait
cixctl --host=<host> iso publish --wait
```

The build needs the `cix`, `kernel` and `isotools` hostbuild artifacts and the
Secure Boot signing pair, and fails with a 400 naming what is missing.
`iso publish` puts the ISO and its signature in the artifact cache, so the media
can be fetched from somewhere other than the host about to be wiped. The cache
refuses an ISO whose signature is not there first (409); publish uploads the
signature first.

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
cixctl --host=<host> restore --input=backup.json
cixctl --host=<host> pki import --in=pki.bundle
cixctl --host=<host> signing-keys set --key=PATH --cert=PATH
cixctl --host=<host> release-key set --key=PATH
cixctl --host=<host> reboot
```

`pki import` is refused (409) when a CA already exists, which it does when the
installer kept `cix-config`. `restore` writes the bundle's fields back to their
state files and does not hot-reload, so reboot for them to take effect. Fields
are independent and all optional, so a partial restore is legitimate: container
definitions alone, or networks alone. Every field is validated on the way in.

The `pkg_installed` field records which packages were installed into which
images; the built files are not in the bundle. After the reboot, check
`cixctl pkg ls` and the images your containers use. An install
(`pkg install`, `image materialize`) fetches an approved prebuilt artifact from
the artifact cache where one exists, and builds from source otherwise.

### 5. Re-establish trust

- Reissue any certificates that matter outside the host, if the PKI was not
  imported.
- Re-enroll Secure Boot at first boot if MokManager prompts.

---

## Verifying the result

The point of the exercise is usually a specific property, so check that one
directly rather than checking that the box is up:

```sh
curl http://<host>/v1/storage        # cix-config and cix-containers fs_type
curl http://<host>/v1/system/boot    # build_version, slot
curl http://<host>/v1/containers     # the fleet came back
```

`GET /v1/storage` reads the superblock, so it answers what the filesystem
actually is rather than what was intended.
