# 0292 — Configuration applies section by section, and only where one setter owns the section

## Status

Accepted. Builds parts 2 and 3 of [ADR-0206](0206-configuration-as-a-first-class-document.md) (`POST /v1/config` and the diff), partially — the half that can be built honestly today — and records why the other half is a separate problem rather than remaining work of the same kind.

## Context

ADR-0206 accepted a Cisco-style configuration document with five parts and shipped three of them: `GET /v1/config` renders one ordered, redacted document from live state; `cixctl show running-config` presents it; and the `ConfigDocument` schema generates the section vocabulary so a subsystem cannot be silently missing from it. The two it did not ship were the write direction — applying a document, and saying what a document would change.

Nothing about the read side made the write side easy. Measured while scoping this:

- **`daemon/src/config.c`'s table is render-only** — `{name, void (*)(struct json_writer *)}`, one function per section, each calling a subsystem's existing `_write_json`. There is no parse, no compare, no apply anywhere in it, and the generated `CIX_CONFIG_SECTIONS(X)` macro carried only names, so nothing in the generated surface constrained a writer.
- **The only whole-system "apply" this platform had, `do_system_restore()`, is not a reconciler.** It writes raw state files (`container_defs.json`, `networks.json`, …) straight from a backup body. It never compares against live state, never creates or deletes a real resource, and never orders anything. Useful for what it does; no help at all here.
- **The closest real reconcile logic, `handle_container_recipe_apply()`, only creates.** It renders a declarative definition and calls `handle_create()` when the name is absent. No update-in-place, no delete.

So the write direction is new work in every section, and the sections are not alike: `resolver` is one list behind one setter, while `containers` is twelve live resources with dependencies, running processes and destructive deletes. Treating those as one feature is what would make this a project rather than a change.

## Decision

**A section declares, in the schema, how it may be written; only the sections one setter owns are applied today.**

Three attributes join each `ConfigDocument` property, and `apigen` refuses a section missing any of them:

- `x-cix-config-kind` — `object` or `array`, which the differ needs before it can compare anything.
- `x-cix-config-key` (arrays only) — the field, or comma-separated fields, that NAME an element. `""` means compare by position.
- `x-cix-config-apply` — `replace` when one setter replaces the whole section, `reconcile` when applying it means creating, updating and deleting individual live resources.

Eleven sections are `replace` (site, daemon, resolver, time, zswap, swap, backup, dns_forwarders, ldap, package_repo, package_artifacts); twenty-two are `reconcile` and are **diffed but never applied**. A supplied `reconcile` section with real changes refuses the whole request, with nothing touched — applying the appliable half of a document and reporting the rest as skipped would leave a host matching neither the document nor what it had before.

Four decisions inside that, each of which could reasonably have gone the other way:

**1. The generated list, not a hand-written one, decides which sections have appliers.** `apigen` emits a second macro, `CIX_CONFIG_SECTIONS_REPLACE(X)`, and `api_config.c` expands it three times: to declare the apply functions, to define the table, and to look one up. A section the schema calls `replace` with no function does not link; a function for a section the schema no longer calls `replace` is an unused static, which is an error under `-Werror`. The schema and the code cannot drift in either direction, which is the same structural enforcement ADR-0206 chose for the renderer rather than a review habit.

**2. `POST /v1/config/diff`, not `GET`.** ADR-0206 wrote `GET /v1/config/diff`. A GET cannot carry the document being compared, and a GET with a request body is not something this daemon's router, CLI or dashboard assume anywhere else. The endpoint computes and returns; it changes nothing, which is the property that mattered.

**3. Arrays are matched by a declared key, and the fallback is reported.** Comparing two arrays by position reports one insertion at the front as "everything after it changed" — for the 167-entry `packages` section, a diff nobody can read. Matching by an identity key fixes that and introduces its own failure: a key that does not uniquely name an element pairs up the wrong ones, authoritatively. So when a declared key is not unique in the document at hand, the comparison falls back to position and says so in the response (`compared_by_position`), rather than answering confidently from a wrong pairing.

**4. A `replace` section is supplied whole.** Every field it renders must be present and nothing else may be. An absent field is an error, never "leave this one alone" — that rule is what makes the document you fetch and the document you send the same language, and it means a typo'd field name is an error rather than silently becoming "no change".

## What this does not promise

**Dry-run proves shape, not values, and the code says so where it matters.** Both endpoints build the same plan through the same appliers, called with a `dry_run` flag — so the appliability an operator is shown comes from the code that will do the work. What that checks is the document's shape: fields present, types right, no attempt to change a derived value or a redacted secret. It cannot check whether a subsystem will accept the values, because none of them offer a way to ask without doing: `siteconfig_set()` validates a domain suffix as it persists it, `zswap_set()` writes kernel parameters the kernel may refuse. **An apply can therefore still fail after its plan validated**, and the response reports per section what was applied and which one stopped it, rather than claiming an atomicity that does not exist.

