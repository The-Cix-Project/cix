# 0255 — A recipe is a rule, not a version

## Status

Accepted

Supersedes one sentence of [ADR-0193](0193-kernel-release-channel-policy.md) — "The channel reports; it does not act." — and completes the follow-on that ADR named for itself. Extends [ADR-0188](0188-per-package-rolling-policy.md) with a second, orthogonal policy axis rather than replacing its one. Builds directly on [ADR-0254](0254-upstream-checksums-are-verified-not-computed.md), which is the property that makes acting safe.

## Context

Ask this repository how many recipes it has and the answer is 116. Ask how many recipe *directories*, and it is 1046.

The kernel accounts for 23 of them. Every single one is upstream **6.18.40**. Not one of those 23 exists because Linux released anything — they exist because *we* changed a compiler flag, or a build dependency, or a comment, and a new directory was the only way this platform could record that.

The cause is one string. `pkg_version="6.18.40-24"` carries two facts that have nothing to do with each other:

- **6.18.40** — the version upstream published.
- **-24** — the revision of *our rule* for building it.

And that string is load-bearing. `daemon/src/pkg.c` builds the artifact cache name straight from it (`"%.*s/%s-%s-%s.tar.gz"`, artifact base + name + version + arch), image manifests pin it, drift compares it, and `queue_rolling_rebuilds_for()` fires on it. So changing *either* fact — a new upstream release, or a one-line change to how we build — forces a new immutable recipe directory, a new cache key, and a full copy of a file that mostly did not change.

Two consequences, both of which this platform has been living with:

**Rolling does not reach upstream at all.** ADR-0188's `highest`/`newest`/`pinned` chooses among *recipe revisions we already wrote*. Nothing in the platform has ever asked upstream what exists. Every one of the 116 packages carries a literal source URL, so "rolling release" today means rolling across our own edits, not upstream's.

**The kernel cannot roll even in principle.** It is a hostbuild, no image manifest carries it, and `queue_rolling_rebuilds_for()` has exactly one call site — inside `pkg_recipe_add()`. So the one package with a real, published, machine-readable release feed is the one package with no automatic path at all.

ADR-0193 stopped deliberately at reporting, and was explicit about why: an auto-generated pin would mean the daemon recording a `pkg_sha256` it computed by downloading the tarball itself, which silently converts that checksum's meaning from "an operator verified this out of band" to "whatever arrived first". It also named the way out — kernel.org publishes `sha256sums.asc` per release, PGP-clearsigned — and called the missing keyring "the follow-on, and it is a real one rather than a hedge".

That follow-on now exists: `daemon/src/pgpverify.c` verifies an OpenPGP v4 clearsigned document against a pinned fingerprint, and `docs/keys/kernel.org-autosigner.asc` is the pinned key. ADR-0254 made the resulting rule general — an upstream checksum is verified, never computed here. The security objection that stopped ADR-0193 is answered, so the sentence it wrote to stop itself is the sentence this ADR reverses.

## Decision

### A recipe is a rule. One directory per package, forever.

A recipe stops being a snapshot of one version and becomes the standing description of how this platform builds a piece of software: where it comes from, how a release is discovered, how it is authenticated, what it is built with, and how it is built. Nobody copies a recipe directory again, and nobody types a version number.

A recipe may hold variables:

```sh
pkg_name="kernel"
pkg_upstream="kernel.org"                 # how releases are discovered
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v${major}.x/linux-${version}.tar.xz"
pkg_source_verify="kernel.org-autosigner" # whose signature authenticates the checksum
```

There is no `pkg_version` and no `pkg_sha256` in a rolling recipe. Both are *resolved*, not written.

### A build is a record, and the record is what everything else consumes.

Every invariant this platform relies on — an immutable cache key, a pinnable identity, a comparable version, a rebuild trigger — attaches to the **build**, not the recipe:

```
kernel-7.2.3-1
```

`7.2.3` is what upstream published. `-1` is a build sequence, assigned by the platform, counting builds of that package at that upstream version.

**The sequence is an integer, not a hash, and that is load-bearing.** `pkg_version_compare()` (`daemon/src/pkg.c`) is a dpkg-style natural sort: runs of digits compare numerically, runs of non-digits compare bytewise. It is what [ADR-0188](0188-per-package-rolling-policy.md)'s `highest` uses to order candidates. A content hash has no order — `7.2.3-a3f19c2` vs `7.2.3-9be0117` compares `'a'` against `'9'` bytewise and picks the alphabetically luckier build. Two builds of the same upstream version differing only in suffix is not a corner case here; it is the 23-kernel-directories case exactly. So the suffix stays an integer, `highest` keeps working untouched, and this ADR's claim to leave ADR-0188 alone is actually true.

