# 0162 — A project rename reaches further than its name

## Status

Superseded by ADR-0200

## Context

This project has been renamed more than once. This ADR was written for an
earlier rename, before the current Cix identity; its original body named that
earlier identity throughout, which no longer exists anywhere in this codebase.
The record kept here is the part that outlived the specific names: what a
rename in *this* system actually costs, which is not obvious from the outside
and was learned the expensive way.

A rename here is not a documentation exercise. The project's own name is
embedded in places where changing it has real, physical consequences:

- **The Secure Boot signing key's own certificate subject.** The project owns
  a signing key whose certificate carries the project name as its `CN`/`O`.
  Renaming means either keeping a certificate that identifies the project by a
  name it no longer has, or generating a new key — and any machine that has
  already MOK-enrolled the old key needs an interactive, physical-console
  re-enrollment before a boot chain signed with the new one is trusted there.
  This was decided in favour of regenerating: a certificate's embedded identity
  is exactly the kind of thing a rebrand is supposed to reach.

- **On-disk state paths** (`/var/lib/<name>`, `/etc/<name>-tls`,
  `/etc/<name>-ldap`). Every installed host holds live state under a path named
  after the project: containers, images, PKI material, package state. A rename
  either strands that state or requires a deliberate operator migration.

- **The default instance name and its FQDN**, which appear in issued
  certificates and DNS records that already exist in the field.

- **Binary names**, which appear in every operator's shell history, every
  script, every guide, and in `PID 1` itself on an installed system.

- **Image and package names** recorded inside each installed host's own
  package database, not just in this repo.

## Decision

A rename is executed as one deliberate, sequenced pass — brand assets, then
code, then documentation — and its hard-to-reverse consequences (a regenerated
signing key, changed state paths, changed image names) are decided explicitly
and recorded, not discovered afterwards by an operator whose machine stopped
booting.

## Consequences

Recorded here rather than relearned: the signing-key re-enrollment step, the
state-path migration, and the fact that installed hosts carry the old names in
their own databases and must be migrated or reinstalled as a separate,
scheduled action from the repo-side rename.

Superseded by [ADR-0200](0200-rebrand-to-cix.md), which records the rename to
Cix — the current and, at the time of writing, final identity — and which
applies exactly the sequence this ADR describes.
