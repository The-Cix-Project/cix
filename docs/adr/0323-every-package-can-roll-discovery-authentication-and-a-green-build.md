# 0323 — Every package can roll: a discovery kind, an authentication method, and a green build

## Status

Accepted by the owner on 2026-10-02 ("clear now, start phase one"), with the answers recorded below. Amended by [ADR-0324](0324-many-recipe-sources-and-many-package-repositories.md) the same day: the platform authors into a source marked writable, so the list of sources comes before discovery in phase one. It supersedes [ADR-0318](0318-an-upstream-release-is-authenticated-by-a-signed-asset.md)'s rule that only a signed release asset authenticates a release. It extends [ADR-0255](0255-a-recipe-is-a-rule-not-a-version.md) (a recipe is a rule; discovery is a named kind; pinned is a first-class answer) and [ADR-0254](0254-upstream-checksums-are-verified-not-computed.md), and replaces neither.

## Context

The owner asked how to validate that rolling releases work, with hibr as the example: upstream is at v0.91, and `jump` runs 0.49.1. Measured on 192.168.15.95 on 2026-10-02, the chain breaks at its first step, not its last.

- **The source catalogue (`cixctl pkg source-catalogue`) has 211 rows.**
  - 210 are `discover / not-implemented`: the recipe declares no upstream, so it rolls only when a person writes a recipe.
  - The 211th is the kernel, the one package with a working discovery kind: `author / blocked`, *"stable resolves to 7.2.8 and no recipe builds it (newest recipe is 7.2.3-18)"*. Discovery works. Nothing writes the recipe.
- **hibr has tags up to v0.91 and no Gitea releases.** Its newest recipe, `hibr@0.49.1-2`, declares no upstream. Gitea 1.25.4 reports the v0.91 tag as `"verified":false, "reason":"gpg.error.not_signed_commit"`.
- **ADR-0318's design was never implemented.** Its Status says so. The `pkg.discover` schedule it describes does not exist on the box, which has only `refresh-upstreams` (kernel.org) and `recipe-sync`.
- **The half after the recipe works.** jumpbox follows its recipe and converges, tracks hibr `rolling`, and `jump` follows its image. This carried `hibr@0.49.1-2` through on 2026-09-30.

ADR-0318 required a signed release asset. That is right for a source that offers one, and almost no source in the world does. GNU signs tarballs with OpenPGP. kernel.org signs a checksum manifest. Some projects sign git tags. Most offer tags or tarballs over HTTPS and nothing else. A rule that only one method authenticates leaves nearly every package pinned forever, which is the state the catalogue shows.

## Decision

### One pipeline, every stage reported

A release moves through the stages ADR-0256 already names: **discover → authenticate → author → build → publish → roll.** Each stage writes its `(stage, status, reason)` into the package's catalogue row. A roll that stops says where and why in the place an operator already looks. Today's silence is the defect this most directly removes.

### Discovery is a kind, with parameters

The registry in `srcupstream.c` grows kinds beside `kernel.org`:

- `gitea-tags` and `github-tags`: annotated or lightweight tags.
- `gitea-releases` and `github-releases`: release objects and their assets.
- Later, `http-index`: a directory listing such as GNU's mirrors, with a filename pattern.

A recipe gives the kind its parameters: the tag pattern (`v{version}`), which versions count as releases (stable only by default), and the channel and depth that ADR-0193 and ADR-0255 already provide. The kind owns enumeration, as `srcupstream.h` already requires; nothing switches on kind names elsewhere.

### Authentication is a ladder, declared per recipe

| Rung | Method | Who holds the trust root |
|---|---|---|
| 1 | Detached signature over the tarball (minisign or OpenPGP) | A key in the package's upstream trust store (ADR-0318's store, kept apart from Cix's artifact keys) |
| 2 | Signed checksum manifest (`SHA256SUMS` + signature) | Same; kernel.org already works this way (ADR-0254) |
| 3 | Signed git tag (SSH or OpenPGP) | Same. **The platform verifies the signature itself**; a forge's `verified: true` is the forge's opinion, not evidence |
| 4 | Origin trust: TLS to a declared origin, hash recorded at discovery | The origin (see question 1) |
| none | — | The package stays pinned (ADR-0255, unchanged) |

