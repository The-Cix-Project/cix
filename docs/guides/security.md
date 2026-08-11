# Security: PKI, HTTPS, and LDAP-backed accounts

Bootstrapping and operating this platform's internal certificate authority, turning on HTTPS for the daemon itself, issuing certs to containers, and syncing real Unix/SSH accounts from LDAP-managed users. Task-oriented — field-by-field detail lives in [`docs/api/README.md`](../api/README.md); CLI flag syntax in [`cli-reference.md`](cli-reference.md).

## Bootstrapping the CA

```sh
kanxeoctl pki ca bootstrap --common-name="Kanxeo Root CA" --days=3650
```

One-shot — a second call is refused (`409`); see [Rotating the whole chain](#rotating-the-whole-chain) below for the real "start over" operation. **The CA private key is never returned over this API, ever, on any endpoint** — it's the root of trust and stays on the host. Real cryptography (keypair generation, CSR signing) runs through the system's own real, unmodified `openssl` binary as a short-lived subprocess, not a hand-rolled implementation.

Optionally add a second, intermediate tier — requires the root to already exist:

```sh
kanxeoctl pki intermediate bootstrap --common-name="Kanxeo Intermediate CA" --days=1825
```

Once bootstrapped, every future leaf certificate is signed by the intermediate instead of the root automatically, transparent to the issuing call — no separate opt-in, and every fetched cert then carries the full chain (leaf + intermediate).

## Issuing certificates

```sh
kanxeoctl pki cert create --name=svc.internal --sans=svc.internal,svc --days=365
```

`name` becomes the cert's CN and this resource's identifier — a bare name (no `.`) gets this install's own site suffix appended automatically, the same rule DNS records use. The response is the **only** place the leaf's private key is ever returned — `kanxeoctl pki cert ls`/`pki cert rm NAME` never include it again, so save it now if you're issuing by hand. `pki cert rm` deletes the key and cert files from disk, not just the index entry.

**Automatic issuance straight into a container**, instead of issuing separately and figuring out delivery:

```sh
kanxeoctl run --name=web --image=myapp --pki-issue --pki-cert-dir=/etc/kanxeo-tls --pki-days=365 -- /usr/bin/some-binary
```

Issues a cert named after the container and writes `tls.crt`/`tls.key` (mode `0600`) directly into its filesystem at creation time — one-time delivery, no live resync, since a cert doesn't change after a container starts (a chain rotation, below, explicitly redelivers). Deleting the container automatically removes its cert too. Requires the CA to already be bootstrapped.

This install also always keeps its own `"host"` leaf current, auto-reissued whenever site identity or the CA chain changes — nothing to request separately.

## Turning on HTTPS for the daemon itself

Requires a bootstrapped root CA first (`kanxeoctl pki ca bootstrap`, above) — the daemon reuses its own already-issued `"host"` leaf rather than a separate certificate:

```sh
kanxeoctl daemon-config set --enable-https
```

Starts a second, independent listener on `--https-port=` (default `8443`), live immediately. See [`networking.md`](networking.md#the-management-network) for the rest of `daemon-config`'s own contract (port, management-network repoint, a dedicated bind IP) — this is one field of that same live-reconfigurable resource, not a separate mechanism.

## Rotating the whole chain

`pki ca bootstrap`/`pki intermediate bootstrap` are deliberately one-shot and refuse a second call — this is the real, explicit "start over":

```sh
kanxeoctl pki reset --root-common-name="Kanxeo Root CA - lab.internal" --intermediate-common-name="Kanxeo Intermediate CA - lab.internal"
```

Destructive: deletes the root (and intermediate, if any) and every leaf's on-disk key/cert, re-bootstraps them fresh, then reissues every leaf that was tracked beforehand — same name/SANs/owner, a genuinely new keypair and validity period each. Any leaf owned by a still-live, auto-delivered container is automatically redelivered afterward so a running service's `tls.crt`/`tls.key` don't go stale.

## LDAP-backed accounts

Register a running LDAP server container (`glauth`, this platform's standard integrable provider — real `fsnotify` config-watching, no signal needed on every write) so the user/group CRUD below has somewhere to render into:

```sh
kanxeoctl ldap server register --container=ldap1 --config-path=/etc/glauth/glauth.cfg
```

Kanxeo itself stays the source of truth for every user and group — a create/update/delete re-renders the full current set into every currently-registered, currently-running server's config file, preserving everything above the managed section (the operator's own backend/TLS settings) byte-for-byte.

```sh
kanxeoctl ldap group add --name=superheros --gidnumber=5501
kanxeoctl ldap user add --name=j_doe --primarygroup=5501 --mail=j.doe@kanxeo.internal --password=dogood
```

`uidnumber`/`gidnumber` are optional — auto-allocated from a configurable floor (`kanxeoctl ldap config show`/`set --start-uid=N --start-gid=N`, default `10000`, changing it only affects future allocations). A password, if given, is SHA-256-hashed on the daemon side — never stored or returned in plaintext, only a `has_password` boolean is exposed.

### Real SSH accounts on a jump box

This project has no real LDAP-protocol NSS/PAM stack (`openssh.recipe` was deliberately built without PAM). Instead, registering a container as an SSH target renders real `/etc/passwd`/`/etc/group`/`/etc/shadow` entries plus each user's own `~/.ssh/authorized_keys` directly onto its filesystem — sshd itself never talks to LDAP, it just reads ordinary account files kept in sync on every LDAP user/group change:

```sh
kanxeoctl ldap ssh-target register --container=jumpbox1
kanxeoctl ldap user add --name=j_doe --primarygroup=5501 --ssh-key="ssh-ed25519 AAAA..." --password=dogood
```

Only a user with a non-empty `--ssh-key=` and not `--disabled` gets a rendered account; everyone else stays an ordinary directory entry with no shell access. `kanxeoctl ldap ssh-target unregister CONTAINER` does not touch already-rendered accounts on that container's own filesystem — it only stops future syncs.

### Service accounts for containers themselves

A container can provision its own LDAP bind account at creation time, separate from the human accounts above:

```sh
kanxeoctl run --name=svc1 --image=myapp --ldap-provision --ldap-group=svcaccts -- /usr/bin/some-binary
```

Delivers a freshly-generated `bind.secret` (never persisted in plaintext anywhere in Kanxeo's own state — only its hash survives) into `/etc/kanxeo-ldap/` inside the container by default. `--ldap-group=` must already exist; `--ldap-user=` defaults to the container's own name. The account is removed automatically when the container is.
