# 0049 — PKI CA reset/regeneration: reissue-in-place, not destroy

## Status

Accepted

## Context

Raised directly by the user in the same identity-focused batch that produced `instance_name` (ADR-0046 follow-up): the root CA's common name should reflect this install's own `domain_suffix`, and the same for the intermediate. On this project's own live daemon, the root and intermediate were not fresh — `CN=Test Root CA` / `CN=thinC Intermediate CA`, both bootstrapped in an earlier phase, with at least one real leaf (the lldap container's cert, Phase 25) already signed and in active use.

`pki_ca_create()`/`pki_intermediate_create()` are deliberately one-shot (`PKI_ERR_ALREADY_BOOTSTRAPPED` on a second call) — correct for first-ever bootstrap, but it means there was no way at all to change an already-bootstrapped CA's subject short of `thincd` never having a mechanism for it. `struct pki_cert_record` (`daemon/src/pki.c`) also has no field recording which CA tier signed a given leaf; since there is only ever one active signer at a time, this is fine for renaming purposes — every leaf currently tracked was signed by whatever's about to be replaced, full stop.

Two real questions had to be settled before writing any code, not assumed:

1. **What happens to leaves already issued under the old chain?** Silently orphaning them (a cert nobody can renew, verifying against nothing) was rejected outright — every currently-tracked leaf is a real, in-use identity (at minimum, lldap's). The only defensible answer is: reissue every one of them, fresh, under the new chain, in the same operation.
2. **Does a reissue count as a new "issuance moment" for private-key-reveal purposes?** `pki_cert_create()`'s own documented policy is that a leaf's private key is returned exactly once, at the moment of issuance, never again. A CA reset generates a genuinely new keypair for every reissued leaf (the old key/cert are unlinked, unrecoverable, before any reissue is attempted) — so by the letter of that policy, this *is* a new issuance moment for each, and withholding the key here would silently and permanently lose it for any leaf with no live owning container to redeliver into.

## Decision

**New `pki_ca_reset(root_common_name, intermediate_common_name, root_days, intermediate_days, leaf_days, w)`** (`daemon/src/pki.c`) is the one real "start over" operation, explicitly separate from — and never implicitly triggered by — `pki_ca_create()`/`pki_intermediate_create()`'s own one-shot guarantees:

1. Snapshots every currently-tracked leaf (`name`, `sans[]`, `owner_container`) into a heap-allocated copy before touching anything.
2. Unlinks every leaf's key/cert, clears the in-memory index, and persists the now-empty state — so a reissue failure partway through never leaves the index pointing at files that no longer exist.
3. Unlinks the root/intermediate key/cert/serial files, then calls the existing `pki_ca_create()`/`pki_intermediate_create()` (bootstrapped state is now false, so they proceed normally) — no duplicated signing logic, the exact same code path a genuine first-ever bootstrap uses.
4. Re-issues every snapshotted leaf via the existing `pki_cert_create()`, same name/SANs/owner, fresh keypair, `leaf_days` validity. **Every reissued leaf's `cert_pem` and `key_pem` are included in the response** — the "returned exactly once, right now" policy applied honestly to what is, in fact, a new issuance. A leaf whose reissue itself fails is logged and simply absent from the response and the index afterward — not left in a stale "still valid" state, since its signer no longer exists the moment this proceeds.
5. Only re-creates the intermediate if one existed before the call — resetting the root alone never invents an intermediate tier that wasn't already there.

**Redelivery into live containers is the REST handler's job, not `pki_ca_reset()`'s.** `pki.c` has no dependency on `containerdef.c`/`registry.c` today (by design — it doesn't need a live pid or a container's own `--pki-cert-dir` for anything else it does), and this reset shouldn't be the first thing to introduce one. `handle_pki_reset()` (`daemon/src/main.c`) calls a new `redeliver_pki_certs_after_reset()` afterward, for every currently-live container.

**Enumerated via a new `registry_list_names()` (`daemon/src/registry.c`), not `containerdef_resolve_order()`** — the first draft of this used `containerdef_resolve_order()`, mirroring `containerdef_autostart_all()`'s own idiom, and it was wrong: `containerdef_add()` (main.c's `POST /v1/containers` handler) only persists a definition for `restart != "no"`. A plain, unpersisted `restart:"no"` container that used `pki_issue:true` is a completely ordinary, real case (confirmed the hard way — a test container built exactly this way silently never got redelivered, caught by comparing the delivered cert's own file content before and after reset, not just checking the reset endpoint's own status code) and has no containerdef entry to enumerate through at all. `registry_list_names()` enumerates every live `registry_entry` regardless of persistence.

**Ownership checked against the PKI index itself, via a new `pki_cert_owned_by(name, owner)`, not the original request body's `pki_issue` flag** — more robust than re-parsing a persisted body that may not exist, and it's the actual source of truth for "does this container own this cert" regardless of how it got created. `pki_cert_dir` is still recovered from the persisted definition when one exists (same field the original `pki_issue` block reads); a `restart:"no"` container that used a non-default `--pki-cert-dir=` has no persisted body to recover that override from, so the default (`/etc/thinc-tls`) applies instead — a known, narrow gap (see Consequences), not a silent wrong-path write.

**New `POST /v1/pki/reset`** — a compound operation, deliberately not shoehorned into `DELETE /v1/pki/ca` semantics (a reset isn't a resource delete, it's delete-then-regenerate-then-reissue). Optional body fields (`root_common_name`, `intermediate_common_name`, `root_days`, `intermediate_days`, `leaf_days`) default to `"thinC Root CA - <domain_suffix>"` / `"thinC Intermediate CA - <domain_suffix>"` (composed from the current `siteconfig`, via a new minimal `siteconfig_domain_suffix()` accessor) and the same `3650`/`1825`/`365` day defaults the individual bootstrap endpoints already use.

No new CLI confirmation flag (`thincctl pki reset` executes directly) — matches this project's own established convention for every other destructive `thincctl` subcommand (`pki cert rm`, `network rm`, ...). The web dashboard's own `confirm()` dialog (matching `reboot`/`shutdown`'s existing pattern) is where the "are you sure" prompt lives.

## Consequences

- This is the only way to change a bootstrapped CA's subject — there is still no rotation/renewal mechanism short of a full reset; a partial "reissue just this one leaf under a new key, same CA" operation remains unbuilt (not asked for).
- Every certificate issued before a reset stops verifying against the new root the instant the reset completes — this is the whole point, but it's a real, immediate trust-chain break for anything currently relying on the old chain that this reset's own redelivery pass doesn't reach (a leaf with no live owning container, or a container using the cert from somewhere other than the standard `--pki-issue`/`--pki-cert-dir` delivery path).
- `struct pki_cert_record` still has no "signed by which CA tier" field — deliberately not added here, since it isn't needed for this operation (every tracked leaf is always signed by whatever's currently active) and no other consumer has asked for it.
- A `restart:"no"` container that both owns a `pki_issue`-created cert AND was created with a non-default `--pki-cert-dir=` will be redelivered to the *default* path (`/etc/thinc-tls`) after a reset, not its actual custom path, since nothing persists that override once creation completes for a `restart:"no"` container. Narrow (two independent non-default choices have to coincide) and not silently wrong (the standard path still gets a valid, correctly-issued cert — just not necessarily the one path the service inside was told to read from).
- `pki_ca_reset()`'s own response can be large (every reissued leaf's full cert+key PEM) — acceptable for an administrative, infrequent operation, not a hot path.
