# 0396 — A site's deployments live in the site's own repository

## Status

Accepted, 2026-11-26, by the owner: *"move gibsson-1's deployments to the new gibsson-1's repo, and make it clear on that separation and the ability for a administrator/user to add their own repo to push back and pull changes from"*, and the site repository made private. Raised by the owner: *"I have created a new repo called gibsson-1, this will be for the deployments ... why are we mixing ... we should split my own automation (my own deployments) from the cix ones ... when I create a new deployment, I will create a new repo / if I want one, for that deployment, and use it as my source of truth"*. Decides #712.

## Context

`cix-recipes` holds three kinds of recipe: packages, images and deployments. The first two are the platform: any Cix host may use them. A deployment is one site's container: its addresses, its networks, the names of its LDAP groups and volumes. `cix-recipes` is push-mirrored to the public catalogue on GitHub (ADR-0324), so a site's layout is published with it.

Mixing them also breaks the site, measurably. gibsson-1's store holds one deployment per name, and a sync writes git's copy over the box's whenever they differ:
- `cix-recipes` still carries .95's `ntp-1@1.6.0`, at `192.168.150.105`, with `CAP_SYS_TIME`.
- On 2026-11-23 I added gibsson-1's own `ntp-1` on the box (`192.168.250.105`, `-x`, #737).
- On 2026-11-26 `deployment show ntp-1` read .95's again. The running container is still right, but recreating it from the store would put it on the wrong network.

The model already supports the split. A source can own `deployment:` items beside `package:` and `image:` (`pkgsource.c`), every source is synced the same way, and one name is owned by one source (ADR-0324).

## Decision

1. **`cix-recipes` holds packages and images, and no deployment.** It is the platform, and it is public.
2. **A site's deployments live in the site's own repository** (gibsson-1's: `osakka/gibsson-1`), in the same layout: `recipes/deployment/<name>@<version>.json`. The box takes it as a source that owns those deployments.
3. **An administrator adds their own repository as a source, and the box both pulls from it and pushes back to it.** A site repository, or one per deployment if they want it, is one more source owning the names it offers: one name, one source (ADR-0324). Marked `write`, it is where the box's own changes go.
4. **A deployment changes git first, from either end.**
   - An edit in the repository reaches the box at the next sync.
   - A change made on the box is committed to the owning source first and stored after, as `pkg recipe commit` does for a package (ADR-0323): `POST /v1/deployments/{name}/commit` (`cixctl deployment commit NAME --file=PATH --version=V`), which commits `recipes/deployment/<name>@<version>.json` and is the same one job as a recipe commit (#740).
   - **The version is the operator's**, since a deployment's content carries none, and it must be above the version the box holds. The box records that version beside each deployment, from the sync that took it or the commit that wrote it: a sync takes the highest version a source offers, so a lower one would be committed and never used.
   - **`deployment add` of a name a source owns is refused** (409, naming the source and the commit). Stored on the box alone, it would be overwritten at the next sync, as `ntp-1` was.
   - A source with no `write` is pull-only: the box takes what it offers and never commits to it, so a commit for a name it owns is refused.
5. **The move:**
   - The site repository is filled from the box's store, the deployments actually running here, with no literal secret in any of them (a secret is a `{{SECRET:KEY}}`, kept on the box, ADR-0392).
   - The box takes it as a source, without `trust_keys`: read-only at the move, `write` once the commit above is released.
   - The deployments leave `cix-recipes`. Every one of its 21 names is also a gibsson-1 container's name (measured 2026-11-26), so moving gibsson-1's empties the directory. At the owner's direction the same day, the deployments were then removed from its history too (forge and GitHub mirror, `60e76de5`); a private bundle of the old history is kept off the forge.
   - A source dropping a file never deletes a deployment or its kept secrets: a sync adds and overwrites and never removes (`container_recipe_rm()` has one caller, `DELETE /v1/deployments/{name}`).

## Alternatives considered

- **Keep deployments in `cix-recipes`, one directory per site.** Rejected: it stays public, and one name still cannot mean two sites' containers.
- **Keep deployments only on the box.** Rejected: the box's copy has no history and no review, and the owner wants git as the source of truth.

## Consequences

- gibsson-1's containers have one source of truth that is not published.
- Another site is another repository; the catalogue stays the same for everyone.
- `site-config` (the forge's settings, ADR-0388) and the site's deployments stay separate repositories: one configures a service, the other declares this box's containers.
