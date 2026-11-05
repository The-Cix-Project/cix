# 0373 — A provider declares its ABI, and its consumers follow it

## Status

Accepted by the owner on 2026-11-03, for #672, with both halves (the hold and the rebuild). The provider's declaration is CPDL's `abi` field, which cix-build-system shipped in v0.1.129 (cix-build-system#312).

## Context

On 2026-11-03 cixd rolled hibr to 0.99.432-1 (cix-recipes f508356, 11:59Z), and the `vpn` image followed it. hibr 0.99.432 loads only ABI-17 modules. longwire builds its module against `hibr.h` from `tool "hibr"`, and the installed longwire 0.5.39-1 had been built against a hibr older than 0.99.430, which is ABI 16. hibr defines it as `#define HIBR_ABI` in `include/hibr.h`: `16u` up to 0.99.429 and `17u` from 0.99.430, read at each tag on 2026-11-03. longwire's version did not change, so nothing rebuilt it. vpn-1 then exited at every start with `hibr: mod: longwire: abi 16, shell expects 17` and restarted every 15 s for hours. The owner reaches their network through it.

Two things repaired it, and neither was a mechanism:

- **By hand:** longwire 0.5.39-2, the same source rebuilt against hibr 0.99.432.
- **By accident:** longwire 0.5.41-1 was published by rolling discovery at 12:59Z, an hour after hibr 0.99.432. Its build composed the newest hibr, so it was ABI 17. A consumer that rolls on its own cadence repairs itself only when its roll happens to land after the provider's. At the time of writing `vpn` holds hibr 0.99.435-1 and longwire 0.5.42-1, and vpn-1 has its dial tone.

**The population is small and real.** It is a provider whose interface changes without anything the install-time ELF gate can see: a module or plugin ABI, a static archive, a header-only contract. Shared libraries change their soname and are already caught for presence. In cix-recipes today, `tool "hibr"` has two consumers: longwire and a probe.

**What cixd knows today:** nothing about it. A package entry does not record which tool versions built it (`struct pkg_entry`, `daemon/src/pkg.c:125`). The composed environment's identity is a hash of its resolved set, and it is discarded after the build.

## Proposal

1. **The provider declares the ABI it offers**, as a typed CPDL field such as `abi "17"`, carried in `cbs explain`'s JSON. The provider is the one that knows: hibr bumped 16 to 17, and longwire could not have known. [ADR-0322](0322-a-recipe-declares-its-build-memory-within-an-operator-ceiling.md) already settled that an author's declaration is a typed field, never an opaque `metadata` key, so **this half is a cix-build-system ticket**. Until it ships, a provider has no declaration and behaves as today.
2. **cixd records what each build was built against.** When a package is built, cixd records, for every composed tool that declares an ABI, that ABI. The record is written back beside the artifact approval, per output, through the same git-first write-back (ADR-0324), so it travels with the artifact to every host. This is a record of what specific bytes were built against, like the approval itself, and not an author's declaration, which is why it may be cixd-written.
3. **A gate, for safety.** An install or converge that would put provider P at ABI N into an image holding a consumer C built against P's ABI M ≠ N is refused for that image. P stays at its current version there, and the hold is reported in `cixctl pkg source-catalogue` naming both packages and both ABIs. The same gate refuses installing C built against M into an image whose P is at N. This half alone would have kept vpn-1 up on the hibr it ran before 0.99.432.
4. **An author, for liveness.** When a provider publishes a new ABI, cixd authors a rebuild revision of each consumer whose newest revision was built against an older ABI. It uses the ADR-0323 writer, git first, and does what was done by hand with longwire 0.5.39-2. Rolling builds it, and converge then moves provider and consumer together, which releases the hold.
5. **No declaration means no inference.** A provider without an `abi` behaves exactly as today.
6. **A declaration is checked against the source, so it cannot go stale.** hibr's recipes are written by rolling discovery, each copied from the previous revision. A declared `abi "17"` would therefore be copied unchanged into the revision where hibr moves to 18. The hold would then read 17 against 17, and vpn-1 would break exactly as before, this time with a mechanism vouching for it. So a provider's recipe asserts its declaration in its build: hibr's checks `usr/include/hibr/hibr.h` for `#define HIBR_ABI 17u`. A discovery-written revision whose source has moved fails its build and rolls nothing (ADR-0323), until a person updates the number. An ABI change becomes a deliberate act on the provider's side.

**How decision 2 was built (release B, 2026-11-05).** The record is one metadata line per build, `"built_against" "hibr=17 ..."`, not one per output: one build produces every output of its recipe, so all of them were built against the same tools. It is captured when the build environment is resolved, reading each entry's own `abi` at the version it resolved to. It is written in the same edit as the first of that build's approvals, when a repository accepts the artifact. So a host with no repository marked push writes no record, as it writes no approval. A copy of a recipe that keeps its record and drops its approval is refused at publish, and the ADR-0323 writer drops the record with the approvals.

## Alternatives

- **Rebuild every consumer whenever any of its build tools gets a new version.** Rejected: a glibc, coreutils or bash roll would rebuild the corpus.
- **The consumer pins the provider's version at runtime.** Rejected: it stops hibr rolling in every image with longwire until someone edits longwire, which is the manual step this removes. It also needs a version-constraint language CPDL deliberately does not have.
- **Infer the ABI from the provider's headers.** Rejected: a guess about an interface, where the provider's author already states it.

## Consequences and cost

- **cix-build-system:** a ticket for the `abi` declaration and its explain JSON.
- **cixd, once that ships:** the build record and its write-back, the install and converge gate with its source-catalogue report, and the rebuild author. That is four releases (the explain read with the test floor moved to cbs v0.1.129, the record, the hold, the author), after one cycle putting v0.1.129 into the root, each verified on .95 by bumping a test provider's ABI and watching the hold, then the rebuild, then the release.
- **hibr's recipe:** it declares its ABI once the field exists. hibr is the owner's project, so it is also the owner's call whether its release process exposes the number.
