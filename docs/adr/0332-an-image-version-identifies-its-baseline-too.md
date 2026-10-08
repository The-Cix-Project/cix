# 0332 — An image version identifies its baseline, not only its packages

## Status

Proposed, 2026-10-08, for [cix#579](https://git.home.arpa/itdlabs/cix/issues/579). It amends [ADR-0108](0108-image-version-content-hash.md), which defined an image version as a hash of its package manifest, and [ADR-0155](0155-baseline-reseed-manifest-hash-dedup-gap.md), which recorded the consequence this corrects.

Proposed rather than accepted: it changes what every image's version string is, on every host, which is host-wide and hard to reverse. The owner should weigh the churn described under Consequences before it ships.

## Context

ADR-0108 made an image version the hash of its package manifest. ADR-0155 then recorded, from two live incidents, that this "silently discards correct fixes": reinstalling the same package at the same version reproduces the identical hash, `image_produce_new_version()` dedups, repoints `current_version` at the pre-existing directory, and throws the freshly built tree away. A repaired artifact was installed three times with no effect and no message.

ADR-0155 documented that as a trap to work around — "the reliable escape when the content changed but the manifest did not: delete and recreate the image". It did not name the cause, and the cause is narrower than "dedup is dangerous".

**Measured, 2026-10-08:** `build_image_manifest_string()` (`daemon/src/pkg.c:4676`) builds the string that gets hashed, and it is the installed set and nothing else — sorted `name@version,name@version,…` pairs. The baseline `pkg_seed_image_baseline()` stages appears nowhere in it.

So an image's content is *its packages plus the baseline*, while its identity is *its packages*. Dedup is not malfunctioning; it is being asked a question that does not include one of the two things that determine the answer.

**What that cost, concretely.** #577 added `tty` (5:0) to the baseline and shipped in 0.2.57-468. 192.168.15.95 took it on the 2026-10-08 03:00 roll. A freshly created image has the node; `jumpbox` does not, and its device nodes are dated `Sep 21 17:29` — weeks older than the fix. `sudo`, `su`, `ssh -t` and every passphrase prompt still fail on the one image whose purpose is humans logging in, which is the exact symptom #577 was filed for, while #577 reads as closed. #578 added a sixth node the following day and would land identically.

## Decision

**An image version is a hash of its installed set *and* the identity of the baseline staged alongside it.**

1. **A baseline generation is part of the hashed string.** `build_image_manifest_string()` includes it, so an image whose packages are unchanged but whose baseline has moved has a genuinely different expected version. Dedup then does the right thing for the right reason rather than being bypassed.

2. **Nothing is ever written into an existing version's tree.** This is why the obvious repair — re-seed the rootfs an image already has — is rejected: ADR-0107 and ADR-0108 make a version's content immutable, and that immutability is what makes dedup sound in the first place. Correcting the identity keeps it; patching the tree would trade a correctness property for a convenience.

3. **No new operator verb.** An `image reseed` call was considered and rejected: it asks an operator to know that a platform change did not reach their images, which is exactly the knowledge they do not have. The fix must arrive the same way every other image change arrives.

4. **An apply or converge acts on a version difference, not only on a package difference.** Today both compare the installed set against the manifest and do nothing when they agree, so a satisfied image would never re-produce. With the baseline in the hash, "the version I have is not the version I should have" is a real difference, and the apply path is taught to act on it.

5. **The generation is reported, not only computed.** The image's own API response carries the baseline generation its current version was built with, so a stale image is visible rather than inferred. An operator could not previously tell, and neither could I without creating two containers and comparing timestamps.

## Consequences

- **The churn is bounded and spread, not a thundering herd.** An image re-produces when it next applies or converges, not at upgrade. The five followed images pick the new baseline up on their next converge, `jumpbox` included. An image nobody touches keeps the rootfs it has — which is its status quo, not a regression.
- **Running containers are unaffected.** A container records the image version it runs from and keeps running it (ADR-0108), so a new version appearing does not disturb anything already up.
- **Every image's version string changes on the release that lands this**, because the hashed string gains a field. Version strings are opaque hashes and nothing compares them across releases, but anything that recorded one and expects it to recur will see a new value once.
- **Disk cost is one additional version directory per image, at next apply.** ADR-0155's existing retention of superseded trees applies unchanged (#551 deletes them).
- **A baseline change becomes a deliberate, visible act.** The generation has to move for the fix to arrive, which means a future node added to the table without moving it would still not propagate. That is a hand-maintained number, and the same argument ADR-0224 makes for counting gcc recipes applies: a number that must change in a diff is the point, not a flaw — but it needs a gate, so the device list's own digest is asserted against it.

## Alternatives rejected

- **Leave it.** Defensible, and the status quo: a baseline addition reaches new images and no others. It is how #577 came to be closed with its symptom still reproducing. Rejected because the failure is silent and recurs with every future addition.
- **Re-seed an existing version in place.** Smallest change, and breaks the immutability ADR-0107/0108 rest on.
- **Reconcile at daemon start.** Produce a fresh version for any image whose rootfs lacks a baseline node. Testable and surgical, but it needs the hash to differ anyway or dedup discards its work — so it arrives at this decision by a longer road, with a boot-time special case added on top.

## Amendment, 2026-10-08, from implementing it

Decision points 1, 2, 3 and 5 are as written and are implemented. Point 4 is implemented too, but **not the way this ADR implied, and the correction is the finding worth recording.**

**The trigger cannot be a hash comparison.** The obvious reading of "acts on a version difference" is: compute what the version *should* be from the current installed set and re-produce when it differs from what the image is on. That is wrong twice over, and both ways are silent.

- `image_record_version()` repoints `current_version` at any version already in the image's history without touching the installed set. After a deliberate repoint to an older version, the hash test is unequal — so it would re-produce and undo the repoint, as a side effect of a converge nobody connected to it.
- Three callers pass `image_produce_new_version()` an `extra_identity` (the build sandbox's toolchain seed and migration, and build-environment composition). Their versions are chained hashes — `H(manifest, previous, change)` — which never equal the plain manifest hash, so the test would fire on those images on every single pass.

**So the generation is recorded per version, not recomputed.** `struct image_version_entry` gains `baseline_generation`; `image_create()` and the record path write today's value, and a version written before the field existed loads as `0`. The trigger asks "was this tree staged under the current baseline", which is the actual question, and a missing answer (`0`, or a current version that has aged out of the bounded history) is distinguished from a stale one. This also means clause 5 reports the same field the trigger reads, so the two cannot disagree — deriving the report one way and the trigger another would have been two accounts of one fact.

**Three claims in the first draft of this amendment were wrong, and are corrected here rather than quietly dropped.** It argued that point 4 needed a new asynchronous, pidfd-tracked job, on three premises:

- *That a rootfs copy cannot run on the event loop.* The ordinary install path already calls `image_produce_new_version()` straight through from `pkg_build_completed()`, an event-loop callback, and has for its whole history. A forked re-seed beside it would have been a second mechanism for one operation.
- *That the copy is hundreds of megabytes.* It is an O(1) copy-on-write btrfs snapshot, or a hardlink copy on ext4 — ADR-0207 made it so deliberately. Neither copies the data.
- *That identity-alone spends the churn without buying the benefit.* Without point 4 nothing compares expected against current, so an untouched image keeps its version and an installing one gets a version it would have gotten anyway. The identity change alone is zero-churn, and it already fixes ADR-0155's same-manifest reinstall trap.

All three came from reading one call site — the one fork — and generalising from it. The ADR under-estimated point 4 before implementation and the amendment then over-estimated it, from the same cause both times: a conclusion about the shape of the work drawn without reading the paths that do it.

**What arrives where.** The re-seed runs at one place: the converge drain's fully-satisfied point, immediately after `converge_uninstall_strays()`. An unsatisfied image needs nothing there, because the installs still to come each produce a version and the last carries the current baseline. An `apply: converge` enqueues onto that same drain, so an operator apply reaches it. An `apply: declare` deliberately realizes nothing into a rootfs (ADR-0320), so it does not re-seed either — a declare-mode image takes the new baseline on its next real install, and one that never installs keeps the tree it has, which Consequences already states.

**The gate is `test_baseline`, in SELFTESTS.** It pins the device list keyed BY generation, so changing the list without the number fails, and moving the number without recording what it stages fails too. Keyed rather than a flat pin because a flat pin passes for anyone who edits the list and the pin together and leaves the generation behind — the two edits sit in the same diff and read as one change. It is in SELFTESTS and not in `test_devices`, where the subject matter belongs, because `test_devices` creates real containers and a build container cannot run it (#224): a gate in an excluded test is a gate that never runs. It covers the device nodes only; the rest of the baseline is unpinned and says so in both places.
