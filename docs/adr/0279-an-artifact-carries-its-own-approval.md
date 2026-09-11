# 0279 — An artifact carries its own approval

## Status

Accepted

Issue [#403](https://git.home.arpa/itdlabs/cix/issues/403). Amends [ADR-0122](0122-pkg-redesign-part3-artifact-cache-and-server.md), whose cache tiers are unchanged and whose approval mechanism gains a second, independent gate. Builds on [ADR-0220](0220-a-separate-release-signing-key.md) (the release-signing key) and [ADR-0107](0107-package-image-versioning.md) (recipe immutability).

## Context

ADR-0122 gave a recipe one optional line, `pkg_artifact_sha256=`, which opts it into the precompiled-artifact tier: an install fetches the artifact, checks it against that checksum, and only then trusts it. The design was explicitly chosen over trusting the server — "a three-way validation", in the owner's own framing: the recipe declares the hash, the artifact server serves bytes, the daemon verifies before believing.

That is still right. What broke is not the *pin* but the *carry*.

`approve_published_artifact()` (#306) writes that line into **the daemon's own** recipe store at the moment an artifact is published — the one moment the bytes are known to be both built here and accepted by the cache, so the only moment an approval is honestly earnable. Nothing carries it back to git, and git is what a freshly installed host syncs from. Measured on 192.168.15.95, 2026-09-11: the latest revision of 16 of 107 packages was approved on the box and not in git, `glibc`, `openssl`, `zlib`, `linux-headers` and `linux-pam` among them. A new host rebuilt the base of the system from source with the finished, Cix-built artifacts already sitting in the cache — #306's cold-box problem returning for every machine except the one that happened to publish.

The direction of travel is also one-way by construction, and narrower than it looks. `pkg_recipe_add()` accepts an incoming recipe differing from the stored one by exactly one added `pkg_artifact_sha256=` line and nothing else, so an approval that reaches git reaches the fleet on the next ordinary sync — but nothing carries one the other way, and *one line* is literal. `glibc 2.44-6`'s approval sat in git from 2026-08-30 to 2026-09-11 without ever reaching the box, because the commit that added it added an eleven-line comment beside it. A full sync reported `added 27, skipped 1255` and left the box unapproved; with only the comment removed, the next sync applied it. The one fact anybody would naturally write next to a checksum — where these bytes came from and why they are trusted — is the thing that silently disables it.

So the underlying shape is that `pkg_artifact_sha256=` is **artifact state stored inside recipe content**. Git owns recipes, the cache owns artifacts, and this fact belongs to both. That is why ADR-0107's immutability rule needed an exception carved into it for this single line, and why the fact keeps ending up in one store and not the other.

Two positions constrain the fix, and both are the owner's.

The first was written into `glibc 2.44-6` on 2026-08-30 and is the reason the artifact server cannot become the authority:

> The checksum lives here, in git, because the artifact server is never a trust boundary — it serves bytes, this line approves them.

The second was given directly while deciding this ADR: **key custody is the review**, and **the key comes from git via sync**.

Those two together rule out storing approval as cache metadata — that would make the server the authority — and rule *in* a signature, which leaves the server serving bytes while the trust rides on a key. They also settle where the trust anchor comes from: git, the same catalogue of record that already owns recipes. This factors the trust root rather than removing it. Before this ADR, trusting the artifact tier meant trusting 89 separate checksum lines spread across recipes, each arriving by a mechanism that demonstrably did not run. After it, it means trusting the public keys in `docs/keys/`, which are already published, already append-only, and already travel with every sync.

## Decision

**An artifact is approved by a minisign signature published beside it, and a recipe's checksum remains an independent pin.** Both gates, not one replacing the other.

**The signature is the earned, propagating approval.** On a successful publish — the same `code == 201 || 200 || 204` moment that writes an approval today — the daemon signs the artifact with its release key (`releasekey_sign_file()`, ADR-0220) and publishes the signature alongside it. Nothing about the approval is written into a recipe, so nothing has to travel back to git, and no host is privileged over any other by having happened to build it.

**The signature's trusted comment binds the artifact to its recipe revision**, as `<name>@<version>-<release> sha256=<hex>`. A minisign trusted comment is covered by the global signature, so it cannot be edited without invalidating the whole thing. Without this binding a signature is only "the key-holder vouched for some bytes", and a build of `glibc@2.44-17` would verify perfectly as `2.44-16` because the key is the same. The binding is what makes a signature an approval of *this revision's* output rather than of a file.

**The recipe checksum stays, and stays optional.** It is what ADR-0122 made it: a git-tracked, immutable, reviewable pin of specific bytes to a specific recipe revision, visible in a diff. The signature does not replace it, because the owner's answer keeps git as the trust root, and a mechanism that removed the git-held pin while keeping git as the root would be strictly weaker than what exists. The two gates answer different questions — *did Cix build and publish these bytes* versus *are these the bytes this recipe revision was pinned to* — and where both are present, both must pass.

What changes for a fresh host is that the signature alone is sufficient. An artifact whose recipe carries no checksum line installs when its signature verifies and its trusted comment names that exact recipe revision. That is #403's cold-box case, closed without any approval ever needing to reach git.

**The trust anchor is `docs/keys/`, adopted at sync.** The sync tarball is the whole repository — `pkg_sync_extract()` unpacks it and the merge walks `recipes/` inside it — so the published public keys already arrive on every sync and are simply discarded today. They are copied into a daemon-owned trusted-key store as part of the merge. Every minisign-format key there is trusted, retired ones included: a retired key is kept precisely so artifacts it signed stay verifiable, and dropping it would un-approve history. A signature naming a key id in the store is verified against that key; **a signature naming a key id the store does not hold is a hard failure, never a fall-through to trusting it.**

**A trust store that is empty verifies nothing.** It does not accept anything, and it does not silently disable the gate. A host in that state falls back to building from source, which is correct but is also exactly the cold-box cost this ADR exists to remove — so the install media seeds the store from `docs/keys/` at install time, and the first sync then keeps it current. Git via the installer's copy of git is still git.

## Consequences

- **An approval propagates by itself.** It rides with the artifact, so every host that can fetch the artifact can verify it. The publishing host is no longer privileged, and nothing has to be carried, committed or remembered. `tools/carry-artifact-approvals.sh` remains correct for pinning but stops being the only route by which a second machine learns an artifact is good.
- **The exception in `recipe_adds_only_artifact_sha256()` stops being load-bearing.** It is still there and still permits pinning a published recipe after the fact, but it is now a convenience rather than the sole path by which an artifact becomes installable. ADR-0107's immutability is no longer in tension with an approval that can only be earned after publication.
- **cix-cache must change first, and it is a separate repository.** Its store recognises a fixed suffix table, `{".iso.minisig", ".tar.gz", ".iso"}`, and `store_valid_name()` refuses any name whose suffix is not in it — so `<pkg>-<version>-<release>-<arch>.tar.gz.minisig` is rejected outright today. `.minisig` must compose with any base suffix rather than being spelled `.iso.minisig`. Read from the source, not probed with a write.
- **Verification is new code in the daemon, which today signs but never verifies.** Parsing a minisign file means the two base64 lines, legacy `Ed` mode, an 8-byte key id, a 64-byte signature, the trusted comment, and the global signature over `signature || trusted_comment` — the last of which is the one routinely omitted, as `releasekey.h` already warns. It slots into the existing artifact-fetch child, which already forks curl and waits rather than `execve`-ing it, so the second fetch and the verify have somewhere to live.
- **Existing artifacts are unsigned and stay installable.** 624 artifacts exist today and none carries a signature; a recipe that pins one still installs it through the checksum gate exactly as before. Signing is additive and starts with the next publish. Nothing is kept alive by compatibility code — there are two independent gates and neither is a legacy path.
- **The review moves, and that is the point.** It was "someone reads a `pkg_artifact_sha256=` line in a diff", for approvals that mostly never reached a diff at all. It becomes "someone decides who holds the signing key and which hosts may publish". That is a real shift in what is being trusted, made deliberately: the crypto is not the interesting part, the custody is.
