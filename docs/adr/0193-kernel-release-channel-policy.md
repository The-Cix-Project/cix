# 0193 — Which kernel line a box tracks is a setting, and it reports rather than acts

## Status

Accepted

Builds on [ADR-0188](0188-per-package-rolling-policy.md)'s position that version-selection policy is operator state rather than recipe content. The kernel is a package; this adds the one thing a package policy cannot express — which upstream *line* the pin is supposed to belong to.

**Partly superseded by [ADR-0255](0255-a-recipe-is-a-rule-not-a-version.md).** One sentence of the Decision below — *"The channel reports; it does not act."* — no longer holds. It was written because acting meant recording a `pkg_sha256` the daemon computed over its own download; the section "On the checksum question this deliberately does not answer yet" names the way out and calls the missing keyring a real follow-on. That keyring now exists ([ADR-0254](0254-upstream-checksums-are-verified-not-computed.md)), so a channel may act. **Everything else here stands**, in particular that `longterm` resolves within the line you are already on, that null is a real answer, and that the running kernel comes from `uname()` rather than the recipe.

## Context

This platform pinned one kernel version in one recipe and said nothing else about it. That single number carried two facts implicitly and reported neither: which release line it belonged to (longterm, stable, mainline), and how far behind that line it had drifted.

The question that surfaced it was an operator asking, plainly, "should we be on 7.2?" — and nothing in the platform could answer. At that moment kernel.org's mainline was 7.2, stable was 7.1.9, longterm was 6.18.45, and this project sat on 6.18.40: the right *line* and five point releases behind it. Both halves of that sentence were true, neither was visible anywhere, and working them out meant a human reading kernel.org.

A cautious box and an aggressive one want different answers here, which is exactly the shape of a setting rather than a constant.

## Decision

**The kernel line is a per-box channel, and it is `pinned` by default.**

`GET`/`PUT /v1/system/kernel-policy` with `channel` one of `pinned`, `longterm`, `stable`, `mainline` — kernel.org's own monikers, not names invented here. `pinned` is today's behaviour unchanged: the version lives in the kernel recipe and nothing proposes moving it.

**Resolution comes from kernel.org's own `releases.json`.** No copy of that data exists in this repo. A mirror of a fact that changes weekly, in a file nobody has a reason to open, is a second source of truth by construction — it would be wrong within a month and nothing would notice.

Three things follow from taking the reporting seriously:

**Null is a real answer.** `resolved_version` and `behind` are null until a refresh has succeeded, and null on `pinned`. A box that has never asked is not a box that is up to date; rendering it as `behind: false` would be the single wrong answer that reads as reassuring. Three states, not two.

**`longterm` resolves within the line you are already on.** kernel.org lists six longterm lines simultaneously, so the moniker alone does not name a version. Taking the newest would silently propose a 5.15 → 6.18 jump to a box deliberately sitting on an older longterm line. It falls back to the newest only once kernel.org stops listing your line, and `running_series_maintained` says which of those happened — the fallback is reported, not disguised as a normal resolution.

**The running kernel comes from `uname()`, not the recipe.** The pin says what was last built; `uname()` says what actually booted. After a failed A/B update those differ, and the version an operator needs to compare against is the one that is running.

**The channel reports; it does not act.** Selecting one never rewrites the recipe pin and never fetches anything. Only `POST /system/kernel-policy/refresh` reaches the network, and it returns `202` — the fetch is a forked `curl` watched by a pidfd, exactly like every other outbound fetch here, because a blocking call would stall the whole control plane behind a DNS timeout on a box with no upstream resolvers ([ADR-0076](0076-host-dns-resolver-config.md)), which is precisely the wedge [ADR-0189](0189-control-plane-stall-watchdog.md)'s watchdog exists to catch.

### On the checksum question this deliberately does not answer yet

The issue behind this ADR raised a real problem with going further: an auto-generated pin means the daemon records a `pkg_sha256` it computed by downloading the tarball itself, which changes that checksum's meaning from "an operator verified this out of band" to "whatever arrived first". That is a downgrade of a security property, and it should not happen as a side effect of a convenience feature.

Not accepting the downgrade is the decision here. Reporting is genuinely useful on its own — it answers the question that prompted this — and it is complete without touching the pin.

There is also a better option than the three the issue listed, found while building this and recorded for whoever picks it up: kernel.org publishes `sha256sums.asc` per release directory, carrying its own checksum for every tarball. A proposed pin can quote **kernel.org's published checksum** rather than one computed here, which is neither trust-on-first-use nor a claim of out-of-band verification — it is the same thing an operator does by hand. It is PGP-clearsigned, and verifying that signature needs a keyring this platform does not have yet; that is the follow-on, and it is a real one rather than a hedge.

## Consequences

An operator can now ask "am I behind, and behind what" and get an answer from the box rather than from a browser. The recipe pin remains the only thing that decides what gets built, so nothing about how a kernel is produced or rolled out changes.

The cached `releases.json` is kept on disk, which means the answer survives a restart and a reboot without the network — and also means it can be arbitrarily old. `releases_fetched_at` is reported for exactly that reason; an answer with no date on it invites being read as current.

Six longterm lines is kernel.org's choice, not a stable property. If it ever lists none, or changes the moniker vocabulary, `kernelpolicy_ingest_releases()` will find no usable release and keep the previous answer rather than resolving to nothing — a stale true answer beats an empty fresh one, and the rejection is visible in `last_refresh_error`.