**What the hash determines is dedup, not the name.** Each build record carries a `build_inputs` hash over everything that determines the output: the recipe file's content, the resolved source checksums, and the build environment's resolved manifest — the last being the packages ADR-0199 composes the environment from, not a named image, so `__pkgbuild` builds are covered rather than being a gap. Same hash means the build already exists: reuse it, do not increment. A different hash means the next sequence number. This is [ADR-0108](0108-image-version-content-hash.md)'s manifest-hash idea applied to packages, and it closes a gap the old scheme had — `-24` was typed by a human and never encoded that the *compiler* had changed underneath it, whereas this hash does.

**Nothing about artifact naming changes at all.** `<name>-<version>-<arch>.tar.gz` keeps its exact present shape, and `kernel-7.2.3-1` looks precisely like today's `kernel-6.18.40-24`. What changes is who assigns the number and whether a directory gets copied to hold it: the platform, and no.

The recipe file itself is never copied into the build record. Its hash identifies it, and git holds the content — which is what git is for, and what makes 1046 historical directories collapsible to 116 without losing the ability to say what any past artifact was built from.

### Rollability is declared, and absence means pinned.

A recipe that names a `pkg_upstream` kind can roll. A recipe that carries a literal `pkg_version`/`pkg_sha256` is pinned. A recipe that says nothing is pinned.

Pinned is not a legacy mode or a compatibility shim — it is a permanent, first-class answer. Some software has no machine-readable release feed, some has no signed checksums, and some this platform deliberately does not want moving. Those are pinned on purpose, forever, and a pinned recipe is a complete and correct recipe.

This is what makes "cannot resolve" loud by construction: a package that never declared how it discovers releases is never silently left behind, because it was never trying to move.

### Discovery is a named kind, not a general-purpose scraper.

`pkg_upstream` names a resolver. `kernel.org` is the first, and it is deliberately *one kind among several to come*, not the model — generalising kernel.org's `releases.json` into a universal shape would be inventing a standard upstreams have not agreed to. Each kind knows how to enumerate a project's releases and where that project publishes its own signed checksums. A package whose upstream offers neither cannot declare a kind, and is therefore pinned, correctly and by design.

### Two policy axes, deliberately not merged.

ADR-0188 and this ADR both use the word "pinned" for different things. They are not the same axis and must not be collapsed into one setting:

| Axis | Question | Values | Owner |
|---|---|---|---|
| **Source policy** (new) | Which *upstream release* do we build? | a channel + a depth, or `pinned` | operator, per package |
| **Artifact policy** ([ADR-0188](0188-per-package-rolling-policy.md)) | Which *built artifact* does an image take? | `highest`, `newest`, `pinned` | operator, per package, unchanged |

Source policy decides what gets built. Artifact policy decides what gets consumed. A package can roll its source while an image holds an older artifact, which is exactly the control a staged rollout needs.

### The channel is operator state; the *capability* is recipe content.

ADR-0188 established that version-selection policy is operator state rather than recipe content, and that holds here without amendment. The recipe declares only what is *true about the software* — that it publishes releases, where, and who signs them. Which line a given box tracks is a preference, set through the API:

`GET`/`PUT /v1/packages/{name}/source-policy` with `channel` and `depth`.

The kernel's existing `/v1/system/kernel-policy` becomes one instance of this rather than a parallel mechanism, since a kernel is a package (#316).

### Depth: `n-<lines>.<releases>`

A channel names a stream; it does not say how far back in it to sit. Depth does, and the grammar is read literally: go back `<lines>` release lines, then `<releases>` releases within that line. The `.<releases>` part is optional and defaults to `0`.

Against kernel.org's `stable` today (7.2.3 newest, 7.1.13 newest of the previous line):

| Depth | Means | Resolves to |
|---|---|---|
| `n` | newest release in the channel | 7.2.3 |
| `n-0.1` | same line, one release back | 7.2.2 |
| `n-1` | previous line, newest release of it | 7.1.13 |
| `n-2` | two lines back, newest release of it | 7.0.x |

