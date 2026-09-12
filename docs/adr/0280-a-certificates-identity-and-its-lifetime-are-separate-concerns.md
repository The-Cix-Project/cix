# ADR-0280: A certificate's identity and its lifetime are separate concerns

- **Status:** Accepted
- **Date:** 2026-09-12
- **Issue:** [#397](https://git.home.arpa/itdlabs/cix/issues/397)

## Context

`jump` runs sshd. Until now its SSH host key was whatever `ssh-keygen -A` generated
inside the container at first boot, which meant the key was a property of the
container's filesystem and changed whenever that filesystem was rebuilt. Every
operator connecting to the jump host saw SSH's host-key-changed warning after a
rolling rebuild.

The obvious fix was to stop generating a throwaway key and use the platform's own
PKI instead — Cix already issues leaf certificates from its own CA chain, already
delivers them into running containers, and `sshd` will happily take a PEM private
key as a `HostKey`. That was built (jump 1.9.0: a `waitkey` oneshot gating sshd on
the PKI-delivered key, `HostKey /etc/cix-tls/tls.key`) and it worked — sshd loaded
the PKI key and served on port 22 with no host-key error.

It did not fix the problem. Measured on 192.168.15.95, 2026-09-12: across a single
`follow_rolling` rebuild, jump's host key went from `2048 SHA256:nCR2Epsa...` to
`2048 SHA256:H8urRHcL...`, and jump's certificate `not_after` moved from
`Sep 11 03:24:51 2027` to `Sep 12 00:04:42 2027`. A new key, a new certificate,
the same warning.

The cause is not a bug. `pki_issue` sets the new cert's `owner_container` to the
container's own name, and `pki_cert_forget_owner()` deletes a cert whose owner
matches the container being deleted. A rolling rebuild is a delete-and-recreate —
`apply` cannot recreate in place, because the address is still held by the live
container (`409 ip is held by container "jump"`) — so every rebuild shredded the
certificate and issued a fresh one. The platform was behaving exactly as designed.

## Decision

**A certificate's identity and its lifetime are separate concerns, and a container
may be given an identity it does not own.**

The PKI record already separates them; only the API did not:

- **Identity** is `name` plus `sans[]` — what the certificate *is*.
- **Lifetime** is `owner_container` — and nothing else reads that field. Only
  `pki_cert_owned_by()` and `pki_cert_forget_owner()` consult it, and the latter
  shreds a cert solely when its owner equals the container being deleted. A cert
  with an empty owner matches no container name and survives every delete. Such a
  cert has always been creatable: `POST /v1/pki/certs` makes one, and the record's
  own comment describes it as "not tied to any container's lifecycle".

`pki_cert_deliver()` likewise never consults the owner — its only precondition is
`cert_find(name) != NULL`. The delivery mechanism was already lifetime-agnostic.

The single missing piece was a way to ask for it, so `POST /v1/containers` gains
one field:

    "pki_cert": "jump-ssh"

Deliver the already-existing certificate of this name into this container. Create
nothing. Own nothing.

`pki_issue` is unchanged and remains the default for a leaf TLS certificate.

## Why ownership is right for TLS and wrong for SSH

The two protocols fail differently, and that difference — not a preference — is
what decides the lifetime:

- **TLS verifies the CA.** A client checks that the presented leaf chains to a
  trusted root and that its SAN matches the name dialled. It does not care which
  leaf. Rotating a leaf on every rebuild is invisible to every correctly
  configured client, and a leaf that dies with its service is a genuine security
  property: a decommissioned service must not keep a live credential. `pki_issue`
  is correct here, and stays the recommendation for LDAPS, HTTPS and the rest.
- **SSH pins the exact key.** It is trust-on-first-use: the client records these
  precise bytes in `known_hosts` and reports any change as a possible attack.
  There is no CA in the loop to absorb a rotation. An SSH host identity must
  therefore outlive any individual container instance.

Conflating the two is what produced a correct implementation of the wrong thing.

## Alternatives considered

- **Put `/etc/ssh` on a persistent volume.** Rejected. It keeps the key outside
  the PKI entirely, in a per-container mount, which is the opposite of the central
  certificate store this platform already has — and #397 had itself named this the
  stop-gap that leaves the trust-on-first-use problem in place. It also answers
  only this one container, while the underlying gap ("a container cannot be given
  an identity it does not own") would remain for the next case.
- **Make `pki_issue` stop setting an owner.** Rejected as a regression: it would
  strip the correct lifetime from every leaf TLS certificate on the platform to
  fix one SSH host key, leaving dead services holding live credentials.
- **Have the daemon auto-create the cert when `pki_cert` names a missing one.**
  Rejected. It reintroduces exactly the coupling being removed — the container's
  creation would once again be the moment the identity is born. An identity that
  must outlive containers is created deliberately, once, by an operator.
- **An SSH certificate authority (`TrustedUserCAKeys`/`HostCertificate`).** Not
  rejected, but out of scope and a genuinely different decision. It would remove
  trust-on-first-use altogether rather than stabilise the key, and an OpenSSH
  certificate is not an X.509 certificate — it is a distinct signed object with
  SSH-format fields, so Cix's X.509 CA cannot issue one without new machinery.
  Worth revisiting on its own merits; it does not block this.

## Consequences

- An identity can now outlive the containers that present it, which is what makes
  a stable SSH host key possible without leaving the PKI.
- `pki_cert` and `pki_issue` are mutually exclusive, enforced as a `400`. Both
  write `tls.crt`/`tls.key` into `pki_cert_dir`, so declaring both is a conflict
  with no sane resolution; silently letting the second writer win would be a
  stop-gap.
- The named certificate must exist at container-create time, enforced as a `400`
  naming the missing certificate. Checking at delivery time instead would produce
  a container that starts successfully and quietly lacks the identity it asked
  for, because delivery failures are best-effort by design.
- Delivery repeats on every start, where `pki_issue`'s is described as one-time.
  This is not an inconsistency: a fresh container instance starts with a fresh
  filesystem, so the file must be written again. `pki_issue` already relies on the
  same repetition for its `restart: "always"` respawns.
- The operator now owns a real lifecycle decision: nothing deletes an unowned
  certificate automatically, so retiring a service means deleting its certificate
  deliberately.
- A bare `pki_cert` name is qualified with the site suffix exactly as
  `POST /v1/pki/certs` qualifies the name it creates (`siteconfig_qualify()`,
  ADR-0052), so the two cannot disagree and a deployment recipe stays portable
  instead of hardcoding one install's domain. Qualification is gated on
  `site_name`, which is `""` on 192.168.15.95 (measured 2026-09-12) — so this
  is a no-op there and would have gone unnoticed until the first install that
  set one. Same class of inconsistency ADR-0092 fixed for `dns_register`.
- The delivered file layout is identical to `pki_issue`'s, so a service
  configuration written against one works unchanged against the other. jump 1.9.0's
  `HostKey /etc/cix-tls/tls.key` needed no change.
