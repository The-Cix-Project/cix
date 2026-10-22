# 0350 — A host trusts its operator's CA by registration

## Status

Proposed, 2026-10-22, for #591. The owner chose the approach on 2026-10-20 (option 1 on #591: an operator-registered trust anchor, on the `pkg upstream-keys` precedent) and asked for an ADR and a price before any work. The owner has two decisions to make: accept or amend this, and choose where the combined bundle takes effect (rule 4: at assembly only, as #591 worded it, or also at runtime, as this ADR recommends).

## Context

Read from the code at `78cd9145`.

**The control-plane root's trust store is the last file still copied off the build host.** `mkbootroot` copies `/etc/ssl/certs/ca-certificates.crt` from whatever machine runs it (`image/src/mkbootroot.c`, around line 1447). After ADR-0337 every other program and library in the root comes from the `cix-hosttools` image, and on an installed host the build host is the previous root. So the trust store every Cix host verifies TLS against is whatever a Debian dev machine put there once, carried forward release after release. Nothing records what is in it.

**The obvious replacement breaks the box.** `ca-certificates@2026.09.03-2` pins Mozilla's root program as curl.se publishes it, and `cix-hosttools` 2.4.4 already installs it. But the owner's forge is served by a private CA (`curl -k -v https://git.home.arpa/` names the issuer `gibnet Intermediate CA`, O=itdlabs). No public root program contains it. The file copied today works because the build host's copy has the site's CA added to it. Replacing it with public roots alone would break `pkg recipe commit`, recipe sync and every HTTPS source on the forge, which is the box's ability to fetch its own next fix. 0.2.57-481 broke exactly that, for a different reason, in the same place.

**One consumer in cixd.** Every HTTPS fetch cixd makes goes through `curlfetch_perform()` (`daemon/src/curlfetch.c`), the only place a libcurl handle is created. #410 removed every forked `curl`, so nothing else reads the bundle path. The handle sets no `CURLOPT_CAINFO`, so libcurl uses its compiled-in default, which is this one file. The LDAP client is not a consumer: `ldap_tls_start()` verifies against an explicit CA it is handed.

**What carries over from the upstream-keys precedent (ADR-0318, ADR-0326).** An anchor is added by an operator, persisted, identified by a fingerprint of its own bytes, never fetched, never trusted on first use, and listed through the API. What does not carry over: per-package scoping (a TLS anchor answers for every fetch), and supply through the signed catalogue. The catalogue cannot carry it, because fetching the catalogue is what needs the anchor.

## Decision

**A host trusts public roots from the `ca-certificates` package plus the CAs its operator registers, and nothing else. The registered set is a store, listed and changed through the API.**

1. **A trust-anchor store**, persisted in the state directory on the config partition, so it survives updates and reinstalls that keep `/config`:
   - `GET /v1/system/trust-anchors` lists each anchor with its SHA-256 fingerprint (of the DER), subject, issuer, `not_after` and when it was added.
   - `POST /v1/system/trust-anchors` with `{"pem": "..."}` adds one.
   - `DELETE /v1/system/trust-anchors/{fingerprint}` removes one.
   - The permissions are `system:read` and `system:write`, as for the signing keys. `pki:*` governs the platform's own issuing CA, which is a different thing.
   - `cixctl trust-anchors ls|add --file=PEM|rm FINGERPRINT`, and a table with an add form on the dashboard.

2. **What the store refuses.** The PEM must be exactly one X.509 certificate with `basicConstraints CA:TRUE`. A private key, an expired certificate, a duplicate and anything past a fixed count (16) are refused with the reason. Nothing fetches a certificate: the operator supplies the bytes. Inspection reuses `pkicrypto.c`'s certificate reading and fingerprinting.

3. **The public half comes from the package.** `mkbootroot` stages `/etc/ssl/certs/ca-certificates.crt` from the `ca-certificates` package in `cix-hosttools`, as ADR-0337 does every other root file, and no longer copies the build host's.

4. **Where the combined bundle takes effect: the owner's choice.**
   - **(a) At assembly only, as #591 was worded.** Each assembly writes public roots plus the registered anchors into the root's bundle. An anchor added or removed takes effect after the next assembly and reboot. If a root boots without an anchor it needs, it cannot fetch its own fix; recovery is `boot-next` to the previous slot, then registering the anchor, then a reassembly.
   - **(b) At assembly and at runtime, recommended.** As (a), and cixd also writes the combined bundle into the state directory whenever the store changes and at start, and `curlfetch_perform()` hands it to libcurl as `CURLOPT_CAINFO`. An anchor takes effect for cixd's next fetch, with no reassembly and no reboot. If a root boots without an anchor it needs, recovery is one local `cixctl trust-anchors add`, which needs no fetch. The assembly-time copy still matters for anything else in the root that verifies TLS against the default path.
   - The property that separates them is recovery: with (b), a missing anchor can always be repaired from the box itself.

5. **The cut-over runs in this order, and step 3 never ships before step 2 is measured:**
   1. Release one ships the store, the endpoints, cixctl, the dashboard and, under (b), `CURLOPT_CAINFO`, with the root's bundle still copied as today. Nothing changes for a host that registers nothing.
   2. The owner registers the site CA on 192.168.15.95, and `GET /v1/system/trust-anchors` shows it.
   3. Release two switches `mkbootroot` to the package bundle plus the registered anchors. Verified by `pkg recipe commit` and a recipe sync against the forge on the new root, not by the root starting.
   - If step 3 shipped before step 2: under (a) the box could not fetch from the forge until rolled back; under (b) one `cixctl trust-anchors add` repairs it.

6. **Out of scope, and unchanged:** the container-side `cix-ca-bundle.pem` written by `pki_write_trust_bundle_file()`; the LDAP client's explicit CA; and whether the platform's own CA should be an implicit anchor. Each is its own decision.

## Consequences

- **A fresh install trusts public roots only**, so the default sources (the public catalogue on GitHub, `cache.cix.world`) work at first boot. An operator with a private CA registers it once. On .95 the forge is the only private-CA endpoint; the LAN cache is plain HTTP.
- **The trust set is declared and visible.** Today nothing records what the root trusts. After this, it is the package's version plus a list the API shows.
- **The last build-host file leaves the root**, completing ADR-0337 for the control plane.
- **Two releases**, because the order in rule 5 cannot be collapsed without risking the forge.
