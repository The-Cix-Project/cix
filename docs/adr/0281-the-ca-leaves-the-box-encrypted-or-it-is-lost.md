# 0281 — The CA leaves the box encrypted, or it is lost

## Status

Accepted

Issue [#415](https://git.home.arpa/itdlabs/cix/issues/415). **Narrowly supersedes** one half of [ADR-0033](0033-platform-state-backup-restore.md) — the rule it cites from `docs/api/README.md`, "the CA private key is never returned over the API, in any endpoint, ever". It is now returned by exactly one endpoint, only encrypted under an operator passphrase; every other endpoint is unchanged. ADR-0033's *other* half — that the PKI stays out of `/system/backup` — is kept, not reversed. Raised in cost by [ADR-0280](0280-a-certificates-identity-and-its-lifetime-are-separate-concerns.md), which made a certificate a durable identity.

## Context

PKI state lives at `/config/state/pki`, on the `cix-config` partition. That is a deliberate placement and a good one: the CA survives a reboot, an A/B slot update and a rolling rebuild, because none of those touch that partition.

`cix-install.c` formats it:

```c
	if (mkfs_btrfs(config_dev, "cix-config", 0) != 0)
		return 1;
```

So a reinstall destroyed the root CA, the intermediate, and every leaf under them. Nothing warned about it and nothing could carry one across.

**ADR-0033 had already decided this, deliberately and with the owner.** It is worth quoting, because this ADR is not correcting carelessness:

> PKI is not referenced by this endpoint at all, confirmed explicitly with the user rather than assumed. ... Three options were considered directly with the user: exclude PKI entirely from this mechanism (chosen), add an explicit opt-in flag that deliberately breaks that rule when asked, or accept re-bootstrapping the CA on every restore. Excluding it entirely keeps the existing "never leaves via API" guarantee absolute ... PKI backup is a genuinely separate, host-level concern (`/var/lib/cix/pki/`, backed up directly, outside the API), not partially folded in here.

The option this ADR now builds is recognisably the second of those three, and it was considered and declined on the merits. So the question is not whether that reasoning was sloppy. It was not. The question is whether its premise still holds.

It does not, and for a reason nobody connected at the time. That decision rests on a stated alternative — the PKI directory "backed up directly, outside the API" — and **two later, unrelated changes removed it**:

- The path moved. `/var/lib/cix/pki/` became `/config/state/pki` when state relocated to the config partition (#250). The directory ADR-0033 named no longer exists.
- More decisively, **there is nowhere to back it up *from*.** The host is shell-less and API-only by charter: no SSH to the host, no shell, no file access outside the REST surface. "Backed up directly, outside the API" names an operation an operator cannot perform on this platform. It may have read as ordinary when written; it is not available now.

So the guarantee was kept absolute, and the alternative it depended on quietly went away. What remained was not a separation of concerns but a dropped one — and the half of ADR-0033 that is genuinely right (secrets do not belong in the config bundle) was doing the work of justifying both.

That is not hypothetical. The release signing key was lost to a reinstall on 2026-09-06 — a key in precisely this position, protected by precisely this reasoning.

ADR-0280 then raised the cost. A certificate is now a *durable identity*: `pki_cert` exists so an SSH host key outlives the containers presenting it. An identity that survives every rolling rebuild and then changes on a reinstall is only partly fixed.

The third option ADR-0033 lists — "accept re-bootstrapping the CA on every restore" — is what the platform has been doing by default, and it is what #415 was filed about.

## Decision

**The whole PKI store can be exported off the box, encrypted under an operator passphrase, and imported back.** Two endpoints, `POST /v1/pki/export` and `POST /v1/pki/import`, separate from `/system/backup`.

And **the installer keeps a CA it finds rather than formatting over it**, announcing that it did.

### The supersession, stated plainly

"The CA private key is never returned over the API, in any endpoint, ever" becomes: *the CA private key is returned by `POST /v1/pki/export` alone, only encrypted under a passphrase the daemon never stores, and by no other endpoint under any circumstances.*

The original rule protected against the key becoming *casually* available — in a `GET` run while screen-sharing, in a config file, in a bundle handed to a colleague. An encrypted bundle behind a deliberate, passphrase-taking `POST` is not casually available, which is why this supersession is narrow rather than an abandonment.

What the rule could not protect against, and did not admit, was the key being *unrecoverable*. ADR-0033 weighed "absolute guarantee" against "opt-in flag that breaks it when asked" and chose the guarantee — correctly, given that it believed a direct filesystem backup existed. With that alternative gone, the same trade reads differently: the choice is no longer "absolute guarantee vs. a carve-out", it is "absolute guarantee vs. being able to keep your CA at all".

A related claim in ADR-0033's own wording was simply false and is corrected rather than superseded: it said "the CA private key (and every issued leaf's own key) is never returned over the API". A leaf's private key **is** returned, exactly once, by the call that issues it — `POST /pki/certs` and `POST /pki/reset` both do it, and both document it as "returned exactly once, right now". Only the CA's and the intermediate's keys were ever withheld.

### Why the whole store, not just the CA

A leaf is only re-derivable if something reissues it, and the certificates ADR-0280 exists for are exactly the ones nothing reissues. An unowned cert is a durable identity; restoring only the CA would rotate the very SSH host key that decision was made to stop rotating.

Owned leaves are carried too, and that is what makes a restore hold for `pki_issue` containers: when autostart recreates one, issuance finds the restored certificate already present and keeps it.

### Why two mechanisms rather than one

Folding the PKI into `/system/backup` under a flag was considered and rejected. It would make every existing consumer of that endpoint — including the dashboard's download button — a secret-handling path, and a bundle that *sometimes* carries the trust root is easy to mishandle precisely because it usually does not.

The cost of two is that an operator can forget one. That is addressed directly rather than by merging: `GET /system/backup` now reports `"pki_included": false`, so the bundle names what it does not carry. A backup that silently omits the trust root reads as complete, and the operator finds out at the reinstall they needed it for.

### Why import refuses when a CA exists

`409`, matching `POST /pki/ca`'s one-shot posture. Replacing a live trust root would invalidate every certificate the install has issued, and nothing about "import a backup" says an operator meant that.

This leaves a real gap, stated rather than hidden: `pki reset` regenerates rather than deletes, so **there is no path from a bootstrapped CA back to an imported one.** An install that needs a restore must import before anything bootstraps. A replace mode is deliberately not built here — it is a destructive operation that deserves its own decision.

### Cryptographic choices, and why each is explicit

`-aes-256-cbc -pbkdf2 -iter 600000 -md sha256 -salt`. Every parameter is stated on both sides rather than left to a default, because `openssl enc` defaults have moved across versions and **a bundle that a later release cannot decrypt is not a backup**. The parameters are also reported in the export response, so a bundle can be opened by hand with a plain `openssl` if this platform is unavailable — which is exactly the situation a disaster-recovery artifact has to survive.

CBC rather than an AEAD mode because `openssl enc` refuses AEAD outright. That leaves the bundle without an integrity tag, so a wrong passphrase does not fail cleanly — it decrypts to garbage. Import therefore **validates what it decoded rather than trusting that it decoded**: the blob must parse, carry a known version, and its CA key and certificate must be a matching pair, checked by deriving a public key from each and comparing. No corrupted input produces that by accident.

The passphrase reaches `openssl` through a `0600` file, never `argv` — `-pass pass:<secret>` is readable in `/proc/<pid>/cmdline` for as long as the child lives. `cixctl` will not accept it on its own command line either, for the same reason; it prompts, or reads a file.

### Ordering, not staging

Import writes leaves first, then the intermediate, then `ca.crt`, and the **CA private key last**. `pki_ca_bootstrapped()` is true only once both `ca.crt` and `ca.key` exist, so a failure at any point leaves an install that still reads as not-bootstrapped — which means the `409` guard still lets a corrected retry through, rather than locking an operator out of their own import over a half-written store. That property is what makes a staging directory unnecessary.

The in-memory index is reloaded from the file just written, not from what the import happened to build. `pki_ca_bootstrapped()` is `stat`-based and flips the instant the files land, so a stale index would make `GET /pki/certs` answer "empty" on an install that had just restored a dozen — and it would look like a *successful* import, which is the dangerous shape.

### The installer, and why preserving is safe to default on

`cix-install` mounts `cix-config` **read-only**, stats `/state/pki/ca.key`, and if it finds one keeps the partition. A mount failure means "format it", which is the correct answer for both a fresh disk and a partition whose geometry moved — so the safe answer and the common answer are the same one.

It is announced on the console, every time. That distinction is the whole reason this is an acceptable default: an installer that *silently* preserves state is a hazard of its own kind, because the operator cannot tell afterwards which install they are looking at. `--wipe-config` forces the format, and the destructive direction is the one that has to be said out loud.

This covers the operator who never exported. It is **not** a substitute for an export, and the guide says so: it only helps when the disk is intact and the layout unchanged. An export survives a dead disk and a move to new hardware; preserving a partition cannot.

## Consequences

- An install can be rebuilt with its trust root intact, which is what "a central certificate store, consistent across reinstalls" actually requires.
- The passphrase is unrecoverable by design. Losing it loses the bundle, and `cixctl pki export` therefore asks for it twice when typed — a mistyped passphrase produces an artifact nobody can open, discovered at the worst possible moment.
- An exported bundle is a high-value secret at rest. `cixctl` writes it `0600` and says to keep it off the box, but it is now an object an operator has to look after, which was not true before.
- The passphrase travels in a request body, so these calls want HTTPS. Documented in the guide and in the contract.
- Import does not restage containers holding certificates from the previous store; they are recreated or the host is rebooted.
- `/system/backup` gained `pki_included: false`, a field whose only job is to name an absence.
