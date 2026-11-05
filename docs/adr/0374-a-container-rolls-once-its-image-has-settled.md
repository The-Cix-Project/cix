# 0374 — A container rolls once its image has settled

## Status

Accepted by the owner on 2026-11-05, for #672, chosen over a job that installs a provider and its consumers together and over a hold judged on installed state alone. It amends [ADR-0124](0124-pkg-redesign-part5-rolling-containers-and-restart-jitter.md) and [ADR-0323](0323-every-package-can-roll-discovery-authentication-and-a-green-build.md) in when a container rolls, and refines decision 3 of [ADR-0373](0373-a-provider-declares-its-abi-and-its-consumers-follow-it.md) in what the hold judges.

## Context

ADR-0373's hold has to let a provider and its consumer move together. When hibr moves from ABI 17 to 18, longwire has to move to a revision built against 18 in the same step. Neither may move alone.

An image moves one package per job. The rebuild drain, `pkg_try_start_queued_rebuild()`, starts one install for the first unsatisfied manifest entry and returns, and each install produces its own image version. A follower container moves onto every one of them. `apply_rolling_container_restarts()` runs after every package job, from `try_start_queued_pkg_rebuild()` in `daemon/src/main.c`, and compares the container's version with the image's current version, with nothing asking whether the image's converge is still under way. Read on 2026-11-05.

So moving hibr and longwire takes two jobs, and the version between them holds one new and one old. vpn-1 would roll onto it and crash-loop, and ADR-0323's rollback would then mark a good package version bad partway through the move. No install order avoids it. On 2026-11-03 vpn-1 crash-looped on exactly such an image, `abi 16, shell expects 17`, for hours.

The same applies beyond ABIs. Any change that spans two packages of one image reaches its containers one package at a time today.

## Decision

1. **A follower container rolls only onto a settled image.** An image is settled when no package job is in flight for it and it is not in the rebuild queue. Until then its followers stay on the version they run, and they roll at the first package-job completion that finds it settled. A pass ends with the image leaving the queue, so a refused entry, the hold included, does not keep followers back.
2. **The hold judges the image's target set, not what is installed.** The target set is:
   - for a rolling manifest entry, the version the drain would install;
   - for a pinned entry, its pin;
   - for any other package, the installed version.

   Installing provider P at ABI N is refused when a consumer's target revision was built against another ABI of P. Installing consumer C is refused when its record names an ABI that P's target does not offer. So a provider and a consumer moving together pass in either order, the version between them is never run, and a move that would leave the image inconsistent is refused.
3. **The verdict is one pure function**, asked by `pkg_install_start()` after it resolves the version, so a request, a dependency and the drain all get the same answer. A request is refused with a 409 naming both packages and both ABIs. The drain notes the same sentence on the package's catalogue row, as `install`/`blocked`.
4. **No record, no hold** (ADR-0373 decision 5). A revision not yet built has no record, so it constrains nothing until it is built.

## Consequences

- A follower may stay on its version for the length of an image's converge, which is several builds for a large roll. That is the price of never running half of one.
- An image held in the queue by a gate (`PKG_GATE_ROLL`) holds its followers too, which matches what the gate is for.
- A held provider still counts at its target. Where the provider is a rolling entry, as hibr is in `vpn`, publishing a new ABI that is held also holds every consumer revision built against the old one. So an unrelated longwire fix built against hibr 17 cannot land in `vpn` while hibr 18 waits, though installing it would leave the image consistent. A pair move is unaffected. Whether a held rolling target should count at its installed version instead is release D's to decide.
- The target set assumes its targets are reached. Take a consumer revision that is published but whose build fails. Its record does not exist yet, so it holds nothing, and the provider moves. The pass then ends with the image settled and inconsistent, and its followers roll onto it. ADR-0323's runtime rollback is what catches that, as it catches any failed roll. Closing the gap is part of release D, whose author writes the consumer revision and so knows the move is a pair.
- Release D (ADR-0373 decision 4) inherits one ordering question. A consumer revision built before its provider's new version is installed anywhere composes the old provider and records the old ABI. The author has to make the new provider composable first, for example installed in a build image, and that is D's to settle.
