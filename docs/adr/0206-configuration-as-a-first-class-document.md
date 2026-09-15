# 0206 — Configuration is a first-class, renderable, replayable document

## Status

Accepted, and implemented for the sections one setter owns. Parts 1 (`GET /v1/config`), 4 (`cixctl show running-config`) and 5 (the schema generating the section vocabulary) were built first. Parts 2 (apply) and 3 (diff) followed in [ADR-0292](0292-configuration-applies-section-by-section.md), which builds both for the eleven sections a single setter replaces and diffs -- but deliberately does not apply -- the twenty-two whose application means reconciling individual live resources. Part 3 landed as `POST /v1/config/diff` rather than the `GET` named below: a GET cannot carry the document being compared. See that ADR for what remains and why it is a separate decision rather than remaining work of the same kind.

Originally proposed. Requested by the owner: *"I want to be able to do a show running-config type of thing? Old school Cisco-ey... think of 2000s Cisco style configuration, and I think we need the exact same style but much more enhanced since we're 2026."*

## Context

A Cix host's configuration is real and substantial — containers, networks, DNS records and forwarders, DHCP ranges and reservations, PKI, LDAP, NTP, syslog targets, disks and roles, sysctls, kernel modules, swap, the resolver, site identity, package state. Every piece of it is reachable over REST, and that is genuinely good: it is all inspectable, all scriptable, nothing hidden in a file the daemon does not own.

What does not exist is a way to see it **as one thing**. An operator asking "what is this box actually configured to do?" has no answer short of visiting a dozen endpoints and assembling the picture themselves. That is the question a network engineer answers in one command, and has since the 1990s:

```
Router# show running-config
```

The reason that command has outlived nearly everything else from its era is not nostalgia. It is that a single, ordered, legible, complete text rendering of device state turns out to be the right primitive for an enormous number of real tasks: reviewing a change, diffing two devices, capturing state before touching something, handing a config to a colleague, storing it in version control, and — with `configure replace` — putting the box back exactly the way it was.

Cix has a `system backup`, but that is a different tool for a different job: an opaque bundle for restoring a machine. It is not something a human reads, reviews in a pull request, or diffs against yesterday.

The 2026 version of this should keep what made the original good and drop what made it painful:

| kept | dropped |
|---|---|
| One command, complete picture | Text-only — no machine-readable form |
| Ordered, sectioned, legible | Config that drifts from what the device is actually doing |
| Replayable as commands | Line-by-line application with no atomicity |
| Diffable | No notion of "what changed since I got here" |
|  | Secrets printed in the clear |

## Decision

**Configuration becomes a first-class document, rendered from live state, never stored as a second copy of it.**

Four parts:

**1. `GET /v1/config` returns the canonical configuration document.** Structured, complete, ordered, redacted — that is the contract. The *document* is the daemon's; the *presentation* is not. Cisco-style text, sectioning, colour and paging are rendered by `cixctl`, and the dashboard renders the same document its own way.

That line was drawn deliberately after an initial draft put the text rendering server-side too. The argument against: presentation is the part that gets iterated most, and on this platform a daemon change means a rebuild, an A/B slot write and a reboot — an absurd loop for moving a section heading. The argument for keeping the *document* server-side is unchanged and decisive (below). Text is never parsed back, so its format is not a contract at all, which leaves the CLI free.

The critical constraint, and the reason this is an ADR rather than a feature ticket: **the running config is derived, never authoritative.** The moment a rendered config is stored and treated as the truth, this project has two sources of truth for every setting and a drift problem it will never fully close — the exact failure mode `One Source of Truth` exists to prevent, and one this project has already been bitten by in its documentation. The daemon's own state remains the only truth; `GET /v1/config` is a *view*, exactly like the dashboard.

**2. Secrets are never rendered.** PKI private keys, LDAP bind passwords, repo and artifact tokens are shown as a redaction marker, never a value. A config document is the single most likely artifact to be pasted into a ticket or committed to git, so this is a hard rule, not a default.

