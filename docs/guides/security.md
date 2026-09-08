# Security: PKI, HTTPS, and LDAP-backed accounts

Bootstrapping and operating this platform's internal certificate authority, turning on HTTPS for the daemon itself, issuing certs to containers, and syncing real Unix/SSH accounts from LDAP-managed users. Task-oriented — field-by-field detail lives in [`docs/api/README.md`](../api/README.md); CLI flag syntax in [`cli-reference.md`](cli-reference.md).

## Bootstrapping the CA

```sh
cixctl pki ca bootstrap --common-name="Cix Root CA" --days=3650
```

One-shot — a second call is refused (`409`); see [Rotating the whole chain](#rotating-the-whole-chain) below for the real "start over" operation. **The CA private key is never returned over this API, ever, on any endpoint** — it's the root of trust and stays on the host. Real cryptography (keypair generation, CSR signing) runs through the system's own real, unmodified `openssl` binary as a short-lived subprocess, not a hand-rolled implementation.

Optionally add a second, intermediate tier — requires the root to already exist:

```sh
cixctl pki intermediate bootstrap --common-name="Cix Intermediate CA" --days=1825
```

Once bootstrapped, every future leaf certificate is signed by the intermediate instead of the root automatically, transparent to the issuing call — no separate opt-in, and every fetched cert then carries the full chain (leaf + intermediate).

## Issuing certificates

```sh
cixctl pki cert create --name=svc.internal --sans=svc.internal,svc --days=365
```

`name` becomes the cert's CN and this resource's identifier — a bare name (no `.`) gets this install's own site suffix appended automatically, the same rule DNS records use. The response is the **only** place the leaf's private key is ever returned — `cixctl pki cert ls`/`pki cert rm NAME` never include it again, so save it now if you're issuing by hand. `pki cert rm` deletes the key and cert files from disk, not just the index entry.

**Automatic issuance straight into a container**, instead of issuing separately and figuring out delivery:

```sh
cixctl container run --name=web --image=myapp --pki-issue --pki-cert-dir=/etc/cix-tls --pki-days=365--service=main=/usr/bin/some-binary
```

Issues a cert named after the container and writes `tls.crt`/`tls.key` (mode `0600`) directly into its filesystem at creation time — one-time delivery, no live resync, since a cert doesn't change after a container starts (a chain rotation, below, explicitly redelivers). Deleting the container automatically removes its cert too. Requires the CA to already be bootstrapped.

This install also always keeps its own `"host"` leaf current, auto-reissued whenever site identity or the CA chain changes — nothing to request separately.

## Turning on HTTPS for the daemon itself

Requires a bootstrapped root CA first (`cixctl pki ca bootstrap`, above) — the daemon reuses its own already-issued `"host"` leaf rather than a separate certificate:

```sh
cixctl daemon-config set --enable-https
```

Starts a second, independent listener on `--https-port=` (default `443`), live immediately. See [`networking.md`](networking.md#the-management-network) for the rest of `daemon-config`'s own contract (port, management-network repoint, a dedicated bind IP) — this is one field of that same live-reconfigurable resource, not a separate mechanism.

## Trusting the CA on your own device

This install's root CA is private and self-signed — nothing trusts it by default, so a browser or OS hitting `https://<box>:<https-port>/` (the web dashboard, or a direct API call) shows a certificate warning until you trust it once, on each device you connect from. This is expected, not a bug: it's the same reason `curl` needs `--cacert` for a self-signed endpoint. **Found live**: an untrusted browser doesn't just show a warning once — every poll the dashboard's own JavaScript makes (every 2 seconds, and this daemon never does HTTP keep-alive, so each one is a fresh TLS handshake) fails the same way, which can flood `GET /system/logs` and the physical console with `https handshake failed` warnings fast enough to crowd out everything else (ADR-0134 rate-limits the logging itself, but trusting the cert is what actually stops the failures).

**Get the certificate**: web dashboard's System > PKI > Root CA page has a "Download certificate (.crt)" button once the CA is bootstrapped — or fetch it directly:

```sh
cixctl pki ca show --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["cert_pem"])' > cix-root-ca.crt
```

Trusting the **root** is enough even if you've also bootstrapped an intermediate CA (System > PKI > Intermediate CA has its own download button too, but you don't need to separately trust it) — the HTTPS listener sends the full leaf+intermediate chain on every handshake (ADR-0136), so a client that trusts only the root can validate the whole path.

**Trust it** — steps differ by platform, since there's no single OS-wide trust store:

- **Windows**: double-click the downloaded `.crt` file → **Install Certificate** → **Local Machine** (needs admin, trusts it for every user) or **Current User** → **Place all certificates in the following store** → **Trusted Root Certification Authorities**.
- **macOS**: open **Keychain Access** → **File > Import Items** → select the file (imports to the login keychain by default) → find it in the list, double-click it → expand **Trust** → set **When using this certificate** to **Always Trust** → close (prompts for your password).
- **Linux, system-wide** (curl, most non-browser tools): `sudo cp cix-root-ca.crt /usr/local/share/ca-certificates/ && sudo update-ca-certificates` (Debian/Ubuntu); `sudo cp cix-root-ca.crt /etc/pki/ca-trust/source/anchors/ && sudo update-ca-trust` (Fedora/RHEL).
- **Firefox** (any OS — it keeps its own trust store, independent of the OS one above): **Settings > Privacy & Security > Certificates > View Certificates > Authorities tab > Import** → select the file → check **Trust this CA to identify websites**.
- **Chrome/Edge**: uses the OS-level trust store on Windows/macOS (the steps above cover it) and, on Linux, typically the same NSS database Firefox uses — the Linux system-wide step above is usually enough, but if it still isn't trusted, import it the same way as the Firefox step, into Chrome's own **Settings > Privacy and security > Security > Manage certificates**.

**If a browser still shows a warning after trusting the root**, and you're connecting by bare IP address (`https://192.168.x.x/`) rather than a hostname: the daemon's own auto-issued `"host"` leaf certificate only carries this install's DNS FQDN as its Subject Alternative Name (`GET /pki/certs/host`'s own `sans` field), not the raw IP — a browser doing strict hostname verification will flag that as a *different* warning (hostname mismatch, not "untrusted") even with the chain fully trusted. Reach the box by its FQDN instead (whatever your own DNS setup resolves it through), or accept the mismatch warning if IP access is what you need — this endpoint doesn't currently issue IP-SAN certificates.

