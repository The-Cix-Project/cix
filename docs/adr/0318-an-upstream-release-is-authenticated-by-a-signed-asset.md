# 0318 — An upstream release is authenticated by a signed asset, and the platform writes the next recipe

## Status

Superseded in part by [ADR-0323](0323-every-package-can-roll-discovery-authentication-and-a-green-build.md) (2026-10-02): a signed release asset is no longer the only way a release is authenticated. The git-first writer and the separate upstream key store stand.

Accepted by the owner on 2026-09-30, who chose both open questions in [#508](https://git.home.arpa/itdlabs/cix/issues/508): releases are authenticated by **signed release assets**, and a recipe the platform writes is **committed to cix-recipes**. Supersedes one consequence of [ADR-0255](0255-a-recipe-is-a-rule-not-a-version.md): "The daemon no longer generates recipes". Depends on cix-build-system#274 (CPDL must accept the provider name) and hibr#62 (hibr must publish signed assets). Nothing is implemented.

## Context

The owner wants hibr, their shell, to follow its releases on this platform without anyone writing a recipe. [ADR-0255](0255-a-recipe-is-a-rule-not-a-version.md) set the direction: a recipe declares an `upstream` discovery kind, and rolling runs through Discover, Resolve, Authenticate, Fetch, Build, Publish and Roll. Measured on 192.168.15.95 on 2026-09-30, the platform stands here:

- **Publish and Roll work.** Publishing `hibr@0.49.1-2` queued a rolling rebuild of jumpbox, which tracks hibr `rolling` since `jumpbox@2.9.0`. The `jump` container followed it (`follow_rolling`).
- **Discover only reports.** `srcupstream` knows one kind, `kernel.org`, and `srcresolve` reports whether a recipe exists for the version upstream publishes. Nothing acts on the answer.
- **Nothing writes a recipe.** ADR-0255 expected recipes to carry variables, so that no revision would need writing. CPDL has no variables, and a published `(name, version)` is immutable ([ADR-0107](0107-package-image-versioning.md)). So a new release needs a new recipe file, and today a person writes it.
- **CPDL refuses any other kind.** A recipe declaring `upstream "gitea-releases"` is rejected at publish with `CPDL-E3006: validation: unsupported upstream discovery provider`.
- **hibr publishes nothing signed.** Its releases are bare tags. Gitea generates tag archives on request, and those bytes have been seen to change after the fact ([#513](https://git.home.arpa/itdlabs/cix/issues/513)).

ADR-0255 is explicit that a kind must know "where that project publishes its own signed checksums", and that an upstream offering none "cannot declare a kind, and is therefore pinned". A Gitea tag has no checksum at all.

## Decision

### 1. A `gitea-releases` discovery kind

A recipe declaring `upstream "gitea-releases"` names its repository through its own main source URL: host, owner and repository, the same repository that URL already points at. Discovery reads that repository's releases through the Gitea API, with the repo token the daemon already holds (`{{REPO_TOKEN}}`). It takes the newest release that is neither a draft nor a pre-release, and expects two assets on it:

- `<name>-<version>.tar.gz`, where `<version>` is the tag without a leading `v`;
- `<name>-<version>.tar.gz.minisig`, a detached signature over it.

The tarball is an *uploaded* asset. Its bytes are fixed when it is published, which is the property a tag archive does not have.

### 2. Authentication is a minisign signature over the asset, checked in full

The asset is fetched host-side and verified with `releasekey_verify_file()`, the same code as [ADR-0279](0279-an-artifact-carries-its-own-approval.md)'s artifact gate. The trusted comment is compared **in full** against a string the daemon builds, never parsed:

```
<name> v<version> <name>-<version>.tar.gz sha256=<64 hex>
```

A signature by a trusted key that approves some other release, file or bytes is refused, for the same reason the artifact gate refuses one (a genuine signature over the wrong thing is how the wrong bytes get installed).

**Upstream keys live in their own trust store, per package**, under the daemon's state and managed through their own API. They are never placed in `docs/keys/` or the artifact trust directory. A key that approves hibr's sources must not be able to approve a Cix artifact, and a Cix release key must not approve hibr's sources.

**There is no trust-on-first-use.** A release with no signature, a signature by an unregistered key, or a comment that does not match halts that package and says why, as ADR-0255 requires. It never falls back to hashing whatever arrived.

### 3. The platform writes the next recipe revision

When discovery finds a verified release newer than every recipe for that package, the daemon writes the next revision from the newest existing one. It changes exactly these:

- `version` becomes the release's version and `release` becomes `1`;
- the main source's `url` becomes the asset's URL, keeping `{{REPO_TOKEN}}`, and its `sha256` becomes the verified digest;
- `artifact_sha256` is **removed**, because an approval of one byte sequence is never carried forward to another (Build Provenance Mandate);
- a `changelog` entry is generated, naming the release, the key that signed it and the digest.

Everything else, including the build phases, the declared tools and the assertions, is carried over unchanged. A release whose build needs a different recipe fails its build and says so. That is the correct outcome: a person then writes that revision, as they do today.

### 4. It is committed to cix-recipes first, and only then published

The written revision is committed to cix-recipes `main` through the Gitea contents API, as the flat file `recipes/package/<name>@<version>-1.cbs` ([ADR-0308](0308-recipes-are-their-own-repository-flat.md)). It is then published to this host's store through the ordinary `pkg_recipe_add()` path, which queues the rolling rebuilds. Git stays the one source of truth: every host that syncs gets the same revision, and no host holds a recipe git lacks.

**Committing is opt-in, and off by default.** It needs a repo token with write access and an explicit setting, for the reason [ADR-0315](0315-the-public-catalogue-and-cache-are-the-defaults.md) gives for never pushing by default. A host that has not enabled it discovers and verifies, reports the release as available, and **writes nothing**. It does not write locally instead, because a local-only revision is exactly the divergence this section rules out.

The [ADR-0273](0273-a-gate-holds-automation-where-a-change-escapes-its-blast-radius.md) `publish` gate is the approval point for a written revision, as for any other. There is no new gate.

### 5. One hourly schedule; each recipe opts in

A `pkg.discover` scheduler action ([ADR-0257](0257-one-scheduler-structured-schedules.md)) checks every recipe that declares an `upstream` kind. On a host that has never saved a schedule file, a default `pkg.discover` runs **hourly**. That is ADR-0316's rule for `recipe-sync`: a deleted schedule stays deleted, and a saved one is never replaced. A recipe opts in by declaring `upstream`, and there is no per-recipe interval. A discovery is one API call per rolling package, so a per-package interval would be a setting with nothing to tune.

## Alternatives considered

- **Trust the forge (TOFU).** Fetch the tag archive over TLS with the token and record whatever it hashes to. It needs no change to hibr, but it is not authentication. It would contradict ADR-0255's Authenticate stage outright, and it stays exposed to #513's archive drift. Rejected by the owner.
- **Write to this host's store only.** Simpler, but the store would then hold recipes git does not, and no other host would learn of them. Rejected by the owner.
- **Variable recipes, as ADR-0255 envisioned.** That needs CPDL to grow resolved variables, and it changes what a published revision means. It is not ruled out for later; this ADR is the route that works with immutable revisions as they are.

## Consequences

- **ADR-0255's "The daemon no longer generates recipes" no longer holds.** It generates exactly one kind: the next revision of a recipe whose upstream publishes signed releases. It never writes a first recipe for a package.
- **hibr cannot roll until it publishes signed assets** (hibr#62). Until then, discovery reports "no signed release" for it and moves nothing, which is the intended result.
- **No recipe can declare the kind until CPDL accepts it** (cix-build-system#274).
- **A written revision that does not build fails like any other**, and nothing downstream moves: the rolling rebuild only runs on what was published and built. Discovery keeps reporting the release until a revision builds.
- **Other forges come as further kinds**, each with its own signature convention, not as options on this one.
