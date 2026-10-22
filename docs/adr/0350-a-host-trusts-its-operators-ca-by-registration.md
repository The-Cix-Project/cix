# 0350 — A host trusts its operator's CA by registration

## Status

Proposed, 2026-10-22, for #591. The owner chose the approach on 2026-10-20 (option 1 on #591: an operator-registered trust anchor, on the `pkg upstream-keys` precedent) and asked for an ADR and a price before any work. On 2026-10-22 the owner chose that the combined bundle takes effect at runtime as well as at assembly, and that the owner registers the site CA. A review of the first draft then found that writing anchors into the root would put the owner's private CA into the published installer ISO (rule 4), so this text narrows that choice to runtime only and asks for acceptance again.

## Context

Read from the code at `78cd9145`.

**The control-plane root's trust store is the last file still copied off the build host.** `mkbootroot` copies `/etc/ssl/certs/ca-certificates.crt` from whatever machine runs it (`image/src/mkbootroot.c`, around line 1447). After ADR-0337 every other program and library in the root comes from the `cix-hosttools` image, and on an installed host the build host is the previous root. So the trust store every Cix host verifies TLS against is whatever a Debian dev machine put there once, carried forward release after release. Nothing records what is in it.

**The obvious replacement breaks the box.** `ca-certificates@2026.09.03-2` pins Mozilla's root program as curl.se publishes it, and `cix-hosttools` 2.4.4 already installs it. But the owner's forge is served by a private CA (`curl -k -v https://git.home.arpa/` names the issuer `gibnet Intermediate CA`, O=itdlabs). No public root program contains it. The file copied today works because the build host's copy has the site's CA added to it. Replacing it with public roots alone would break `pkg recipe commit`, recipe sync and every HTTPS source on the forge, which is the box's ability to fetch its own next fix. 0.2.57-481 broke exactly that, for a different reason, in the same place.

**One TLS verifier in the root.** Every HTTPS fetch cixd makes goes through `curlfetch_perform()` (`daemon/src/curlfetch.c`), the only place a libcurl handle is created. #410 removed every forked `curl`. The handle sets no `CURLOPT_CAINFO`, so libcurl uses its compiled-in default, which `curl@8.21.0-7` sets to this file (`--with-ca-bundle`, cix#589). Nothing else in the root verifies a TLS peer against that path: `cixctl`'s client (`client/src/httpclient.c`) speaks plain HTTP, the other programs `include/controlplane_programs.h` stages (`cbs`, the squashfs, ext4 and btrfs tools, `sfdisk`, `modprobe`, `modinfo`) make no TLS connection, and the LDAP client (`ldap_tls_start()`) verifies against an explicit CA it is handed.

**The root is published.** The squashfs `mkbootroot` assembles is the one `mkinstalleriso` embeds, and the installer ISO is published to the artifact caches, `cache.cix.world` included. Anything written into the root reaches every host installed from it.

**What carries over from the upstream-keys precedent (ADR-0318, ADR-0326).** An anchor is added by an operator, persisted, identified by a fingerprint of its own bytes, never fetched, never trusted on first use, and listed through the API. What does not carry over: per-package scoping (a TLS anchor answers for every fetch), and supply through the signed catalogue. The catalogue cannot carry it, because fetching the catalogue is what needs the anchor.

## Decision

**A host's root trusts public roots from the `ca-certificates` package and nothing else. cixd trusts those plus the CAs its operator registers, which are a store listed and changed through the API and never written into the root.**

1. **A trust-anchor store**, persisted in the state directory on the config partition, so it survives updates:
   - `GET /v1/system/trust-anchors` lists each anchor with its fingerprint, subject, issuer, `not_after` and when it was added.
   - `POST /v1/system/trust-anchors` with `{"pem": "..."}` adds one.
   - `DELETE /v1/system/trust-anchors/{fingerprint}` removes one.
   - The fingerprint is the SHA-256 of the certificate's DER as 64 lowercase hex characters, with no separators, so the path parameter is a plain character class.
   - The permissions are `system:read` and `system:write`, as for the signing keys. `pki:*` governs the platform's own issuing CA, which is a different thing.
   - `cixctl trust-anchors ls|add --file=PEM|rm FINGERPRINT`, and a table with an add form on the dashboard.

