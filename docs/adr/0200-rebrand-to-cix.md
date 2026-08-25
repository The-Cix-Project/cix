# 0200 — Rebrand to Cix

## Status

Accepted

## Context

The project is renamed to **Cix** — "C" for the C language and for containers,
"ix" for POSIX and UNIX, pronounced *six*. The owner supplied a complete brand
system for it (`docs/brand/`): guidelines, the logo reference sheet, and the
canonical SVG mark plus a 102-icon UI set.

This is the project's second rename. [ADR-0162](0162-project-rename-and-its-blast-radius.md)
records what the first one taught: a rename here reaches into a Secure Boot
certificate's subject, on-disk state paths, the default instance FQDN, binary
names, and the image/package names recorded inside every installed host's own
database. That ADR is the reason this one could be planned rather than
discovered.

Three decisions were taken explicitly with the owner before any file changed,
because each of them has a defensible alternative:

**The CLI binary name.** The brand guidelines' own service-and-binary table
nominates `cix` as the primary human-facing CLI, and its command grammar is
written as `cix container create web`. The owner chose `cixctl` instead,
keeping the daemon/control-plane pairing explicit (`cixd` + `cixctl`) and
leaving the bare `cix` name unclaimed. This is a deliberate, recorded departure
from the guidelines rather than an oversight — the guidelines themselves
reserve the compact-binary question for "technical necessity", and an explicit
control-plane suffix is this project's answer to it.

**How far the rename reaches into history.** The narrower option — the one
[ADR-0192](0192-cix-identifier-prefix.md) took for the identifier-prefix
rename — is to rewrite living documentation and leave the historical record
saying what things were called at the time. The owner chose the maximal
option: **zero occurrences** of either previous name anywhere in the
repository, `CHANGELOG.md` history and `docs/mission/MISSION.md` included. The
cost is accepted knowingly: the changelog no longer reads as a literal
transcript of what each release was called on the day it shipped, and the
frozen charter now carries the current name rather than the one originally
typed into it. What is preserved is every entry's *substance* — only names
changed, never claims, dates, or decisions.

**The installed host.** Renaming the on-disk state paths (`/var/lib/cix`,
`/etc/cix-tls`, `/etc/cix-ldap`) and the well-known image names (`cix-builder`,
`cix-hosttools`) means an installed host's existing state no longer matches
what the new binary looks for. The owner chose to **defer the live host
entirely**: this change is repo-side only. The running box keeps its old
binaries and its old state until a separately scheduled migration, so nothing
in flight on it — including a multi-hour compiler bootstrap — is disturbed by
a rename.

## Decision

Rename to Cix across the entire repository in one pass: `cixd`, `cixctl`,
`cix-install`, `cix-recover`, the `cix_`/`CIX_` identifier prefix, state paths,
image and package names, the `X-Cix-Exec-Cmd` header, the PKI's own
`Cix Root CA` subject, every document, and every recipe.

Two things are deliberately **not** mechanically renamed:

1. **`docs/brand/`** — the owner's own supplied material. It legitimately names
   the previous identity because migrating away from it is that document's
   subject (its section 29 is the migration plan itself). Rewriting those
   mentions would make the document describe a rename from Cix to Cix. It is
   replaced only by new versions from the owner, per the Documentation Map.

2. **The Secure Boot signing key's bytes.** The key is regenerated, not
   renamed — `CN=Cix Secure Boot Signing Key, O=Cix Project`, following the
   precedent ADR-0162 set for exactly this. Consequence below.

## Consequences

- **Any machine that MOK-enrolled the previous signing key must re-enroll**
  before a boot chain signed with the new key is trusted there. This is an
  interactive, physical-console MokManager step. It is the same one-time cost
  the previous rename paid, and it is why the public cert (`.crt`/`.cer`) is
  committed while the private key stays gitignored.

- **Installed hosts are now out of step with this repository** until their
  scheduled migration: their state lives under the old paths, their package
  databases record the old image names, and their running binaries have the old
  names. A migration is a deliberate operator action (stop the daemon, move the
  state directories, rename the image directories and their references in the
  state files, deploy the new binaries) — never a compatibility path in the
  code, which only ever knows the current names.

- **`git blame` on renamed lines points here** rather than at the commit that
  last changed the logic — the same cost ADR-0192 paid, and the same mitigation:
  the rename is its own commit, touching nothing else, so a reviewer can verify
  it by reading the diff for anything that is not a name.

- **The repository itself is renamed separately by the owner.** Documentation
  links already point at the post-rename location, so they are correct from the
  moment that rename happens and wrong only in the window before it.
