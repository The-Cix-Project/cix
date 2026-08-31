# 0218 — The API is the daemon, and it decides what each channel exposes

## Status

Accepted. Extends the API-First Mandate (`CLAUDE.md`) from a rule people follow
into a property the build enforces. Prerequisite for [#182](https://git.home.arpa/itdlabs/cix/issues/182),
which regroups the API into five lifecycle domains and would otherwise be
re-typed by hand across three surfaces.

## Context

The API-First Mandate already says the right thing: the daemon is the only
process touching the runtime, and the CLI and dashboard are pure REST clients.
What it does not say is how anyone *knows* the three agree. Today nothing
checks, and three separate hand-maintained things must stay in step:

- `daemon/src/main.c` — a hand-written dispatcher of `strcmp`/`strncmp` chains
- `cli/src/main.c` — hand-written path literals with `%s` interpolation
- `web/app.js` — hand-written paths, some template literals, some string
  concatenation

### The measurement, including the part that matters most

Coverage was measured three times, by inspection, and gave three different
answers:

| attempt | method | CLI-only | web-only |
|---|---|---|---|
| 1 | string literals | 77 | 32 |
| 2 | + template literals | 77 | 4 |
| 3 | + string concatenation | **20** | **17** |

Attempt 2 supported the confident, **false** conclusion that the dashboard
could not stop a container. It can — via
`"/v1/containers/" + encodeURIComponent(name) + "/stop"`, which that extraction
did not see.

That is the finding. The true numbers (174 spec paths, 163 CLI, 160 web, 143
shared) are less important than the fact that **coverage cannot be determined
by reading the code**. If it takes three attempts and still nearly produces a
false claim, no reviewer will do better, and no amount of discipline keeps
three surfaces aligned. Every drift found in this project so far was found by
a human noticing, late: a stale API contract, an ADR index stopped at 0214, an
artifact cache five releases behind.

### The daemon is not exempt

`docs/api/openapi.yaml` is *declared* authoritative, but the dispatcher is
hand-written, so the spec can disagree with the daemon itself. It did: the
files-API contract described a `404` on a cleanly stopped container long after
the code stopped doing that (#162). A contract that only describes intent is
documentation, not a contract.

## Decision

**One machine-readable route table, extracted from `openapi.yaml`, generates
the routing layer of all three surfaces.**

1. **`openapi.yaml` stays the single source of truth.** It is not demoted to a
   generated artifact — the Documentation Map's existing rule stands, and its
   prose is genuinely valuable. All 263 operations already carry an
   `operationId`, which becomes the join key between spec, handler, CLI
   command and dashboard call.

2. **The daemon is not a surface, and has nothing to declare.** The contract
   and the daemon are the same statement: every operation in `openapi.yaml`
   has exactly one handler, generated unconditionally, with no opt-out and no
   extension field governing it. There is no valid state in which the daemon
   does not serve what the contract says it serves, so there is nothing to
   express. Treating the daemon as one of three declarable surfaces — as an
   earlier draft of this ADR did — smuggles in the idea that it might
   legitimately not implement something, which is exactly the circumvention
   API-First exists to forbid.

3. **Only the presentation channels opt in**, via `x-cix-expose`
   (`[cli, web]`, either, both, or empty). Not every capability belongs in
   every channel — a dashboard has no business exposing `pkg hostbuild`, and
   the CLI needs no `esp/entries` view. **The API decides this**, in one
   place, rather than each channel deciding for itself and drifting. The 20
   CLI-only and 17 web-only paths become either **declared** or **failures**,
   never silently either.

4. **A generator, built with TCC, emits the routing layer**:
   - the daemon's dispatch table — `(method, pattern) -> handler symbol`
     derived from `operationId`, for **every** operation without exception.
     **An operation whose handler does not exist becomes a link error**, and a
     handler no operation names becomes unreachable, because dispatch happens
     only through the table. That pair of properties is what makes the
     contract and the daemon one thing rather than two that agree.
   - a coverage assertion per presentation channel: every operation whose
     `x-cix-expose` names that channel must be reachable from it.

5. **Only routing is generated. UX is not.** Commands, flags, wording, page
   layout and the dashboard's information design stay hand-written. Generating
   those produces a worse CLI and a worse dashboard, and this decision does not
   pretend otherwise.

6. **The extractor is deliberately restricted, and fails loudly.** It reads
   only `paths:` → method → `operationId`/`x-cix-expose`, an
   indentation-regular subset — not general YAML, which this project has no
   parser for and does not need one for. It **refuses to skip** anything it
   does not recognise. A generator that silently mis-parses its input would be
   the worst possible outcome here: a tool confidently wrong about its own
   subject, which is the exact failure class this ADR exists to remove.

## Consequences

**Drift becomes a build failure rather than a discovery.** The property is
sufficiency, enforced the way ADR-0199 enforces build tools: *"Sufficiency is
enforced by the build. Minimality is review, not enforcement."* Whether an
endpoint *should* exist on a surface stays a judgement; whether a declared one
*does* stops being one.

**The dispatcher becomes generated, which is the largest part of this, and the
non-negotiable part.** Its handlers are untouched — only the table and the
matching move. Generating CLI and dashboard alone was considered and rejected:
it leaves the spec able to disagree with the code that serves it, which is
where #162's stale contract came from, and it would fix two thirds of a
three-sided problem while leaving the one side everything else depends on
unenforced.

**#182 becomes tractable.** Regrouping ~174 endpoints into five lifecycle
domains, by hand, across three surfaces is the highest-drift-risk operation
this project has attempted. With one table it is an edit to the table plus
whatever UX each surface deserves.

**A build-time generator is new to this project.** The Makefile has exactly one
existing exception — a `git describe` for the version string, documented at the
point it happens. This adds a second, and it is a real cost: a generation step
is a thing that can itself be wrong. It is accepted because the alternative has
already been measured and it does not work.

## Alternatives considered

**A conformance test instead of generation.** Cheaper, and it would catch
divergence — but only by parsing three hand-written surfaces, which is exactly
the operation that produced three different answers above. A checker built on
unreliable extraction inherits the unreliability, and a green check would be
worth less than no check.

**Generate `openapi.yaml` from a JSON manifest.** Machine-first, and it removes
the restricted parser. Rejected because the spec's prose is the API's
documentation for humans, and moving it into JSON string literals to satisfy a
tool makes the most-read document harder to write and worse to read.

**Generate the CLI and dashboard fully, commands and all.** Rejected: it
produces a uniform, mechanical interface. A good CLI is not a transliteration
of a REST API, and neither is a good dashboard. The line is drawn at routing on
purpose.

**Leave it to discipline.** This is what has been in force, and the record is
three index drifts, a five-release artifact gap and a stale contract — all
found by a person noticing, none by a check. One of those was written by
someone who had re-read the rule hours earlier.
