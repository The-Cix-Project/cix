# 0320 — An image's policy decides how its recipe, manifest and installed set agree

## Status

Accepted by the owner on 2026-09-30, as proposed, defaults included, for [#535](https://git.home.arpa/itdlabs/cix/issues/535).

The owner's direction on #535: *"We should be able to upgrade, downgrade, and we should be able to put a recipe that's hand made in the repo, and edit one on the box ... we should have the flexibility, but it should be settable on the policy."* So this ADR does **not** collapse the three copies into one. The single source of truth is the **policy** that says which copy wins, and when.

## Context

An image's package versions are written in three places, by three legitimate writers:

| copy | written by |
|---|---|
| the **image recipe** (`recipes/image/<name>@<ver>.json` in cix-recipes, synced into the box's store every 6 hours) | a person, by hand, in git |
| the **live manifest** (`GET /v1/images/{name}`) | `PUT /v1/images/{name}/manifest`, or `apply-recipe`, which declares every recipe entry |
| the **installed set** (`GET /v1/pkg`) | `pkg install` and `--upgrade` |

Measured on 192.168.15.95 on 2026-09-30:
- The sync **stores** each image recipe (the highest version per name overwrites the stored one) but never applies it (`sync_walk_image_recipes()`).
- `apply-recipe` only **declares**: one `image_manifest_set()` per entry, with no direction check (`pkg_image_recipe_apply_start()`).
- An explicit install does not move the manifest's pin. #535 measured 29 pins left at an old revision while the new one was installed, and the rolling drain ignores pinned entries. So every change to jumpbox today took three steps kept in step by hand: edit the recipe, apply it, install each package.

Nothing is wrong with any single writer. What is missing is a rule for what happens when they disagree. #535's footgun is one case of it: applying a stale `cix-builder@6.1.0` recipe would have walked seven packages back one to five revisions each, with nothing asked.

## Decision

### 1. Every image has a policy, set per image, like ADR-0188's per-package policy

`GET` / `PUT /v1/images/{name}/policy` (`cixctl image policy NAME ...`). It is operator state, never recipe content. An image with no saved policy has the defaults, and "no policy" and "the defaults" are the same state.

| setting | values | default | what it decides |
|---|---|---|---|
| `recipe` | `manual`, `follow` | `manual` | Whether a new recipe version reaching the store (by sync or publish) is applied by itself. Under `manual` only an operator applies it, as today. Under `follow` the recipe in git drives the image, which is how a hand-made recipe in the repo reaches the box with no one on the box. |
| `apply` | `declare`, `converge` | `declare` | What applying a recipe does. `declare` sets the manifest only, as today. `converge` also installs, upgrades or downgrades each entry to match, and reports per package what it did. |
| `downgrade` | `refuse`, `allow` | `refuse` | Whether an apply may move a pin to an older version. Under `refuse` the apply is a 409 listing every entry it would move backwards, and changes nothing. A single apply can still pass `allow_downgrade: true`: a downgrade stays possible, but it is always an explicit act, never a side effect of applying a stale recipe. |

### 2. An explicit install moves the pin with it

Installing, upgrading or downgrading a package that the image's manifest **pins** moves that pin to the version installed. So the manifest and the installed set stop disagreeing, which they only ever did by accident. A `rolling` entry is untouched, as its floor already allows this. This is the box-side edit the owner asked to keep. It needs no policy, because both writers are on the box and the operator just said which version they want.

### 3. The box's state can be exported as a recipe

`GET /v1/images/{name}/recipe/export` (`cixctl image recipe export NAME`) renders the live manifest as an image recipe, ready to commit to cix-recipes. So an image edited on the box can go back into git as its recipe, and under `recipe: follow` the edit then survives the next sync instead of being replaced by it.

## Defaults, and why they are these

Every default except `downgrade: refuse` is exactly today's behaviour, so installing this moves nothing on any existing image. `downgrade: refuse` changes only an apply that would have walked a pin backwards. That is #535's footgun, and the override is one flag. An operator opts an image into `follow` or `converge` per image.

## Consequences

- **A downgrade can bring back a corrupted stored tree.** Applying a recipe whose package set existed before reuses that image version's stored tree (ADR-0155), including one written wrong before #531 was fixed ([#551](https://git.home.arpa/itdlabs/cix/issues/551)). This ADR did not fix that; #551 did, in the release after it: a reused stored tree is now checked against the files its packages record, and replaced by the tree just built when it is wrong.
- **Under `recipe: follow`, committing an image recipe to cix-recipes `main` is a deploy** within one sync window, the same property the package store already has.
- **A pin moving with an install** means a manifest no longer shows what was *requested* last, only what is installed. That is the point, and `recipe/export` is how that state gets written down.

## Alternatives considered

- **Make one copy authoritative** (the recipe, or the manifest). Rejected by the owner: both hand-made recipes in git and edits on the box are wanted.
- **Only refuse backwards applies.** It removes the footgun and leaves the three-step routine and the drift in place.
