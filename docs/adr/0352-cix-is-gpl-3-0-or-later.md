# 0352 — Cix is GPL-3.0-or-later

## Status

Accepted by the owner, 2026-10-23. Asked directly: *"I want to change the licensing of cix to gpl3, please do so now"*, and, given the choice, GPL-3.0-or-later over GPL-3.0-only, for this repository and not yet for cix-recipes.

## Context

The Cix source was released under the Apache License, Version 2.0, stated in six places: `LICENSE`, `README.md`, `CONTRIBUTING.md`, `TRADEMARK.md`, the banner `cixctl` prints, and the Documentation Map in `CLAUDE.md`. No source file carries a licence header or SPDX identifier, so those six are the whole statement.

`CONTRIBUTING.md` says contributors keep copyright in their contributions, so a relicence needs every copyright holder. Every commit in the history is the owner's (`git log --format='%an <%ae>'`: 3380 commits under three spellings of one person, and no `Signed-off-by` from anyone else), so the owner alone can make it.

## Decision

1. **The Cix source is licensed under the GNU General Public License, version 3, or (at your option) any later version** (`GPL-3.0-or-later`).
2. **`LICENSE` is the GPLv3 text verbatim**, as the FSF publishes it. The "or later" grant is stated where the licence is named, in `README.md` and `CONTRIBUTING.md`, because the licence text itself does not carry it.
3. **Contributions are accepted under the same licence**, with the DCO sign-off unchanged.
4. **The marks policy is unchanged.** `TRADEMARK.md` still says the code licence grants no right to the Cix name and marks; only the licence it names changes.
5. **cix-recipes is not changed by this decision.** It has no licence file today, and a commit there reaches every Cix user through the public mirror (ADR-0324), so its licence is a separate decision for the owner.

## Alternatives

- **GPL-3.0-only.** Fixes the version, so a future GPL applies only by a deliberate relicence. The owner chose "or later", the FSF's recommended form.
- **Keep Apache-2.0.** Permissive: anyone may ship a modified Cix without its source. The owner chose copyleft.

## Consequences

- A distributed modified Cix must carry its source under the same terms. That covers a binary, an installer ISO or an image built from this tree.
- The packages Cix builds keep their own upstream licences. A recipe in cix-recipes is not Cix source in this sense, and an artifact built from a third-party tarball carries that project's licence.
- Releases up to and including 0.2.57-583 were published under Apache-2.0 and stay available under it. The change applies from the first commit after this ADR.