Three rules hold on every rung:

- **No downgrade.** A written revision never uses a weaker rung than the revision it came from. Moving down is an operator act, in a commit, with a reason.
- **Immutability.** Once a version's sha256 is in a committed revision, a later fetch that produces different bytes halts the package and says so. It never re-pins.
- **Keys and trusted origins change only by an operator's API call**, never as a side effect of discovery. There is no trust-on-first-use of a key.

### The platform writes the recipe, git first

The author stage is ADR-0318's writer, unchanged. It produces the next revision from the newest one: new version, release 1, url, sha256, changelog, and `artifact_sha256` dropped. It **commits it to cix-recipes and only then publishes it**. A host without a write token writes nothing. The commit is the audit record of what was authenticated and how.

### A discovered release is a candidate until it builds

Publishing makes images that follow the package rebuild. A failed build leaves the image where it was: the image commit (`image_produce_new_version`, `pkg.c`) runs only on the success path, and a failed upgrade keeps the installed version. That was read from the code and is to be measured on the box in phase one. The catalogue row then reads `build / failed` with the build log's name.

### CPDL carries the parameters

`upstream "kind"` is a bare string today. This needs a parameterised block, requested from cbs in its general form, not for hibr:

```
upstream "gitea-tags" {
    tag "v{version}"
    verify "signed-tag" key "hibr-release-2026"
}
```

### Validation is the real event, with a record at every step

A release is pushed upstream. Within one discovery interval, each step has a record that can be checked:

1. a `hibr@<ver>-1` commit lands in cix-recipes;
2. the recipe store lists it;
3. the build log exists and passed;
4. jumpbox has a new image version hash;
5. `jump` has a new start time.

The catalogue shows the same chain while it happens.

## Phasing and cost

**Phase one** brings the owner's own forge and the kernel to a working roll:

- In cixd: `gitea-tags`, rung 3 and rung 4 verification, the author stage, stage reporting in the catalogue, and an hourly `pkg.discover` schedule.
- One cbs ticket for the `upstream` block.
- A hibr recipe revision.

Estimated at four to six cix release cycles. **.95 has a saved schedule file, so the new schedule must be created there explicitly**; a default is created only on a host that never saved one.

**Phase two** brings the rest of the world: `github-tags`/`github-releases`, OpenPGP for GNU-style signatures, `http-index`, and then recipes opting in package by package.

## The owner's answers (2026-10-02)