Once trusted, no further action is needed — the same cert (or its successor after a [chain rotation](#rotating-the-whole-chain), which requires re-trusting) is presented on every future connection to this install.

## Rotating the whole chain

`pki ca bootstrap`/`pki intermediate bootstrap` are deliberately one-shot and refuse a second call — this is the real, explicit "start over":

```sh
cixctl pki reset --root-common-name="Cix Root CA - lab.internal" --intermediate-common-name="Cix Intermediate CA - lab.internal"
```

Destructive: deletes the root (and intermediate, if any) and every leaf's on-disk key/cert, re-bootstraps them fresh, then reissues every leaf that was tracked beforehand — same name/SANs/owner, a genuinely new keypair and validity period each. Any leaf owned by a still-live, auto-delivered container is automatically redelivered afterward so a running service's `tls.crt`/`tls.key` don't go stale.

## LDAP-backed accounts

Register a running LDAP server container (`glauth`, this platform's standard integrable provider — real `fsnotify` config-watching, no signal needed on every write) so the user/group CRUD below has somewhere to render into:

```sh
cixctl ldap server register --container=ldap1 --config-path=/etc/glauth/glauth.cfg
```

Cix itself stays the source of truth for every user and group — a create/update/delete re-renders the full current set into every currently-registered, currently-running server's config file, preserving everything above the managed section (the operator's own backend/TLS settings) byte-for-byte.

```sh
cixctl ldap group add --name=superheros --gidnumber=5501
cixctl ldap user add --name=j_doe --primarygroup=5501 --mail=j.doe@cix.internal --password=dogood
```

`uidnumber`/`gidnumber` are optional — auto-allocated from a configurable floor (`cixctl ldap config show`/`set --start-uid=N --start-gid=N`, default `10000`, changing it only affects future allocations). A password, if given, is bcrypt-hashed on the daemon side (ADR-0144) — never stored or returned in plaintext, only a `has_password` boolean is exposed.

### Real SSH accounts on a jump box (ADR-0144 task #838)

`sshd` queries LDAP live at connection time — real password auth via `pam_ldap.so`'s own bind-as-user check, real pubkey auth via a live `AuthorizedKeysCommand` `ldapsearch` for the user's `sshPublicKey` attribute. Not a `cixctl` one-liner: the target container needs `openssh` built `--with-pam` plus `linux-pam`/`nss-pam-ldapd` already installed, and real per-container config (`/etc/nslcd.conf`, `/etc/nsswitch.conf`, `/etc/pam.d/sshd`, `UsePAM yes`/`AuthorizedKeysCommand` in `sshd_config`) staged via `files[]` at creation time — see [`docs/api/README.md`'s "Real, live-LDAP SSH login"](../api/README.md#real-live-ldap-ssh-login-adr-0144-task-838) for the full field-by-field setup, and [ADR-0144](../adr/0144-host-authentication-and-real-ldap.md)/[ADR-0145](../adr/0145-retire-file-rendered-ssh-target-sync.md) for why it's built this way.

**Which users may log into which container is a per-container decision** (issue #76). A container created with `ldap_client` accepts every account in the directory unless you say otherwise; `--ldap-allow-group=` narrows it to named groups, enforced by `nslcd` itself via a `pam_authz_search` in the staged `/etc/nslcd.conf`:

```sh
cixctl container run --name=jumpbox1 --image=jumpbox \
  --ldap-client --ldap-allow-group=jumpusers --ldap-allow-group=admins \
  --service=sshd=/usr/sbin/sshd -D
```

Every named group must already exist, or creation is refused naming the offending one — a typo there would otherwise render a filter matching nobody and lock the container out completely. See [`docs/api/README.md`'s "Who may log in here"](../api/README.md#who-may-log-in-here-ldap_allow_groups-issue-76) for the rendered filter and the full rule set.

`nslcd` and the `AuthorizedKeysCommand` script both need a real bind identity with search capability — grant one via `--can-search` on a dedicated service account, never a human login account:

```sh
cixctl ldap group add --name=service-accounts --gidnumber=10001
cixctl ldap user add --name=svc-nslcd --primarygroup=10001 --password=<a-real-secret> --can-search
cixctl ldap user add --name=j_doe --primarygroup=5501 --ssh-key="ssh-ed25519 AAAA..." --password=dogood --loginshell=/usr/bin/bash
```

`--ssh-key=` is rendered as glauth's own real `sshkeys` LDAP attribute, queried live rather than copied to a file. `--loginshell=` matters here in a way it didn't before: an empty one renders as glauth's own default, which doesn't resolve on this project's own minimal images (`/usr/bin/bash` is the real path, not `/bin/bash`) — `sshd` rejects the login outright if it can't find the configured shell.

Two more real, non-obvious gotchas confirmed live re-provisioning this from scratch (both closed, neither is a hack -- both are the standard, documented fix for the class of problem they are): `nslcd.conf` needs `pam_authc_ppolicy no` -- glauth's `config` backend doesn't recognize the LDAP password-policy control `nslcd` requests by default, and returns a spurious "Invalid credentials" rather than ignoring the unsupported control gracefully; and `openssh` needs `recipes/package/openssh/10.4p1-8/build.sh` specifically, not `-7` -- with `UsePAM yes`, the actual PAM conversation runs inside `sshd`'s own privsep pre-auth child, which `chroot()`s to `--with-privsep-path` before that conversation ever happens, so `pam_ldap.so`'s attempt to reach `nslcd`'s local socket fails unless that chroot target lives on the same filesystem the socket does (`-8` moves it from `/var/empty` to `/run/sshd-empty`, alongside `nslcd`'s own `/run/nslcd/socket`, and the container's own startup command hard-links the socket into the chroot once `nslcd` has bound it -- see `docs/api/README.md`'s own worked example for the exact sequencing).

### Service accounts for containers themselves

A container can provision its own LDAP bind account at creation time, separate from the human accounts above:

```sh
cixctl container run --name=svc1 --image=myapp --ldap-provision --ldap-group=svcaccts--service=main=/usr/bin/some-binary
```

Delivers a freshly-generated `bind.secret` (never persisted in plaintext anywhere in Cix's own state — only its hash survives) into `/etc/cix-ldap/` inside the container by default. `--ldap-group=` must already exist; `--ldap-user=` defaults to the container's own name. The account is removed automatically when the container is.

### Break-glass recovery (ADR-0146)

Host-auth write-gating (`GET`/`PUT /system/hostauth-config`, [`docs/api/README.md`'s "Host authentication"](../api/README.md#host-authentication-adr-0144)) has no in-band bypass once active, by design — nothing reachable over the REST API can turn it off from the outside. If every login genuinely stops working (every configured admin-group user's credential rejected, or the LDAP backend serving stale/empty config after a reboot — see [ADR-0146](../adr/0146-ldap-startup-resync-and-break-glass-recovery.md) for the real incident that motivated this tool), the only way back in is physical or hypervisor console access to the machine itself:

1. Attach the same installer ISO used to originally install this system (`docs/guides/installing.md`) as boot media, and force a reboot.
2. At the GRUB menu, select **"Cix Recovery"** instead of the normal install entry.
3. The tool mounts the already-installed system's own containers partition, shows the current host-auth config for confirmation, and requires typing `RESET` (all capitals) before changing anything.
4. Confirming resets **only** `admin_groups` back to empty — the same state a fresh install starts in, where every write is open with no login required. Every other setting (LDAP backend config, session idle timeout, every container, every LDAP user/group record) is left completely untouched.
5. Remove the recovery media and reboot into the normal installed system. Every API write is open again — reconfigure a real admin group (`cixctl hostauth-config set --admin-group=...`) before anyone relies on gating again.

This is deliberately **not** a network-reachable escape hatch: reaching this tool at all requires the same level of access needed to attach different boot media and power-cycle the machine, which a remote attacker manipulating the REST API alone can never do. The typed confirmation is a second, independent gate on top of that physical-access requirement — a stray or accidental boot into this entry can't silently disable write-gating.

## Verifying a downloaded installer ISO

An installer ISO is the one artifact with no host on the far side to
check it — that is the whole reason it is stored and served rather than
composed locally. So it is signed, and the check happens on a machine
you already trust, before the stick is written:

```
minisign -Vm cix-installer-<version>-<release>-<arch>.iso -p cix-release.pub
```

Use **stock `minisign`**, not a Cix tool: an installer verifying its own
signature is the code being checked doing the checking, and a
substituted ISO either reports success or never implements the check at
all. A stranger with no Cix software must be able to tell a genuine
installer from a fabricated one — the last step deliberately is not
ours.

The key is committed at [`docs/keys/cix-release.pub`](../keys/cix-release.pub);
pin a copy once rather than re-fetching it, and see
[`docs/keys/README.md`](../keys/README.md) for why it lives in git and
not beside the ISO.

**This is a different key from the Secure Boot pair above.** That one is
RSA, because UEFI requires it, and it decides whether firmware will boot
an image. This one is Ed25519, because minisign requires it, and it
tells a downloader the bytes really came from us. One key doing both
jobs would mean whoever can sign a download can also sign a bootloader,
and the recovery costs are nothing alike: republishing a public key,
versus re-enrolling firmware on every host in the fleet
([ADR-0220](../adr/0220-a-separate-release-signing-key.md)).