**3. `POST /v1/config` applies the structured document, all-or-nothing.** Structured only — accepting rendered text back would create a second input path competing with this one, and text-parsing is the most fragile part of the model being borrowed. This is the real 2026 enhancement and the reason the rendering must be replayable rather than merely pretty. It is `configure replace`, not `configure terminal`: the document describes the intended end state, the daemon computes the difference against live state, and either the whole change applies or none of it does. A config that can be rendered but not applied is a report; one that round-trips is infrastructure-as-code, with the daemon — not a client, not a separate tool — owning the reconciliation.

**4. `GET /v1/config/diff` answers "what has changed."** Against a supplied document, or against the config as of last boot. This is what makes a config document useful during an incident rather than only during a review.

**5. The schema generates the vocabulary; nothing hand-maintains a second copy of it.** This is the owner's hard requirement on the whole design, and it outranks every syntax question: *"it has to be 100% aligned and tied to the API with no exception... that same api code would dictate the config code."*

Agreement between two hand-maintained things is precisely what drifts, and every drift this project has suffered has that shape — `openapi.yaml` against `api/README.md` (ten phases stale), a recipe's approved checksum against the artifact it approves (#145), ADR-0203 against the code it described, `esp.c`'s ordering model against systemd-boot's actual behaviour. A config vocabulary maintained beside the API would be the same failure with a larger surface.

So the `ConfigDocument` schema in `openapi.yaml` — already this project's declared source of truth for the contract — generates the daemon's render/apply scaffolding, `cixctl`'s config vocabulary and completion, and (fetched at runtime via `GET /v1/config/schema`) the dashboard's rendering. Runtime for the web client specifically, so a dashboard cannot go stale against the daemon it is talking to.

Enforced rather than intended, because a rule nothing checks erodes: a build-time check that every subsystem registering config state appears in the schema and vice versa, so adding a subsystem and forgetting fails the build instead of shipping a quietly partial document.

Cisco-style sectioning and ordering are deliberately imitated, because dependency order is real: networks before the containers attached to them, images before packages, DNS servers before the records that reach them. A rendering whose order cannot be replayed is not replayable, so ordering is part of the contract, not cosmetic.

## Consequences

Every subsystem that holds configuration must be able to render and accept it. That is real work, and it is worth being explicit that it grows with the platform: **any new configurable subsystem must join the config document, or the document silently becomes a partial truth** — which is worse than not having it, because its whole value is completeness. This is the main cost of the decision and the thing most likely to erode.

`GET /v1/config` also becomes the natural answer to several existing awkwardnesses: capturing state before a risky change, comparing two hosts, and reviewing what a `dns provision` or a recipe apply actually did.

The relationship to `system backup` should stay sharp rather than blur: **backup is for machines** (opaque, complete, includes state a human should not edit); **config is for people** (legible, reviewable, diffable, redacted). Neither replaces the other, and merging them would produce something bad at both jobs.

**There is no `startup-config`, and that question is settled rather than deferred.** Cisco's split exists because IOS devices lose configuration on reload unless it is written to NVRAM. A Cix host persists everything continuously, so a startup config would be imitating a limitation this platform does not have — copying the shape of the model instead of the substance.

The genuinely useful half of "two configurations" is the JunOS/IOS-XR model rather than the IOS one, and it costs nothing because it falls out of the client's edit buffer:

- **running-config** — live state, `GET /v1/config`
- **candidate** — the client's buffer while in config mode, never persisted server-side
- **startup-config** — does not exist

Configuration *history and rollback* (`rollback 3`) is a different feature that overlaps `system backup`, and should not be reintroduced under the startup-config name.

**Config mode itself is a client, which is why this decision is affordable.** Given render and atomic apply, the modal CLI is an editor over a document: `configure` fetches it into a buffer, context commands (`container blah`) push scope, settings mutate the buffer, `no ...` removes from it, and `commit` posts the whole thing for the daemon to diff and apply. Parser, context stack and per-context completion are entirely client-side; the daemon gains two endpoints. This only works because apply is atomic document-replace — line-at-a-time application would put reconciliation back in the client and reintroduce the drift this ADR exists to prevent.