This is why `n-0.1` and `n-1` are spelled differently rather than being two readings of one token: on this channel today they differ by an entire release line, and a platform that guessed between them would silently move a box across a major version boundary. ADR-0193 hit the identical ambiguity with `longterm` — six lines listed at once, "newest" meaning nothing on its own — and resolved it the same way, within the line you are already on.

### Open: what a rollable recipe does before an operator sets anything

ADR-0193 made the kernel channel `pinned` by default, which was right when the channel only ever reported. Carried forward literally it would mean nothing rolls until 116 per-package policies have been set by hand — the opposite of the intent here.

The alternative is a platform-wide default channel with per-package override, so declaring a recipe rollable is enough to make it roll.

**This is deliberately left open** as an operator-preference decision rather than settled here. It changes what the platform does on day one and belongs to whoever runs it, not to this ADR.

### A resolution failure halts that package and says why.

If a depth asks for a line or a release that does not exist, if a signature does not verify, if a fingerprint does not match, or if a discovery kind cannot reach its upstream: **that package stops, and the reason is reported.** It never falls back to the newest, never falls back to the last thing built, and never silently stays where it is while presenting as current.

Either the recipe is adjusted, or the package does not roll. Both are acceptable outcomes. Rolling anyway is not.

### The stages, and the one rule that binds them

The pipeline is nine stages across [ADR-0230](0230-the-five-lifecycle-domains.md)'s existing five domains — no sixth domain, no parallel vocabulary:

| # | Stage | Domain | Produces |
|---|---|---|---|
| 1 | Discover | Catalogue | what upstream currently publishes |
| 2 | Resolve | Catalogue | one concrete version, from channel + depth |
| 3 | Authenticate | Catalogue | upstream's own checksum, signature-verified |
| 4 | Fetch | CI | source bytes matching that checksum |
| 5 | Build | CI | a build tree, in the declared build image |
| 6 | Publish | CD | an artifact, `<name>-<version>-<hash>-<arch>` |
| 7 | Roll | CD | rebuilt images tracking the package |
| 8 | Assemble | Host lifecycle | a bootroot, or an ISO (Media) |
| 9 | Deploy | Host lifecycle | an A/B update, and a reboot |

Each stage has exactly one status surface, and reports its own failure with its own reason. #308 is the standing lesson here: a stage that has status but no trigger, or a trigger but no status, is worse than one that has neither, because it presents as working.

**Every stage has a named trigger, including for a hostbuild.** Stage 7 (Roll) works through image manifests, and the kernel is in none — so for a hostbuild that stage is genuinely empty, and saying "the rolling machinery takes over" would be the #308 pattern again. Two triggers are therefore stated rather than assumed: **a successful resolution (stage 2) starts the fetch (stage 4) regardless of build kind**, which is what gives the kernel an automatic path for the first time; and **a hostbuild's publish (stage 6) starts assembly (stage 8) directly**, because there is no image in between to react to it. For an ordinary package the path is unchanged: publish triggers Roll, and Roll triggers assembly.

**No stage begins until its predecessor is *verified*, not merely *reported*.** This platform has learned that twice from roots that assembled cleanly, reported success, and then panicked at boot with `Attempted to kill init!`. "The previous stage returned 0" is not evidence the previous stage produced something that works; the check belongs at the boundary, and the boundary refuses to open without it.

## Consequences

**1046 recipe directories collapse to 116.** The history is not lost — it is in git, which is the correct place for it, and a build record names the recipe hash that produced it.

**The kernel gains an automatic path for the first time.** It is still a hostbuild, but source policy reaches it directly rather than through an image manifest that never carried it.

**#314 dissolves rather than being answered.** The question was where a daemon-generated recipe should live. The daemon no longer generates recipes — it resolves a variable and records a build. Recipes stay hand-written, in git, and rarely touched. Close it as no-longer-applicable rather than deciding it.

**#315 is resolved by naming.** Source policy and artifact policy each keep their own `pinned`, in separate settings that are never merged.

**A recipe change and an upstream release stop being the same event.** Today both produce a new directory and are indistinguishable in the tree. Afterwards, one edits a rule and one moves a variable, and the build record says which happened.

**Pinning gets stronger, not weaker.** A pinned recipe means "this platform has decided not to move", which is a real decision. Today it also silently means "nobody has looked", because there was no other way to write a recipe.