**The document mixes configuration with running state, and that limits full-document apply.** A container element carries `pid`, `status`, `exit_status`, `captured_output`; `dhcp_servers` carry `running`; `volumes` carry `created_at` and `backup_last_at`; `networks` carry their current `address`; `disk_roles` carry `present`. None of that is configuration. The render is stable at rest — two fetches of the whole document from 192.168.15.95 minutes apart differed in zero paths, measured — so this is not a nondeterministic renderer. But a container restarting between fetch and apply moves `pid`, `containers` is a `reconcile` section, and the whole request is then refused for a reason that has nothing to do with what the operator edited.

So the contract is: **apply operates on the sections you supply, and supplying only what you are changing is the normal way to use it** (#471 asks whether the document should carry state at all). `cixctl config apply --section=NAME` exists to make that one step rather than hand-edited JSON, and both the endpoint description and the CLI help say why rather than treating it as a style preference.

**A derived field inside a `replace` section makes a stale document unreplayable, and that is #471 again.** Measured on 192.168.15.95 while verifying this: `zswap` renders `max_pool_percent` (intent) alongside `kernel` (what the kernel currently has, ADR-0196's deliberate split). Raise the percent, then replay the document fetched *before* that change, and it is refused — correctly, since its `kernel` mirror is now stale — but the refusal names `kernel` when the operator edited `max_pool_percent`. The messages now end with what to do about it ("re-fetch the document if yours predates a change made elsewhere"), which is a fix for the confusion and not for the cause. The cause is that a section carrying both intent and observation cannot be round-tripped across a change to itself, and that is the same mixing [#471](https://git.home.arpa/itdlabs/cix/issues/471) describes, appearing inside a section that is otherwise straightforward.

**Secrets have no spelling in this document, which is what makes applying one safe.** The document renders a token or password as a set/not-set boolean. Every applier reaching a setter that takes a secret passes NULL, and all three such setters define NULL as "leave this field unchanged" (`pkg_repo_set_config`, `pkg_artifact_set_config`, `ldap_config_set_client` — checked in their headers before this was written, because the alternative reading, that an omitted secret clears it, would mean changing a repo URL through this document silently broke every fetch through it). Sending back a redaction marker that differs from live is refused by name, rather than interpreted.

## What was measured

Verified live on 192.168.15.95 (v2.57.177), because "eleven sections apply" is a statement about the code and not, on its own, evidence:

- The whole live 33-section document diffed against itself: **zero changes, zero blocked** — which exercises every renderer, the keyed array matching for all 22 array sections, and the round-trip the whole design rests on.
- **Eight of the eleven appliers ran for real**, each confirmed through its own subsystem's endpoint and then restored: `resolver`, `time`, `dns_forwarders`, `backup`, `site`, `package_repo`, `package_artifacts`, `zswap`. `package_repo`'s `auth_token_set` was still true afterwards, which is the secret-preservation claim above measured rather than reasoned.
- **Nine of eleven, once `swap` became testable.** `swap` was initially unreachable: host swap could not be enabled on that machine at all, because btrfs refuses a copy-on-write swapfile ([#472](https://git.home.arpa/itdlabs/cix/issues/472)) — found by this verification, fixed in `swap.c`, and then both of the swap applier's guards were measured. A `size_mb` change against enabled swap is refused with "disable it and enable it again at the new size", nothing touched; the disable-then-enable it points at works through this endpoint in both directions.
- **Two were deliberately not exercised on the live host**: `daemon` rebinds the listener the request arrives on (and that host has no shell to recover through), and `ldap` reloads the directory server this session authenticates against.
- All-or-nothing, proven rather than asserted: a **changed** `resolver` sent in the same body as an edited `containers` section returned 400 with `applied: false` on both, and `GET /v1/system/resolv` confirmed the resolver was untouched afterwards.
- The refusals are specific and were each produced on the box: unknown section, a missing field, a derived field (`zswap.supported`), a redaction marker (`auth_token_set`), an unknown field.
- The "an apply can fail after its plan validated" path was exercised for real, unintentionally: the swap enable above validated, then failed in the setter, and came back 500 with `applied: false` and the subsystem's reason on that section — which is exactly what this ADR says happens.

## Consequences

- An operator can now ask what a configuration document means before anything acts on it, and can apply the eleven sections one setter owns — identity, resolver, time, sysctl-adjacent kernel settings, swap, backup destination, directory configuration and the package repo/artifact locations.
- **Reconcile-mode apply is a separate decision, not remaining work of this kind.** Before those twenty-two sections can be applied, two things need answering that this ADR deliberately does not: what "all or nothing" means when deleting a container fails halfway through a plan ([#470](https://git.home.arpa/itdlabs/cix/issues/470)), and whether the ConfigDocument should separate configuration from running state ([#471](https://git.home.arpa/itdlabs/cix/issues/471)) — which the mixing above makes a prerequisite, not a cleanup.
- The three new schema attributes are required, so adding a configurable subsystem to this platform now means declaring how it may be written, not only that it exists. That is a slightly higher bar for a new section and a deliberate one: a section whose write semantics are unstated would default to something, and a default here is a decision nobody made about how an operator's document may change a live host.
- `jsondiff.c` is a general structural differ with no configuration knowledge in it, gated by `test_jsondiff` in `SELFTESTS` — pure computation, so it runs as a build gate on a Cix host (#224) rather than only where the tree happens to build.
