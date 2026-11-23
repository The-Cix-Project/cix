# 0393 — An install that must wait is queued, not refused

## Status

Accepted, 2026-11-23, by the owner ("Accept"), for #711.

## Context

`POST /v1/pkg/install` answers 409 when work already in flight blocks it. Since #701 that includes two installs into one image that share a dependency: the second is refused while the first installs it. Setting up an image on gibsson-1 meant re-issuing installs by hand after each 409. The owner, 2026-11-21: *"should it not have queued them so that they build once slots are empty?"*

Three causes of that 409 clear by themselves when a job ends:

- a job for the same package and image is running;
- another job is installing a dependency this one needs (#701);
- every build slot is taken.

Other refusals do not clear by waiting: the ABI hold (ADR-0374), a missing recipe, an image that does not exist (#500), and the one-hostbuild rule. They stay errors.

Two existing mechanisms were considered as the carrier and do not fit:

- **The image manifest and the rolling drain.** An explicit install deliberately does not declare a package the manifest does not name (ADR-0320: "installing is not declaring"). Under the default `apply: declare` policy, the re-derive does not queue an entry the image lacks, so a queued install written there would not reliably run.
- **Package entries.** They have no waiting state (fetching, building, installed, failed).

## Decision

1. **An explicit install that is blocked only by work in flight is queued.** The causes are the three above. The answer is `202` with `queued: true`, its position, and what it waits for. Every other refusal is unchanged.
2. **The queue holds the request exactly as given** (package, version, image, upgrade), in arrival order, in one state file, so it survives a daemon restart and an in-place switch. A second request for the same package and image while one is queued is a 409 naming the queued one.
3. **It is started where queued work is already started:** whenever a package job ends. In order, every queued install whose cause has cleared starts, before the rolling drain takes its turn, so an operator's request runs before automatic work. A request that then fails for a reason waiting cannot fix leaves the queue, and its failure is on the package entry, as for any install.
4. **It is visible and can be cancelled.** `GET /v1/pkg/install-queue` lists it (`cixctl pkg queue`), and `DELETE /v1/pkg/install-queue/{image}/{name}` removes one entry. The daemon announces changes to the queue (ADR-0339).
5. **A hostbuild is not queued.** The host roll owns when one runs (ADR-0327).

## Alternatives considered

- **Declare the package in the image manifest and queue the image.** Rejected: it changes what an install means, and on the default policy it does not drain.
- **Retry in cixctl.** Rejected: it dies with the terminal, the dashboard and agents do not get it, and nobody else can see it.

## Consequences

- Installing an image's packages back to back runs them all, each when it can. #701's measurement, 27 packages into one image, is the case.
- The queue is a third list of pending work, beside the rebuild queue and running jobs. It is the only one that holds an operator's request rather than a derived state, which is why it must be persisted and visible.