2. **What the store refuses.** The PEM must be exactly one X.509 certificate with `basicConstraints CA:TRUE`. A private key, an expired certificate, a duplicate and anything past a fixed count (16) are refused with the reason. Nothing fetches a certificate: the operator supplies the bytes. Inspection reuses `pkicrypto.c`'s certificate reading and fingerprinting.
   - **A root is what to register.** An intermediate is accepted too, because libcurl on OpenSSL accepts a partial chain unless `CURLSSLOPT_NO_PARTIALCHAIN` is set, and cixd does not set it. Release one's verification measures this against the forge rather than relying on the documentation.

3. **The root's bundle is the package's.** `mkbootroot` stages `/etc/ssl/certs/ca-certificates.crt` from the `ca-certificates` package in `cix-hosttools`, as ADR-0337 does every other root file, and no longer copies the build host's. No anchor is ever written into the root, so the installer ISO carries public roots only.

4. **The combined bundle lives in the state directory, and cixd's one handle uses it.** At start and whenever the store changes, cixd writes the root's public bundle followed by every registered anchor to one file in the state directory, by write-and-rename. `curlfetch_perform()` hands that file to libcurl as `CURLOPT_CAINFO`. An anchor takes effect for the next fetch, with no reassembly and no reboot.
   - **Missing file:** if the combined bundle cannot be written or is absent at a fetch, cixd logs it once and the handle falls back to libcurl's default, the root's public bundle. Start never depends on it, so a test daemon in a build container behaves as today.
   - The owner chose "assembly and runtime" on 2026-10-22. This keeps runtime and drops the assembly half, because the assembly half is what would publish the private CA. Since nothing but cixd verifies against the root's bundle, nothing loses an anchor by this.

5. **Where else the store appears:**
   - **Config document (ADR-0206):** a `trust_anchors` section, applied by `reconcile`. A certificate is public and its PEM describes it completely, so the document can create and remove anchors by fingerprint, as it does other named elements.
   - **Backup bundle (ADR-0033):** included. An anchor is configuration and public, not PKI key material, and a restore onto new hardware has to reach the forge again.
   - **Factory reset and `--wipe-config`:** remove it with the rest of the state directory, as intended.

6. **The cut-over runs in this order, and step 3 never ships before step 2 is measured:**
   1. Release one ships the store, the endpoints, cixctl, the dashboard, the config section, the backup entry and `CURLOPT_CAINFO`, with the root's bundle still copied from the build host. Nothing changes for a host that registers nothing. The contract's `kernel-policy/refresh` description, which still says "a forked curl watched by pidfd", is corrected in the same change.
   2. The owner registers the site CA on 192.168.15.95, and `GET /v1/system/trust-anchors` shows it.
   3. Release two switches `mkbootroot` to the package bundle.
   - **Verified by a pair**, not by the root starting: on the new root, `pkg recipe commit` and a recipe sync against the forge succeed (the anchor is trusted), and one fetch of a host whose certificate no registered anchor and no public root covers fails with the CA error (`CURLOPT_CAINFO` is in force and not silently ignored).
   - If step 3 shipped before step 2, the forge would be unreachable until one `cixctl trust-anchors add`, which needs no fetch.

7. **Rejected:**
   - #591's option 2, an anchor directory on `/config` read by convention: content arriving by filesystem convention rather than by declaration, which ADR-0333 removed from images.
   - #591's option 3, keeping the build host's file: the trust store stays inherited and unrecorded.
   - Writing anchors into the root at assembly: it publishes the owner's private CA in the installer ISO.
   - A per-source `ca` field on ADR-0324's source list: a fetch URL need not belong to a source, and the certificate's own names already scope what an anchor can vouch for.

8. **Out of scope, and unchanged:** the container-side `cix-ca-bundle.pem` written by `pki_write_trust_bundle_file()`; the LDAP client's explicit CA; and whether the platform's own CA should be an implicit anchor. Each is its own decision.

## Consequences

- **A fresh install trusts public roots only**, so the default sources (the public catalogue on GitHub, `cache.cix.world`) work at first boot. An operator with a private CA registers it once. On .95 the forge is the only private-CA endpoint; the LAN cache is plain HTTP.
- **A registered anchor is the most sensitive write after the signing keys.** A rogue anchor lets whoever holds its key impersonate the forge to this host, and recipe sync with it. The store is `system:write`, every change is audited, and the list is always visible.
- **The trust set is declared and visible.** Today nothing records what the root trusts. After this it is the package's version plus a list the API shows.
- **The last build-host file leaves the root**, completing ADR-0337 for the control plane.
- **Two releases**, because the order in rule 6 cannot be collapsed without risking the forge.
