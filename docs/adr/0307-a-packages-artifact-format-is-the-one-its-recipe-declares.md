# 0307 — A package's artifact format is the one its recipe declares

## Status

Proposed. Stage 3 of the four-stage flip
[ADR-0305](0305-a-recipes-format-is-its-filename.md) set out, and the
first stage to change what is written into the shared artifact cache
rather than only how a build is driven.

The owner has accepted the decision's shape, including the two clauses
that go beyond "support both": clause 2, which moves finalization into
`--finalize-command`, and clause 6, which makes `cbs` mandatory in the
control-plane root. `Proposed` rather than `Accepted` only because no
code implements it yet; it flips when the daemon does.

**Its one dependency is already cleared.**
[cix-build-system#173](https://git.home.arpa/itdlabs/cix-build-system/issues/173)
was filed and closed on 2026-09-18, and `cbs explain --json` on `main`
(VERSION 0.1.26, tagged `v0.1.26`) now emits `format` as a top-level key.
The section **What this depended on** below records what was measured
before the fix and is kept as the reason the field exists, not as a
present-tense blocker.

That dependency is now fully cleared, and the sentence that used to
stand here — *"what remains before the code can land is a `cbs` recipe
revision at `v0.1.26` built and installed on the host, since the running
engine is still `v0.1.25-6`"* — is history rather than a blocker.
`cbs v0.1.29-1` is installed in `cix-builder` and `cix-hosttools`, and
`mkbootroot` has staged it into the running control-plane root
(192.168.15.95, `v2.57.224`). Read at tag `v0.1.29`, `src/main.c:344`
emits `,"format":` immediately after `release`, so the key is there.
What it uncovered instead is clause 7 below: the documents already on
disk were derived by the engine that was running when each was
published, and most of them predate that key.

It is also a deliberate, owner-decided exception to this project's
standing no-backward-compatibility rule, which says clean cut-overs and
no legacy-fallback shims. The owner's instruction, verbatim, on
2026-09-18: *"I think we should support both and then once we're
healthy, we will discuss how to drop the tar balls, right?"* The
exception is granted with an exit condition, and the exit condition is
in this document rather than in someone's memory — see **Retiring the
tarball** below.

The distinction that makes the exception coherent, and the reason it is
not the shim the rule forbids: that rule governs **code kept alive to
serve an old caller**. This concerns **immutable data already in a
shared store**. 767 tarballs are published, signed, checksum-approved
and installable today, and no decision taken here can rewrite them. A
format the platform can still read is not a fallback path; it is the
platform continuing to be able to read what it wrote.

Scope: **package** artifacts. Image artifacts (`images/<name>-<hash>.tar.gz`,
ADR-0123) are a different kind with a different consumer and this ADR
does not touch them.

## Context

CPDL already models this decision, and the platform already ignores it.

Measured on 2026-09-18 against cix-build-system `main` (VERSION 0.1.26),
`src/validate.c:698` and `:775`: a recipe's `format` must be exactly one
of `"cixpkg"` or `"tar.gz"`, and exactly one declaration is required.
The language has had two artifact formats and a per-recipe choice
between them for as long as this project has been writing CPDL.

Measured the same day, every `build.cbs` in this repo — twelve of them,
`recipes/package/{cbs,probe-pbs,probe-pbs-caps,probe-approve-pbs}/*` —
declares `format "cixpkg"`. And every artifact ever published from one
of them is a `.tar.gz`:

```
$ curl -s http://192.168.15.31:8080/api/v1/artifacts | ...
cbs-v0.1.25-6-x86_64         ['.tar.gz']
probe-pbs-2-1-x86_64         ['.tar.gz']
probe-approve-pbs-1-3-x86_64 ['.tar.gz']
   ... 780 entries total: .tar.gz 767, .iso 13, 0 with more than one format
```

`daemon/src/pkg.c:8498` is why: cixd's `cbs build` command passes no
`--output`, so CBS writes no artifact at all, and cixd then tars the
staged tree exactly as it does for a shell recipe. The comment above it
(`PKG_CBS_WORKSPACE`) says so plainly and calls it stage 1.

So the state to fix is not "the platform supports one format". It is
**the platform has a recipe field that declares the artifact format and
nothing reads it** — twelve recipes making a claim the daemon silently
contradicts. That is a One Source of Truth violation sitting in the
tree, and closing it is what stage 3 is.

Five further measurements shaped the decision rather than merely
supporting it.

**CIXPKG v2 is not an archive cixd could learn to read.**
`src/cixpkg.c`: an 8-byte magic `CIXPKG\0\2`, a 352-byte header carrying
the manifest and payload sizes, a SHA-256 of each, a flags byte and a
127-byte identity string, followed by a zstd-compressed manifest and a
zstd-compressed payload. The manifest is line-oriented —
`f <mode> <uid> <gid> <size> <sha256> <relpath>`, `d <mode> <uid> <gid>
<relpath>`, symlink targets hex-encoded — and the extractor rejects any
entry whose uid or gid is not 0, any mode carrying setuid or setgid, and
any file whose bytes do not match the digest the manifest claims for it.
Reading that in cixd would be a second implementation of a format this
project does not own.

**`cbs build` refuses `format "tar.gz"` outright — with or without
`--output`.** `src/package.c:305` fails a recipe declaring anything but
`cixpkg` with *"standalone builds require cixpkg"*, and the check is
**not** conditional on an output path: `cbs_build_standalone_with_events_policy()`
takes `package_path` at `:257` and does not reference it again until
`:389`, eighty lines after the refusal. "Standalone" names CBS's CLI
pipeline as against its embedder library API, not "a build that writes
an artifact".

This is why all twelve `build.cbs` in this repo declare `cixpkg`: there
was never a choice to make. `tar.gz` is a value CPDL's grammar accepts
and `cbs build` cannot execute. It is reachable only through the
embedder API, which cixd does not use — cixd execs the binary.

The consequence for this ADR is direct and it killed a clause: no PBS
recipe can declare `tar.gz` and still build, so there is no route by
which a PBS recipe produces a tarball, and any design that needed one —
a bootstrap exemption for `cbs` itself, for instance — has to solve its
problem another way.

**The prune policy is exactly the finalize rules that survive, and it is
not the flag.** `src/prune.c` accepts three rules and no others:
`strip-debug`, `drop-libtool-archives`, `drop-static-archives` — and the
last unlinks a `.a` only when `same_directory_shared_object()` holds,
which is clause 2's "superseded by a shared object" in someone else's
code. That is ADR-0251's clauses 1, 2 and 3, with nothing able to express
the clause 4 that [ADR-0306](0306-a-package-keeps-its-documentation-and-its-licence.md)
withdrew. Separately, `src/package.c:375-378` sets `CIXPKG_FLAG_FINALIZED`
when **`--finalize-command` returns 0** — not when a prune policy runs —
and `src/main.c:517` invokes that command as `execlp(cmd, cmd, staged_root,
NULL)`, an executable taking the staged root as its one argument. The two
options are different mechanisms: the finalize command is the embedder's
own step and is what the artifact records having run; the prune policy is
a declared rule list applied afterwards.

**There is a second producer of package artifacts, outside the recipe
build path.** `daemon/src/main.c:9572`, `artifact_export_start()`, writes
`<name>-<version>.tar.gz` for a hostbuild artifact — the `pkg hostbuild
cix --deploy` route. It is not reached by anything decided below, and a
recipe whose artifacts that path produces cannot change format without it
changing too.

**The cache needs no change.** cix-cache's `src/store.c:129-131` holds
one suffix table, and it already reads:

```c
static struct suffix g_suffixes[] = {
	{ ".tar.gz.minisig", 0, 0 }, { ".cixpkg.minisig", 0, 0 }, { ".iso.minisig", 1, 0 },
	{ ".tar.gz", 0, 0 },         { ".cixpkg", 0, 0 },         { ".iso", 1, 1 },
	{ NULL, 0, 0 }
};
```

`.cixpkg` and its `.minisig` sidecar are already recognised, already in
the package tier (`installer` 0), and already follow the same signature
convention. This was read rather than inferred, deliberately: that server
has produced four bogus bug reports from unmeasured assumptions about it.

## Decision

**1. The recipe declares the format; the daemon obeys it.** A PBS
recipe declaring `format "cixpkg"` publishes `<name>-<version>.cixpkg`. A
shell recipe has no `format` field to read and therefore publishes
`<name>-<version>.tar.gz`, permanently and by construction rather than by
a rule written down anywhere.

One version of one package is **one byte sequence in one format**. The
same version is never published twice in two formats. This is the clause
that keeps coexistence from becoming a parallel implementation: the two
formats are not two ways to ship a package, they are a property of which
recipe built it.

A PBS recipe declaring `format "tar.gz"` is **refused at publish**, with
a message saying `cbs build` cannot execute it. The value is legal CPDL
and unbuildable by the engine (measured above), so the only honest
handling is to reject it at the boundary rather than store a recipe that
will fail at build time — the same posture publishing a `build.cbs` on a
host with no `cbs` already takes.

That refusal is why the daemon must **read** the declared value rather
than assume `cixpkg` for every PBS recipe. Assuming would publish such a
recipe happily and fail later, and it would encode a restriction that is
CBS's to lift, not ours to bake in. The daemon learns it the same way it
learns everything else about a PBS recipe: from the explain document,
which `parse_pbs_recipe()` reads and the recipe file it never parses.
That document gained a `format` key on 2026-09-18
(cix-build-system#173); the host must be running a `cbs` carrying it
before any of this can be read — see **What this depended on**.

**2. CBS packages a PBS build; cixd packages a shell build.** `cbs
build` gains `--output` and cixd stops tarring the staged tree for a PBS
recipe. A shell recipe is packaged by cixd exactly as it is now. Since
`cbs build` executes only `cixpkg` recipes, this is not a second policy
choice on top of clause 1 — it is clause 1 restated at the producer.

`. /build/finalize.sh` is replaced by `--finalize-command` pointing at
the same policy as an executable taking the staged root as `$1`
(`src/main.c:517` `execlp`s it that way). The reason is **one file**:
ADR-0251's rules keep a single definition across both formats, and the
thing to watch here is two expressions of one rule set — ADR-0306's
withdrawal of clause 4 already had to be made in one of them.

`--prune-policy` is deliberately **not** used yet, even though its three
rules are exactly the three that survive ADR-0306. Adopting it means
deleting the shell script and moving the policy into a declared file,
which is where this should end up and is a change worth reviewing on its
own rather than inside a format cut-over.

A note so the next reader does not build on it: `--finalize-command`
succeeding sets `CIXPKG_FLAG_FINALIZED` in the artifact header
(`src/package.c:375-378`, byte 224), and **nothing reads that bit**. The
only reads in `src/cixpkg.c` (`:271`, `:437`, `:535`) check that no
*unknown* bits are set; `cbs inspect` reports the recipe digest, licence,
source digests and artifact digest, and not the flags. So the flag is a
true record that no consumer consults. It is available if a use is ever
wanted; it is not a reason for anything decided here.

**3. Consumption dispatches once, on the artifact's extension, at the
one point where bytes become a tree.** Locating an artifact, downloading
it, checking it against the recipe's approved `artifact_sha256` and
verifying its `.minisig` are byte operations and stay format-blind.
Every install-time gate — the undefined-builtin check, the undeclared-
link check, the file manifest, the image merge — reads a tree and stays
format-blind. Between them sits exactly one branch: `cbs extract --into`
for `.cixpkg`, libarchive for `.tar.gz`. Note that `cbs_cixpkg_extract()`
refuses a destination that already exists, which the tarball path does
not, so the branch owns creating its own destination path.

**4. A `.cixpkg` carries its own integrity, and `cbs verify` is how the
platform reads it.** `cbs_cixpkg_extract()` already checks the manifest
digest, the payload digest and every per-file digest before a byte
lands, so extraction alone is safe; `cbs verify` adds the artifact's
identity string and a named error code for an operator. The gain over a
tarball is real and worth stating: a tarball's only integrity statement
is the one digest the recipe approved, while a CIXPKG additionally
carries and checks a digest per file, and refuses setuid modes and
non-root ownership outright.

**5. An approval does not cross formats.** `artifact_sha256` approves a
specific byte sequence, so a recipe converted from shell to PBS is a new
revision with no approval and earns one on its first publish, by the
mechanism [#492](https://git.home.arpa/itdlabs/cix/issues/492) already
ships. The existing approvals stay valid for the revisions they name, and
nothing rewrites them: **88 of 150** packages carry one on their latest
shell revision (measured 2026-09-18). That is the number that matters
here, because only a latest revision is installed — counting every
revision in the tree gives 287, and answers a question about history
rather than about what a host would fetch.

**6. `cbs` must be in the control-plane root, and assembly fails without
it.** A host that cannot extract a `.cixpkg` cannot install any package
built by a PBS recipe, `cbs` itself included — and clause 1 plus the
measurement above leave no escape through a tarball, because no PBS
recipe can declare one. So the engine cannot be something a root might
happen to have.

`image/src/mkbootroot.c:1196` stages `cbs` from `cix-hosttools` behind
`if (stat(src, &st) == 0)` and silently omits it when absent. The comment
above it argues for exactly that — *"a box whose cix-hosttools image
predates `pkg install --image=cix-hosttools cbs` simply has no CPDL
engine, which is a normal state rather than an assembly failure"* — and
this ADR reverses the argument rather than working around it: after
stage 3 that state is not normal, it is a root that cannot install
packages. The staging becomes mandatory and a missing `cbs` fails
assembly, the way `verify_platform_libs_intact()` fails a root carrying
mismatched glibc objects. **That comment is an artefact carrying a claim
this ADR makes false, and it is corrected in the same change.**

This replaces the bootstrap exemption an earlier draft of this ADR gave
`cbs`'s own artifact. The exemption was unimplementable — see the
`cbs build` measurement above — and making the staging mandatory is the
better answer anyway: it removes the exemption, the drift gate that would
have policed it, and the open question it would have left for stage 4.

**7. `explain.json` is a derived cache, so it is re-derived when the
engine that derived it changes** ([#496](https://git.home.arpa/itdlabs/cix/issues/496)).

Clause 1 has the daemon read the declared format from the explain
document, and `parse_pbs_recipe()` reads that document and never the
recipe. The documents already on disk were each derived by whichever
`cbs` was on the host the day that version was published, and `format`
only exists from `v0.1.26` — so most of them do not have it. Measured
via publish times against the host's own boot record: `zlib@1.3.2-14`,
`vim@9.1.1428-4` and `iputils@s20180629-4` were published on 2026-09-18
between 18:28 and 19:06 UTC, and the reassembly that put the newer
engine on the host booted at 22:30:40 UTC that evening; `cbs@v0.1.29-1`,
`mtr@0.96-9` and `libmnl@1.0.5-7` followed at 04:05–04:11 the next
morning.

Neither of the two obvious answers is available. **Refusing** a document
with no `format` makes every recipe published before that reboot
unbuildable, which is the opposite of what stage 3 is for.
**Assuming `cixpkg`** is precisely what clause 1 rules out: the daemon
must be able to refuse a `tar.gz` declaration at publish rather than
accept it and fail at build time, and assuming would encode a
restriction that is CBS's to lift.

So the third answer, which is what the document already claims to be:
**`explain.json` is derived, and derived state is rebuilt when its
producer changes.** A published `(name, version)` is immutable
(ADR-0107) and `cbs explain --json` is deterministic over the recipe
text, so re-deriving cannot produce a different answer about the same
recipe — only a more complete one from a newer engine.

**The sweep runs once at startup, and that is complete rather than
merely convenient.** The host's `cbs` is `/usr/bin/cbs` in the
control-plane root, which `mkbootroot` stages from `cix-hosttools` at
assembly time (clause 6). That root is a read-only squashfs replaced
only by `POST /system/update` and a reboot — so **the engine cannot
change while the daemon is running**, and every way it can change goes
through a restart. A recipe published while the daemon is up is derived
by that same engine at publish. There is therefore no window a startup
sweep misses, and no need for a second, lazy path on the read side.

Staleness is decided by the engine's own version string, not by probing
for a key. `cbs --version` prints `cbs <version>` (`src/main.c:664`),
the daemon records the version its last sweep ran with, and a mismatch
re-derives every PBS recipe's document and stores the new version. That
generalises: the next key CBS adds to explain is picked up by the same
mechanism, where a check for `format` specifically would have to be
written again each time — and a key-absence test cannot tell "this
engine does not emit it" from "this recipe did not declare it".

This does **not** re-derive on read, and the distinction matters. The
comment above `pbs_explain_path()` records why: re-running `cbs explain`
per read would fork once per recipe file, of which there are ~1400, and
#236 already measured the event loop blocked for 10981 ms on a
comparable walk doing something cheaper — twice, each needing the host
reset by hand. The sweep is bounded by the number of PBS recipes (28 at
the time of writing), runs before the daemon serves anything, and then
never again until the engine changes.

## What this depended on

**[cix-build-system#173](https://git.home.arpa/itdlabs/cix-build-system/issues/173)**
— filed and closed 2026-09-18. Recorded here because it is why the daemon
reads this field from the explain document rather than from anywhere
else, and because the shape of the gap explains the shape of the fix.

As measured before it was closed, `cbs explain --json` did not emit the
declared format. Measured by
reading the emitter at cix-build-system `main` (`src/main.c`, the block
ending `"metadata":{...}`): its top-level keys are `architecture,
build_image, capabilities, license, metadata, name, operations, phases,
release, requires, sources, toolchain, toolchain_reason, upstream, urls,
version`. There is no `format`, and the string does not appear in the
emitter at all — while `src/parser.c:792` parses the field and
`src/validate.c:698` enforces its two legal values. CBS reads the
declaration and does not report it.

That matters here more than it would anywhere else, because
`parse_pbs_recipe()` reads `explain.json` and **never the recipe**. A
fact CBS does not put in that document is a fact cixd cannot have.

It is a small ask, and the emitter is already extended this way:
`license` and `metadata` are in that key list on `main` and are absent
from the same emitter at tag `v0.1.25`, the version stage 2 built
(checked by fetching `src/main.c` at both refs on 2026-09-18).

## Retiring the tarball

Not a date, and not "when it feels done". The condition must be a query a
future ADR can actually run, which rules out counting recipe files: old
revisions stay in the tree forever, so `find recipes/package -name
build.sh` is nonzero permanently and means nothing.

What matters is what can still be **installed**. The tarball is retired
by a superseding ADR when **no image's manifest resolves any package to a
shell revision** — for every image, every `name@version` in its manifest,
looked up in `GET /v1/pkg/recipes`, reports its language as `pbs`. Every
term of that is already served over REST, and a `probe-*` recipe can run
it and print the remaining shell revisions by name, which is also the
most useful form of progress towards stage 4.

One thing stands outside that check and is stage 4's problem rather than
this one's: **`artifact_export_start()`** (`daemon/src/main.c:9572`), the
hostbuild path, writes `<name>-<version>.tar.gz` directly and knows
nothing about recipes. It is a second producer of package artifacts, and
no decision in this ADR reaches it.

Until that ADR is written, `.tar.gz` is a supported, first-class format
and not a deprecated one. There is no warning, no grace period and no
migration tooling, because there is nothing to migrate: published
artifacts are immutable and stay readable.

## Consequences

- **`cbs` moves from optional to required on any host that installs
  packages**, by clause 6. That is the real cost of this decision and it
  is worth naming rather than discovering. Beyond the assembly gate, a
  cixd that somehow meets a `.cixpkg` with no `cbs` present must refuse
  it with a message naming `cbs`, exactly as the build path already does
  — never fall through to an extractor that cannot be correct.
- **`GET /v1/pkg/recipes` already has a key called `format`, and it means
  something else.** `daemon/src/pkg.c:6055` emits `"format": "pbs"|"shell"`
  — the recipe's *language*, per ADR-0305. Introducing an artifact format
  under the same name in the same subsystem is two meanings for one key
  and will read as a bug. The existing key is renamed to `"language"`,
  which is the word its own comment uses, and the new one is
  `"artifact_format"`. A clean rename, no alias: `docs/api/openapi.yaml`
  and `docs/api/README.md` change together with it, and the web dashboard
  is the only other reader.
- **The daemon gains one new piece of persisted state: the `cbs` version
  its last explain sweep ran with** (clause 7). That is a record of what
  produced the derived documents, not a second source of truth about
  them — the documents themselves stay the only account of what a recipe
  declares, and this says only which engine wrote them. It is the
  smallest thing that makes "is this cache stale" answerable without
  probing for one key at a time.
- **`daemon/policy/pkg-finalize.sh` becomes an executable**, since
  `--finalize-command` is `execlp`'d with the staged root as `$1` rather
  than sourced. It stays one file and one policy for both formats.
- **`recipe.is_pbs` is not enough to name the artifact, and reaching for
  it would be the bug.** It says which language the recipe is written in
  (`daemon/src/pkg.c:297`) — not which format it declared, and clause 6
  makes `cbs` a PBS recipe that declares `tar.gz` on the day this ships.
  The parsed recipe gains a separate field carrying the declared format,
  read from the explain document; `is_pbs` keeps the single job its
  comment gives it.
- **Two traps that will otherwise each cost a build cycle.**
  `--finalize-command` is `execlp`'d *inside the build container*, so the
  policy script needs a `#!/usr/bin/bash` shebang — these images ship
  `bin/sh` and `usr/bin/bash` and no `/bin/bash`, and a bad interpreter
  reports as exit 127 against a file that visibly exists. And
  `cbs_cixpkg_extract()` refuses a destination that already exists, so
  the cixpkg branch extracts to a sibling temporary path and `rename()`s
  it into place — a copy instead would lose modes, which is #139 exactly.
- **`probe-*` recipes are how this gets proven.** The proof is a real
  `.cixpkg` published to the shared cache, installed into a real image on
  192.168.15.95, with the resulting tree compared file-for-file — content
  and mode — against the same package built as a tarball.

## Alternatives rejected

**Publish both formats for every build.** Two byte sequences for one
version, two digests, and an `artifact_sha256` that can only approve one
of them.

**A flag day: convert everything at once.** There is no mechanism that
could. 767 published tarballs are immutable and a fresh install still
installs them.

**Let cixd read CIXPKG itself.** ADR-0305 already decided cixd execs
`cbs` and never parses its formats, and the container layout above is a
second implementation of a format this project does not own — with
per-file digest checking and setuid rejection in it, so the copy would
be a security-relevant one.

**Choose the format at publish time, from a request field.** It would
make the same recipe capable of producing either artifact, which is the
thing clause 1 exists to prevent, and it would put the choice somewhere
a reader of the recipe cannot see it.

**Move to `--prune-policy` now and delete the finalize script.** Its
three rules are exactly the three ADR-0251 clauses that survive, so this
is where the platform should end up — but it replaces a script with a
declared policy file, and that belongs in its own change rather than
inside a format cut-over where a behavioural difference would be hard to
attribute. Deferred by clause 2, on purpose and in writing.

**Exempt `cbs`'s own artifact from CIXPKG to avoid a bootstrap
dependency.** This was the previous draft's clause 6 and it cannot be
built: `cbs build` refuses any recipe not declaring `cixpkg`, regardless
of `--output`, so the exemption has no format to fall back to. Clause 6
solves the same problem by making the `cix-hosttools` staging mandatory,
which is both implementable and simpler.
