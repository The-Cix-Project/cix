# 0314 — the cbs bootstrap seed is the previous Cix root

## Status

**Proposed.** Decides [ADR-0309](0309-shell-recipes-are-history-the-shell-path-retires-with-its-last-dependent.md) clause 5, which listed four options and explicitly left the choice to the owner. Nothing in this ADR changes behaviour; it records what is already true so that the shell build path can retire without the question being answered by default. Needs the owner's acceptance before [#516](https://git.home.arpa/itdlabs/cix/issues/516) closes.

## Context

cixd has no CPDL executor of its own. It runs `/usr/bin/cbs` — `explain` on the host when a recipe is published, and `build` inside the composed build environment. So building cbs from source needs a cbs, and ADR-0309 clause 5 asked what the root of that chain is once the shell build path is gone.

The lineage in the corpus shows the original chain: `cbs@v0.1.23-1.sh` through `cbs@v0.1.25-1.sh` were shell recipes, and every revision after them is CPDL, built by the engine the previous revision produced. `cbs@v0.1.25-1.sh` was the only route that started from nothing.

**That file no longer exists in the corpus.** Measured 2026-09-27: `recipes/package/` holds **0** `.sh` files. It was moved to `trash/` with the other superseded shell recipes and is not in the tree cixd's sync walks. So the question is no longer hypothetical — the shell root is already gone, and the chain has not broken, because the seed was never really that file.

## Decision

**The seed is the previous Cix root, and that is sufficient.**

The reasoning is not that the shell recipe was replaced by something, but that it was never the load-bearing seed once a Cix host existed:

- **Every Cix host already carries a Cix-built cbs.** `mkbootroot` refuses to seal a control-plane root without `/usr/bin/cbs` (`image/src/mkbootroot.c`, ADR-0307 clause 6). A host that boots has the engine, necessarily.
- **Building cbs from source is therefore an ordinary upgrade**, using the engine the running host already has, exactly like any other package.
- **The Build Provenance Mandate holds.** The seed is Cix-built, not external and not ambient. It is the same shape as the compiler: there is no external seed, and each generation is built by the one before it.
- **The chain's root is the first Cix root that ever carried cbs**, which is history in the artifact store and the release record, not a file that must stay buildable.

### What this rejects, and why

- **Keeping one shell recipe as the documented bootstrap root.** This is the option that looks safest and is the one actually rejected here. It would make ADR-0309 clause 3 a rule with a permanent exception — one recipe, one build path kept alive for it, forever, for a case that has not arisen. "No parallel implementations" does not admit a parallel kept for reassurance. And it is not even true that it would work: the shell build path retiring means nothing could *build* that recipe either, so keeping the file would preserve the appearance of a bootstrap without the substance.
- **A minimal CPDL executor inside cixd.** A second CPDL implementation, which is a parallel of exactly the kind this retirement exists to remove, and it would have to track cbs's language forever.
- **Seeding from an approved cached artifact no Cix root carried.** Forbidden by the no-external-seed rule (#36–#40).

### The case this does not cover, stated plainly

**A machine with no Cix root at all cannot build cbs from source.** It installs Cix from the installer ISO, which carries a control-plane root, which carries cbs. That is the documented way to get a first Cix host and it always has been — the shell recipe did not change it, because building `cbs@v0.1.25-1.sh` needed a running cixd too.

What is genuinely lost is the ability to reconstruct the engine from source *on a machine that has never run Cix*, using nothing but the corpus. That was already impossible: cixd is the thing that reads recipes, and cixd is Cix.

## Consequences

- ADR-0309 clause 5 is decided, and clause 3's last precondition is met.
- No code changes. `mkbootroot`'s existing refusal to seal a root without cbs becomes load-bearing for this decision as well as for ADR-0307 clause 6, so it must not be relaxed without superseding this ADR.
- The `trash/` directory holding the superseded shell recipes, `cbs@v0.1.25-1.sh` among them, can be deleted when the owner is satisfied — its contents are no longer a bootstrap route, only history that the artifact store and the release record already hold.