1. **Origin trust (rung 4): the owner's own forge.** An external origin uses rungs 1–3 or stays pinned, unless that one package's recipe explicitly opts into origin trust for its origin -- a per-package choice the owner makes in phase two, never a default. Measured over the 117 package recipes: about 34 come from GNU, 17 from kernel.org, 5 from Debian and about 30 from project sites, most of which publish signatures; about 24 come from GitHub, some unsigned.
2. **hibr rolls on own-forge origin trust.** No change in hibr's release process is needed.
3. **A rolling package rolls; there is no approval mode.** *"A rolling release is a rolling release, should never wait, unless it's not a rolling release."* Pinned is how an operator keeps a package still, so the kernel, gcc and glibc roll exactly when their policy says rolling. Heavy builds still never run concurrently: discovery queues them behind each other, because two at once wedged 192.168.15.95. (Correction, 2026-10-02: this answer also said `cixctl pkg source-policy` already sets pinned. It does not. The source policy has only channel and depth, so a package with an upstream had no hold; #565 added one in 0.2.57-451.)
4. **Roll back on runtime failure, never on build failure.**
   - **A build failure needs nothing undone.** The image never moved, because the image commit is on the success path only. The package stays on its previous version, the catalogue says why, and the next upstream release tries again.
   - **A runtime failure is when a container restarted on the new image crash-loops, never reports ready, or fails its health check.** The container is then:
     1. returned to the image version it ran before;
     2. the package version is marked bad, so it does not roll in again;
     3. the mark clears when upstream publishes something newer, or by an operator.
   - **A package no service runs** (hibr in `jump`) has no runtime signal, so its recipe's check phase, which runs the built binary before it ships, is its gate.

## Consequences

- hibr at v0.91 reaches `jump` with no human writing a recipe, and so does every release after it.
- The kernel's 7.2.8 is written, committed, built and rolled, because its source policy is rolling (stable); a kernel an operator wants held is set to pinned.
- A package that cannot be authenticated stays pinned, and the catalogue says why, as it does today.
- Every automated recipe is a git commit, so what rolled, when, and on what evidence is answerable after the fact.

## The author stage, as built (2026-10-02)

- `POST /v1/pkg/recipe-revise` (`cixctl pkg recipe revise`) takes `name`, `version`, `url`, `sha256` and `verification`. It is the one mechanism. A person moving a pinned package uses it now, and discovery will call the same function.
- cixd does not edit CPDL. `cbs revise` (cix-build-system v0.1.101, b1626a5) rewrites the newest recipe byte for byte, changing exactly what ADR-0318 section 3 lists:
  - `version`, and `release` set to 1;
  - the main source's `url` and `sha256`;
  - `artifact_sha256` removed when present;
  - a `changelog` of the form "VERSION-1: NAME VERSION, sha256 HEX, verified by VERIFICATION. Written by cixd from PREVIOUS".

  cbs validates the grammar of what it writes. cixd then checks the intent: it explains the revised text and compares every field it asked for, refusing the revision and naming the field if any differs. This check exists because the first live revision, hibr@0.99.4-1 on 2026-10-02, validated and published with its new changelog written over the metadata key (cix-build-system#279). It was corrected forward as hibr@0.99.4-2.
- The revision then takes the recipe commit's path unchanged: every publish test, a writable source with a token (ADR-0324), git first, then publish.
- Refusals:
  - a package with no recipe, because the author stage never writes a first one;
  - a version whose `-1` revision is not newer than the newest, because there is no downgrade;
  - a changelog that would not fit.
- The verification is free text from the caller today. Discovery (#508) will supply the rung that authenticated the release, and the no-weaker-rung rule is enforced there, where the rung is known.

## Discovery, as built: the gitea-tags kind (2026-10-02)

- **The kind interface takes the package.** `candidates(package, channel, ...)` and `fetched_at(package)`, plus an optional `problem(package)`. kernel.org's single feed ignores the package. gitea-tags reads that package's own cache (`srcgitea.c`, under `pkg/upstream/<name>.json`).
- **The repository comes from the recipe's `source` template**, which cbs requires to expand to the recipe's own main url. The kind's contract is Gitea's API archive route, `<base>/api/v1/repos/<owner>/<repo>/archive/<tag>.tar.gz`, and the tags are listed beside it at `.../tags?limit=50&page=1`.
  - Measured against git.home.arpa's API (the Gitea server's answer, whatever asks): the newest tag is listed first, and a page holds 50 (asking for 100 returned 50 of 86).
  - An unauthenticated request answers 404 on the tags listing (measured against git.home.arpa), on the API archive route (hibr@0.21-1, on 192.168.15.95) and on the web archive route (probe-cix-tarball@302-1, on 192.168.15.95).
  - So the owning source's token reaches the template through `substitute_repo_token()`, as for every url.
- **`tag` spells a release** (`v{version}`). Absent, a tag is its version. The pattern decides the version spelling the author stage will write: cbs's own recipes are versioned `v0.1.102`, so their pattern would be `{version}`.
- **Refresh:** `pkg.refresh-upstreams` refreshes every kind. `params {"kind": "..."}` limits it to one. Each package is one bounded GET in a helper process. A package that cannot be read records why, and its catalogue row says so (`discover / failed`) instead of "never fetched".
- **Blocked upstream:** an own-forge recipe cannot declare `source` yet. cbs v0.1.102's template check reads `{{REPO_TOKEN}}` as an unknown placeholder (cix-build-system#280). Until that ships, hibr's discovery is blocked there.
- **Then (0.2.57-448):** trusted origins, rung 4, the author step and `pkg.discover`; see the next section.

## Authentication and authoring, as built (0.2.57-448)

- **Trusted origins.** `GET`/`PUT /v1/pkg/trusted-origins` and `cixctl pkg trusted-origins`.
  - An origin is exactly `scheme://host[:port]`, and a fresh host trusts none.
  - A recipe's `verify origin` counts only when its source template's origin is listed.
  - An operator who lists an http origin trusts that path too. The list does not second-guess it, and the test's loopback forge is an origin like any other.
- **Rung 4.** For a gitea-tags package whose policy resolves a release no recipe builds, the refresh helper does the following:
  1. Expands the source template with that version (`{version}`, `{major}`, as cbs expands it).
  2. Fetches the archive host-side with the owning source's token, and records its sha256 as a candidate.
  3. Writes the verification string the changelog will carry: "origin trust: the release archive fetched from trusted origin O at T".

  Anything that stops it is the row's `authenticate` note: a method not implemented, an untrusted origin, a failed fetch.
- **The author step** runs in the parent when the helper finishes. The first candidate goes through `pkg_recipe_revise_start()`: the `revision_says()` gate, a writable source with a token, git first, then publish, which queues the rolling rebuilds.
  - Only one revision is written per run, because a recipe commit is one at a time. The other candidates are authored on later runs.
  - A refusal is the row's `author / failed` with the reason. For a recipe with a changelog that is cbs#279 today.
- **The schedule action is `pkg.discover`.** It replaces `pkg.refresh-upstreams` and keeps `params {"kind"}`.
  - A saved schedule still naming the old action reports "not registered in this build" until it is recreated. That happened on 192.168.15.95, where the hourly schedule is created by hand.
- **The url** of the written revision is the source template expanded, with `{{REPO_TOKEN}}` kept. The parsed recipe holds the template raw, and the token is put into a copy only where it reaches curl (`owning_token()`).
- **Still open:**
  - a source-policy hold (#565), added in 0.2.57-451: `pkg source-policy set NAME --pinned=on`;
  - the kernel's rung 2 (its signed checksum list), which the kernel roll needs;
  - own-forge templates (cix-build-system#280) and changelog revisions (cix-build-system#279), both fixed in cbs v0.1.104 (0.2.57-450).

## The validation event, measured (192.168.15.95, 2026-10-03)

hibr rolled from 0.99.4 to 0.99.11 with no person writing a recipe. The one recipe written by hand was hibr@0.99.4-3, which added the upstream block to the release already running. After that, one run of the hourly `discover` schedule (run on demand at 1790986600) produced each step this ADR asks to be checkable:

1. **Commit** `5d72537` in cix-recipes: "hibr@0.99.11-1: committed by cixd on cix before publishing it". Against 0.99.4-3, the diff is exactly:
   - version, and release set to 1;
   - the main url and its sha256 (`64a855ea…`);
   - the artifact approval removed;
   - a changelog naming the digest and "origin trust: the release archive fetched from trusted origin https://git.home.arpa".
2. **The store lists hibr@0.99.11-1**, and the source catalogue row reads resolved 0.99.11, newest recipe 0.99.11-1.
3. **The build passed.** The artifact was published to the cache and signed, and its approval was written back to git (`50013b2`).
4. **Publishing queued the jumpbox rebuild**, and jumpbox's current version became `f9cd32716a6a…`.
5. **`jump` runs on image version `f9cd32716a6a…`**, with a new pid.

It needed cbs v0.1.104 (cix-build-system#279 and #280), 0.2.57-450, and `https://git.home.arpa` on the trusted origins.

## Runtime rollback, as built (0.2.57-452)

Answer 4's runtime half, in `cixd` (main.c, `pkgbad.c`):

- **A roll is recorded when it happens.** When a rolling pass re-pins a live `follow_rolling` container, its definition records `roll_from` (the image version it ran) and `roll_to`. A roll that arrives while an earlier one is still on probation keeps the earlier `roll_from`, unless the container is up and ready on the version in between, which then counts as proven. Both fields persist, so a daemon restart mid-window re-arms the watch.
- **The window starts when the rolled incarnation starts.** It lasts `rollback_window_seconds`, set in `PUT /v1/system/rolling-config` (default 300, range 10–3600). The container goes back to `roll_from` on any of:
  - it is not ready when the window closes, where "ready" is the registry's derived readiness, which every container on 192.168.15.95 reported on 2026-10-03, `jump` included;
  - three exits, each within 30 s of starting, before the window closes;
  - the rolled version cannot be created at all, on the roll or on a crash restart.

  Ready at the deadline confirms the roll. An operator stop ends the watch with no verdict.
- **What is marked.** Every package whose version differs between the two image versions' manifest snapshots is marked bad, because one of them is why and the platform cannot tell which. If none differs (a baseline change), the image version itself is marked, as package `@<image>`. Marks are listed in `GET /v1/pkg/bad-versions` and persisted in `bad_versions.json`.
- **A rolling pass skips an image version that carries a mark.** It logs the refusal once per version. A mark ends when:
  - the package has a newer recipe (or, for an image mark, the image moved on);
  - or an operator calls `DELETE /v1/pkg/bad-versions/{name}/{version}` (`cixctl pkg bad-versions clear`).
- **It shows in the source catalogue** as `verify / failed`, naming the container and the reason. That is the readiness stage ADR-0256 already defines, so no new stage was added.
- **Image GC keeps `roll_from`** while a roll is on probation, or a rollback would have nothing to go back to.

What `test_rolling_restart` proves, gated in the release from this version:
- a rolled release that crash-loops goes back, and its mark names the versions it failed on and went back to;
- the next rolling pass leaves the container alone;
- clearing the mark works and is a 404 the second time;
- a healthy release rolls and stays past the window.

The not-ready-at-the-deadline path is not exercised there. With a 2 s doubling restart backoff, a crash-looping service reaches three exits before any window of 10 s or more closes.

## Rung 2, as built (0.2.57-456)

The signed checksum list, for the kernel first.

- **The upstream key store ADR-0318 described now exists** (`upstreamkeys.c`, `POST/GET /v1/pkg/{name}/upstream-keys`, `DELETE .../{fingerprint}`, `cixctl pkg upstream-keys`):
  - A key enters only by an operator's call, pinned by a fingerprint the operator checked out of band. It is refused unless that fingerprint is the key's own (`pgp_key_fingerprint()`).
  - A lookup is always by package and fingerprint, so the kernel's key answers for nothing else.
  - The store is apart from artifact trust and `docs/keys`.
- **Discovery authenticates by the recipe's verify method, not by its kind.**
  - `verify origin` is rung 4, as before.
  - `verify checksums "openpgp-clearsigned" { url key }` is rung 2. The list is fetched host-side and verified with the existing `pgp_clearsign_verify()`, against the installed key with the recipe's fingerprint.
  - The archive's sha256 is read from the verified text by the last path element of the expanded `source`. The archive is not fetched at discovery; the build fetches it and refuses other bytes.
  - The candidate's verification line, which becomes the changelog, names the file, the list and the key.
- **What authenticate and author found is a per-package record** (`srcrecord.c`, `pkg/discovery/`). It used to live in the gitea-tags listing document, which a second rung made a second writer. The catalogue reads it for every kind, so the `note` hook on a kind is gone.
- **A run that includes kernel.org authenticates after the release-list fetch ends**, so the kernel is checked against the list that run fetched. `{"refresh": false}` authenticates against what is held without fetching, which is useful right after installing a key and is how `test_pkg` stays off the network.
- **Measured on 192.168.15.95, 2026-10-03** (`probe-kernel-checksums@1-1`, `@2-1`):
  - `https://cdn.kernel.org/pub/linux/kernel/v7.x/sha256sums.asc` is one clearsigned list (`Hash: SHA256`), 13548 bytes at the time, with a `<sha256>  <filename>` line for every file in the directory.
  - Its signature is v4, RSA and SHA-256, with issuer fingerprint `B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1`. That is what `pgpverify.c` supports.
- **The kernel cannot declare rung 2 yet.** CPDL requires `{version}` in a verify url, and kernel.org's list is per directory (`{major}` only). That is cix-build-system#281. Until it ships the kernel stays where it is, and its row says so.
- **The key itself is the owner's act.** Installing the autosigner key on a host is pinning that fingerprint, and that is the operator's out-of-band decision. Discovery does not fetch it.
