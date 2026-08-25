# Cix Brand Guidelines

_Source-native systems. Directly._

Comprehensive strategic, verbal, visual, product and governance system for the rebrand from ThinC to Cix.

**Version 1.0 — 24 August 2026**

> Brand system status: strategic and operating system defined. Final custom logo artwork remains subject to dedicated identity exploration and optical refinement before production lock.

<!--
  Source of truth: the owner's Cix_Brand_Guidelines_v1.0.docx (this file is
  its faithful markdown transcription; content authority stays with the
  owner's document — edits here follow a new version from them, per the
  Documentation Map). Logo/icon SVG assets to be added alongside when
  provided. Rebrand execution itself is tracked as issue #121 and is NOT
  scheduled by this file landing.
-->

## How to use this document

This is a decision system, not a mood board. It defines what Cix means, how it behaves, how it speaks, how it looks, and how those rules scale from a shell prompt to a public release.

---

> AUTHORITY — When a local design preference conflicts with a core principle in this document, the principle wins. Exceptions should be intentional, documented and reversible.

### Contents

01. Brand thesis and executive definition

02. Why Cix / rebrand rationale

03. Purpose, mission, vision and promise

04. Positioning and category

05. Audience and user mindsets

06. Brand principles

07. Personality and behaviour

08. Naming, pronunciation and grammar

09. Verbal identity and editorial voice

10. Messaging architecture

11. Brand architecture and resource taxonomy

12. Visual identity strategy

13. Logo system and identity exploration brief

14. Colour system

15. Typography

16. Grid, spacing and composition

17. Iconography and symbols

18. Diagrams, schematics and data visualisation

19. Imagery and illustration

20. Motion, boot and interaction language

21. CLI and shell experience

22. Product UI system

23. Documentation and developer experience

24. Website and launch expression

25. Repository, package and release identity

26. Physical, hardware and environmental applications

27. Accessibility and inclusive design

28. Governance and quality control

29. ThinC → Cix migration system

30. Launch sequence and rollout

31. Copy bank and application examples

32. Brand QA checklists and decision tests

33  Appendix A Terminology

34  Appendix B Asset inventory

35  Appendix C Open decisions and production gates

### Document conventions

| TERM | MEANING |
|---|---|
| MUST | Non-negotiable brand behaviour. Use unless a formally documented exception exists. |
| SHOULD | Default practice. Deviate only when the context has a clear functional reason. |
| MAY | Permitted option within the system. |
| AVOID | Not automatically forbidden, but usually weakens the brand. |
| NEVER | Incompatible with the Cix identity or operating philosophy. |

## Brand thesis and executive definition

Cix should feel like software that belongs close to the machine: concise, inspectable, deliberate, source-native and structurally coherent.

---

### The one-sentence definition

> CANONICAL DEFINITION — Cix is a source-native systems distribution built around direct Linux primitives, a C control plane and a consistent resource model for workloads, networks, hardware and system services.

### The short definition

A source-native systems distribution where C is the control plane.

### The brand thesis

Cix exists for operators and builders who want less distance between intent and mechanism. The system should not romanticise complexity or conceal it behind layers that cannot be inspected. It should expose coherent primitives, compile from source, present capabilities as resources, and make the route from command to kernel behaviour understandable.

The brand is therefore not “minimalism” as an aesthetic. It is directness as an engineering discipline. The visual identity, copy, CLI, product UI, documentation and release process should all make that discipline visible.

### What Cix is

• A full distribution, not merely a container runtime.

• A source-native operating system environment, not simply “open source”.

• A systems product with a strong C identity, without turning C into nostalgia or retro computing.

• A resource-oriented control plane whose taxonomy is consistent across CLI, API, UI and documentation.

• An intentionally low-layer stack: Linux mechanisms first; abstractions only where they create durable clarity.

### What Cix is not

• A generic Kubernetes alternative.

• A “hacker aesthetic” product built from green-on-black terminal clichés.

• A tiny distro whose only proposition is small size.

• A retro UNIX tribute brand.

• A SaaS dashboard with systems terminology painted on top.

• An excuse for opacity, undocumented behaviour or cleverness for its own sake.

## Why Cix / rebrand rationale

ThinC expressed the original architecture. Cix gives the architecture a name large enough to become a distribution, command namespace and ecosystem.

---

### Strategic reason for the change

ThinC was a strong thesis-name: thin host, C at the core, minimal orchestration layers. As the system expands into packaging, hardware, networking, DNS, PKI, build tooling and first-class APIs, the name increasingly describes an implementation idea rather than the whole distribution. Cix retains the intellectual core while creating a shorter, more neutral namespace for the entire system.

### Why the name works

| RULE | GUIDANCE |
|---|---|
| System-level scale | Cix reads like the name of an operating-system family or low-level toolchain rather than a single daemon. It can credibly prefix commands, packages, services and documentation namespaces. |
| CLI ergonomics | The lower-case token “cix” is short, memorable and visually distinct at a shell prompt. It remains usable when composed with resource nouns. |
| Layered meaning | The C is explicit. “ix” quietly echoes UNIX/POSIX lineage and systems naming patterns without forcing a literal acronym. |
| Container resonance | The name can carry a secondary association with containerised execution without making “container” the entire category. |
| Visual economy | Three letters allow a wordmark to remain legible at favicon, TTY, package prefix and hardware-label scale. |

### Name interpretation rule

> DO NOT OVER-EXPLAIN THE ACRONYM — Cix is a name first. The C / UNIX / POSIX / containers reading is useful founder logic, not mandatory customer-facing decoding. Avoid backronyms and “C stands for…” paragraphs in routine marketing.

### Recommended pronunciation

Pronounce Cix as “six” /sɪks/. The pronunciation is fast, unambiguous in speech and aligned with the simple lower-case spelling. In formal first-use contexts, “Cix (pronounced six)” may be used once. Do not stylise the spelling to force pronunciation.

### Trademark and naming gate

Pre-launch legal gate — Before public launch, conduct professional trademark, company-name, domain and package-namespace clearance in all intended jurisdictions and ecosystems. This guideline defines brand use; it is not a legal clearance opinion.

## Purpose, mission, vision and promise

These statements are internal operating tools. They should guide product and communication decisions without becoming corporate wallpaper.

---

| ELEMENT | CIX DEFINITION |
|---|---|
| Purpose | Reduce unnecessary distance between a systems operator and the mechanisms that make the system work. |
| Mission | Build a source-native distribution in which workloads, networks, hardware and essential services are controlled through small, coherent, inspectable C systems. |
| Vision | Infrastructure that can be understood end-to-end by the people who operate it. |
| Promise | Cix will prefer direct, explainable mechanisms over fashionable layers when both can solve the problem reliably. |
| Enemy | Unnecessary indirection: layers, language and interfaces that obscure causality without creating proportionate value. |

### The promise test

A feature, dependency, abstraction or message belongs in Cix when it either reduces operational complexity or makes necessary complexity more legible. “More features” is not automatically progress. “Fewer layers” is not automatically progress either. The decision is whether the system becomes easier to reason about without sacrificing essential capability.

## Positioning and category

Cix should occupy the territory between bare Linux assembly and heavyweight infrastructure platforms: a coherent distribution for direct systems operation.

---

### Positioning statement

> INTERNAL POSITIONING — For systems engineers and infrastructure operators who want control without orchestration sprawl, Cix is a source-native systems distribution that exposes Linux mechanisms through a consistent C control plane and first-class resource model. Unlike heavyweight platforms that add layers to normalise infrastructure, Cix keeps the route from source to running system deliberately short and inspectable.

### Category language

• Primary: systems distribution.

• Secondary: source-native infrastructure distribution.

• Technical description: Linux-based host and workload orchestration distribution with a C control plane.

• Avoid leading with: hypervisor, container platform, Kubernetes replacement, cloud OS, DevOps platform. These can describe contexts, but they should not define the category.

### Differentiation pillars

| RULE | GUIDANCE |
|---|---|
| Source-native | Compilation is part of the operating model and identity, not an inconvenient installation step. |
| C control plane | Core runtime, networking and control services privilege small native components and direct interfaces. |
| Resource consistency | The same nouns and lifecycle logic should recur across CLI, API, UI and documentation. |
| Linux primitives | Namespaces, cgroups, OverlayFS and other kernel facilities are treated as mechanisms to use directly and explain clearly. |
| Inspectability | Operators should be able to discover what exists, what changed, why it changed and which mechanism implements it. |

## Audience and user mindsets

Design for technical seriousness without assuming every user wants obscurity. Cix should reward expertise and accelerate comprehension.

---

| MINDSET | WHAT THEY NEED |
|---|---|
| Primary operator | Systems engineer, SRE, platform/infrastructure engineer or homelab operator who values explicit control and dependable primitives. |
| Primary builder | C systems programmer or infrastructure developer extending Cix, building packages, adding resource types or integrating hardware. |
| Evaluator | Technical lead or architect comparing Cix with conventional Linux distributions, hypervisors, container hosts or orchestration stacks. |
| Reader | Developer consuming documentation or source before ever installing the system. |
| Community contributor | Contributor who needs naming, code, documentation and release conventions that make the project internally coherent. |

### Psychographic traits

• Values causality over convenience theatre.

• Accepts complexity when it is intrinsic; rejects complexity that exists only to support abstraction.

• Reads source, logs and API responses.

• Not impressed by inflated claims or decorative dashboards.

• Wants defaults that are coherent, not magical.

• Prefers tools that remain legible under failure.

## Brand principles

These are the five invariants of the Cix identity. Product and communication decisions should be traceable to at least one of them.

---

### 01 SOURCE IS THE PRODUCT

Cix is not simply distributed with source available. Source, dependency selection, compilation and build provenance are part of the user’s relationship with the system. Make source visible in language, interfaces and release artifacts.

### 02 C ALL THE WAY DOWN

C is a material choice and systems philosophy: small binaries, explicit interfaces, direct control, predictable runtime behaviour. Use this with confidence, but never as language-war bait.

### 03 MECHANISM OVER MACHINERY

Prefer the smallest coherent mechanism that solves the operational problem. Avoid layers whose main function is to compensate for earlier layers.

### 04 EVERYTHING IS A RESOURCE

Containers, networks, devices, DNS records, certificates and services should feel like members of one system, not unrelated product modules.

### 05 BUILT TO BE UNDERSTOOD

A Cix system should remain inspectable during normal operation and failure. Names, logs, APIs, diagrams and UI must reveal relationships rather than obscure them.

### Decision heuristic

EVERY PRODUCT/DESIGN DECISION

> Does this expose or obscure the mechanism? — Does this reduce or add indirection? — Does the name match the resource model? — Can the operator explain what happened? — Can the system show its work?

## Personality and behaviour

Cix should feel composed rather than performatively aggressive. Technical confidence comes from precision, not swagger.

---

| TRAIT | BEHAVIOUR |
|---|---|
| Precise | Say exactly what a thing does, especially in errors, docs and release notes. |
| Direct | Use active verbs, concrete nouns and visible system state. |
| Low-level | Stay comfortable near kernel, hardware and source concepts without making the interface cryptic. |
| Composed | Avoid alarmism, hype, exclamation marks and visual noise. |
| Deliberate | Every dependency, colour, animation, label and abstraction should have a reason. |
| Dryly human | Small moments of wit are welcome in community contexts, but never at the expense of comprehension. |

### The Cix spectrum

| CIX IS | CIX IS NOT |
|---|---|
| Confident, not macho | No “blazing fast”, “insane performance”, war metaphors or chest-beating. |
| Minimal, not barren | Remove decoration; retain context, explanation and affordance. |
| Technical, not cryptic | Use correct terms, then help the reader navigate them. |
| Industrial, not dystopian | Materiality and engineering cues are useful; skulls, hazard cosplay and cyberpunk clichés are not. |
| Unix-aware, not nostalgic | Reference lineage through discipline and structure, not beige-terminal cosplay. |

## Naming, pronunciation and grammar

The brand becomes stronger when every command, service, package and resource follows one grammar.

---

### Master brand

| USE | FORM |
|---|---|
| Display name | Cix |
| CLI token | cix |
| Code/package prefix | cix- when a standalone package requires a namespace |
| Possessive | Cix’s in prose; avoid awkward repeated possessives |
| Pronunciation | six /sɪks/ |
| Plural | Never pluralise the brand name. |

### Command grammar

PREFERRED CLI GRAMMAR

> cix <resource> <verb> [object] [flags] — cix container create web — cix container inspect web — cix network list — cix hardware attach gpu0 web — cix package build base — cix source sync

### Naming rules

• Prefer singular resource nouns in command namespaces: container, network, device, package, certificate.

• Prefer plain English verbs: create, delete, list, inspect, attach, detach, enable, disable, build, sync.

• Use “get” only when retrieval semantics differ materially from “inspect” or “list”.

• Do not invent branded verbs such as “cixify”.

• Reserve cix- prefixes for technical namespaces; do not prefix every concept in prose.

• Resource identifiers should be shell-safe by default: lower-case, digits and hyphens unless a subsystem requires more.

### Service and binary naming

| PATTERN | GUIDANCE |
|---|---|
| cix | Primary human-facing CLI. |
| cixd | Primary system daemon only if a single top-level daemon exists. |
| cix-<resource>d | Resource daemon pattern only when the architecture genuinely separates daemons. |
| libcix* | Libraries when language/toolchain conventions require library naming. |
| cixpkg | Avoid as default. Prefer `cix package` for human-facing operations; reserve compact binary names for technical necessity. |

## Verbal identity and editorial voice

Cix copy should sound like a good systems interface: terse where state is obvious, explanatory where causality matters.

---

### Voice rules

| RULE | EXPRESSION |
|---|---|
| Lead with the mechanism | “Attach device to container” beats “Empower your workloads with hardware acceleration”. |
| Prefer verbs | Build, inspect, attach, route, compile, mount, resolve. |
| Use adjectives sparingly | Avoid lightweight, powerful, seamless, next-generation, revolutionary, intuitive, enterprise-grade unless objectively defined. |
| Make failures actionable | State what failed, what Cix observed, and the next useful action. |
| Do not anthropomorphise | Cix does not “think”, “want”, “get confused” or “refuse to cooperate”. |
| Do not hide risk | Use explicit words for destructive operations and irreversible state. |

### Sentence rhythm

Short declarative sentences are the default. Longer technical explanations are welcome when they carry necessary causal detail. Avoid marketing fragments that create rhythm without meaning.

### Preferred vocabulary

| WORD | USE |
|---|---|
| build | compile from source when that is what actually happens |
| resource | the canonical generic noun for managed system objects |
| inspect | show authoritative detail and relationships |
| state | current machine-readable condition |
| source | the code or source tree, not a vague synonym for origin |
| host | the Cix system providing runtime and resources |
| container | use when the mechanism is a container; do not rename it “instance” to sound novel |
| device | physical or logical hardware resource when device is technically accurate |

### Words to retire

• seamless

• effortless

• magic / magically

• single pane of glass

• revolutionary

• next-gen

• cloud-native (unless specifically describing an interoperability context)

• hyperconverged

• AI-powered unless an actual Cix feature depends on AI

• blazing-fast unless backed by a benchmark and scope

## Messaging architecture

Every public message should descend from one stable hierarchy rather than inventing a new story for each page.

---

| LEVEL | MESSAGE |
|---|---|
| Level 1 / Brand idea | Systems, directly. |
| Level 2 / Product definition | A source-native systems distribution where C is the control plane. |
| Level 3 / Proof | Built from source. Linux primitives. First-class resources. Small native control components. Inspectable state. |
| Level 4 / Capability | Containers, networks, hardware, storage, DNS, PKI, packages, build and API. |
| Level 5 / Evidence | Benchmarks, dependency graphs, source links, syscall-level explanations, reproducible builds, compatibility tables and failure behaviour. |

### Tagline system

| ROLE | LINE |
|---|---|
| Primary recommendation | Systems, directly. — broad enough for the distribution and faithful to the directness thesis. |
| Source campaign | Built from source. — excellent on build/release/install material. |
| Engineering campaign | C, all the way down. — strong community/editorial line; do not make it the only definition of Cix. |
| Operator campaign | Less between you and the machine. — useful for launch pages and talks. |
| Product philosophy | Build what you run. — useful where source provenance is central. |

### Message sequencing

01  Say what Cix is.

02  Say why the direct model matters.

03  Show the mechanism or resource model.

04  Prove the claim with something inspectable.

05  Only then discuss broader philosophy or ecosystem ambition.

## Brand architecture and resource taxonomy

Cix should have one system grammar from the CLI to the web console. Taxonomy is not documentation housekeeping; it is part of the brand.

---

CANONICAL CAPABILITY MAP

> CIX — ├── system — ├── source — ├── build — ├── package — ├── container — ├── network — ├── storage — ├── hardware — ├── dns — ├── pki — └── api

### Architecture rules

• A top-level capability should exist only if it is a durable user mental model, not merely an internal module.

• Use the same noun across API paths, CLI help, navigation labels and documentation whenever technically possible.

• Do not use one term in the UI and another in the API for aesthetic variety.

• Relationships should be visible: a container can show attached networks, devices, storage and certificates without inventing separate dashboards.

• New capability names require a taxonomy review before release.

### API naming

ILLUSTRATIVE PATTERN

> /api/v1/container — /api/v1/network — /api/v1/hardware — /api/v1/dns — /api/v1/pki

> API CAVEAT — Exact API paths must follow the implemented contract. The brand rule is terminological consistency, not forcing a cosmetic URL pattern onto an architecture that needs different semantics.

## Visual identity strategy

The Cix visual system should feel machined, legible and structural: fewer decorative layers, stronger grids, explicit relationships and material colour.

---

### Visual idea

> CORE VISUAL METAPHOR — Intersection. Cix sits where source meets system, hardware meets workload, and operator intent meets Linux mechanism. The X can become a structural motif without becoming an “edgy X brand”.

### Aesthetic principles

| PRINCIPLE | RULE |
|---|---|
| Material, not glossy | Flat colour, crisp edges, tactile neutrals. No glassmorphism, sheen or gradient chrome. |
| Structural, not decorative | Rules, grids, connectors and hierarchy should reveal relationships. |
| Dense when useful | Technical density is acceptable when it increases information throughput. Density must remain ordered. |
| Monochrome first | Every layout should survive in black/white before accent colour is used. |
| Accent with purpose | Colour identifies state, category or emphasis. It does not fill empty space. |
| Quiet confidence | Large type and empty space may be used, but avoid luxury-brand preciousness or tech-startup spectacle. |

### Explicit visual exclusions

• No gradients as brand decoration.

• No neon glow or bloom.

• No faux CRT scanlines.

• No matrix-code backgrounds.

• No random hex dumps as texture.

• No generic cloud illustrations.

• No glass cards.

• No 3D chrome X symbols.

• No circuit-board traces unless the diagram communicates a real topology.

## Logo system and identity exploration brief

Do not freeze a custom mark by accident. The logo must be designed against the actual operating contexts defined here.

---

### Identity architecture

| ASSET | ROLE |
|---|---|
| Primary wordmark | Lower-case cix. This is the default public identifier and should carry most brand recognition. |
| System glyph | A compact mark derived from the geometry of c / i / x or an intersection concept. It must work at 16 px, TTY-adjacent UI scale and one-colour hardware marking. |
| CLI word | `cix` in the configured monospace typeface is a functional identifier, not a substitute for the custom public wordmark. |
| Lockup | Wordmark + optional descriptor such as “systems distribution” for unfamiliar audiences. Do not append “OS” by default. |
| Favicon/avatar | Prefer system glyph once recognised; use the wordmark where size permits until recognition is established. |

### Non-negotiable logo criteria

• Must work in one colour.

• Must remain recognisable at 16×16 px or have an intentional small-size variant.

• Must be drawable without gradients, shadows or transparency effects.

• Must not depend on enclosing the mark in a rounded-square app icon.

• Must not resemble a cryptocurrency token, gaming brand or generic “X” social brand.

• Must have sufficient distinction from Nix, Linux distributions and existing C-letter infrastructure marks.

• Must feel credible stamped into hardware, printed in a manual and shown in a shell-adjacent web UI.

### Exploration territories

| TERRITORY | DESIGN TEST |
|---|---|
| A / Pure wordmark | Typographic customisation of cix; minimal symbol dependence. Explore distinctive terminals, x intersection, aperture of c and dot/axis relationship of i. |
| B / Intersection | Construct the x as an architectural junction. The surrounding letters remain calm so the metaphor is structural rather than decorative. |
| C / Open boundary | Use c as an open containment boundary and x as crossing routes; useful if container/resource semantics can be expressed without literal box icons. |
| D / Monogram | A compact cx or c× system glyph for tiny surfaces, paired with a full wordmark. Must avoid losing the i concept unless the wordmark carries it. |
| E / Source trace | A typographic system where cuts/joins suggest compilation or assembly. Must avoid circuit-board cliché. |

### Clearspace and production rule

> PENDING FINAL ARTWORK — Exact clearspace, minimum size, optical corrections and construction ratios must be authored from the final vector artwork. Until then, never invent “official” geometry from text-only mockups.

## Colour system

Cix uses a material palette: near-black carbon, warm paper, machined neutrals and controlled signal colours. Colour should look engineered, not gamified.

---

| SWATCH | NAME / HEX | ROLE | SEMANTIC |
|---|---|---|---|
|  | Carbon — #0A0D0F | Primary dark background; terminal-adjacent surfaces; high-contrast wordmark. | BASE |
|  | Ferrite — #151A1E | Raised dark surfaces; code panels; navigation. | BASE |
|  | Machined — #242C31 | Dividers, secondary dark surfaces, structural lines. | BASE |
|  | Paper — #F3F0E7 | Primary light background; warm alternative to sterile white. | BASE |
|  | Nickel — #98A2A8 | Secondary text and metadata. | BASE |
|  | Copper — #D87945 | Primary brand accent; focus, selected states, key structural emphasis. | SIGNAL |
|  | Phosphor — #B9E769 | Healthy/live/runtime state; success where state is actually positive. | SIGNAL |
|  | Bus Blue — #5FA8D3 | Informational links, network/data-path emphasis. | SIGNAL |
|  | Amber — #E4B34A | Warnings, physical attention, degraded state. | SIGNAL |
|  | Fault — #E15D57 | Errors, destructive actions, failed state. | SIGNAL |

### Colour rules

• Copper is the primary brand accent, not a universal status colour.

• Phosphor means healthy/live only when the UI is communicating state. Do not use it merely because it “looks terminal”.

• Fault red is reserved for failure and destructive intent. It should never become a decorative brand colour.

• Paper is preferred over pure white for editorial surfaces; pure white may be used for accessibility, print constraints or system-native components.

• In dense product UI, neutrals should dominate. A screen covered in accent colour is off-brand.

## Typography

Typography must work equally well in a technical manual, a web console and a command example. The system pairs a disciplined grotesk with a true monospace.

---

| ROLE | TYPEFACE |
|---|---|
| Primary UI / editorial | Inter. Use Regular/Medium for text, SemiBold/Bold for hierarchy. |
| Display | Inter Display. Use for large headlines and major navigational moments; avoid ultra-tight tracking. |
| Monospace | JetBrains Mono when available; DejaVu Sans Mono is an acceptable open fallback for broad system compatibility. |
| Fallback sans | Noto Sans / system sans stack where Inter is unavailable. |
| Fallback mono | ui-monospace, SFMono-Regular, Consolas, Liberation Mono, monospace. |

### Type scale

| TOKEN | RECOMMENDATION |
|---|---|
| Display 1 | 48–72 px / 0.95–1.02 line-height / Bold |
| Display 2 | 36–48 px / 1.0–1.08 / Bold |
| H1 | 28–36 px / 1.08–1.15 / SemiBold or Bold |
| H2 | 20–24 px / 1.15–1.25 / SemiBold |
| Body | 15–17 px / 1.45–1.65 / Regular |
| UI | 13–15 px / 1.35–1.5 / Regular or Medium |
| Meta | 11–12 px / 1.3–1.45 / Medium |
| Code | 13–15 px desktop; never below readable system minimums |

### Typographic behaviour

• Sentence case is the default.

• All-caps is reserved for compact metadata, table labels, status tags and document kickers.

• Do not use faux small caps.

• Avoid center alignment except for singular ceremonial moments such as a boot mark or event title.

• Numbers in monitoring tables should use tabular figures where supported.

• Code, identifiers and paths are monospaced; ordinary technical nouns are not automatically monospaced.

## Grid, spacing and composition

Cix composition should look assembled from a coherent system. Alignment is a brand asset.

---

### Base spacing unit

> 8 PX BASE — Use an 8 px base unit for product and web spacing, with 4 px half-steps for fine alignment. Do not use arbitrary spacing values simply to “look right”.

| PX | USE |
|---|---|
| 4 | Fine internal gap, icon/text optical correction |
| 8 | Base inline gap |
| 16 | Standard component padding |
| 24 | Related content separation |
| 32 | Section separation |
| 48 | Large content group |
| 64 | Major section boundary |
| 96+ | Editorial/marketing whitespace |

### Grid rules

• Product: use a 12-column responsive grid where page-level composition needs columns, but allow resource tables and trees to follow content constraints.

• Documentation: text measure should usually remain 60–80 characters for prose; code can extend in a separate wide lane.

• Marketing: use asymmetry through spans and empty columns, not random offsets.

• Align diagrams, captions, code and body copy to shared edges.

• Borders should clarify containment or hierarchy; do not box every block.

### Signature composition

A recurring Cix layout may use one strong vertical system axis with horizontal branches or intersections. This can echo the “x” metaphor while remaining functional. The motif must align actual content; it should never be a decorative line placed behind unrelated cards.

## Iconography and symbols

Icons are interface language. They should describe resources and actions, not decorate navigation.

---

| PROPERTY | RULE |
|---|---|
| Stroke | 1.5–2 px at 24 px nominal size; optical adjustments permitted. |
| Geometry | Mostly orthogonal with restrained radius. Avoid bubbly rounded icons. |
| Perspective | Flat. No pseudo-3D isometric infrastructure icons in core UI. |
| Fill | Outline by default; filled variants for selected/active states only where necessary. |
| Metaphor | Prefer technical object or relationship over generic cloud symbolism. |
| Consistency | One icon family across product; vendor/device marks remain separate. |

### Resource icon principles

• Container: do not default to a shipping box. Explore namespace/process boundary metaphors.

• Network: use node/path/port logic rather than Wi-Fi symbols for system networking.

• Hardware: distinguish physical device from logical attachment.

• Storage: distinguish block, filesystem and mount where the difference matters.

• DNS: use record/resolve semantics rather than globe icons.

• PKI: distinguish certificate, key and trust state rather than using one lock for everything.

### Forbidden icon patterns

• Emoji as product icons.

• Mixed icon libraries.

• Decorative lightning bolts for performance.

• Lock icons for every security concept.

• Cloud icons used as generic “infrastructure”.

## Diagrams, schematics and data visualisation

A Cix diagram should explain an implementation relationship. If it only makes a page look technical, remove it.

---

### Diagram grammar

| ELEMENT | MEANING |
|---|---|
| Node | A named resource or system component. |
| Solid line | Active or direct relationship. |
| Dashed line | Optional, inferred, future or control relationship; define in legend. |
| Arrow | Directional data/control flow only; never use arrows as decoration. |
| Intersection | Use when flows genuinely cross or converge. The x motif is strongest when semantically true. |
| Colour | Use semantic accents to distinguish resource classes or state, never random series colouring. |

ILLUSTRATIVE ARCHITECTURE GRAMMAR

> source ─────┐ — ├── build ──── system — packages ───┘                 │ — ├── container — hardware ────────────────────┼── network — └── storage

### Data visualisation

• Start with labels and scales, not colour.

• Use direct annotation where it reduces legend lookup.

• For monitoring, optimise for change detection and outliers rather than visual drama.

• Do not smooth lines unless the underlying metric meaningfully warrants smoothing.

• Always expose units and sample windows.

• Never use 3D charts.

## Imagery and illustration

Cix should rarely need generic photography. When images appear, they should reveal systems, materials or real operation.

---

| AREA | GUIDANCE |
|---|---|
| Preferred photography | Real hardware, racks, boards, connectors, manufacturing detail, workbench environments, source/code in genuine context. |
| Preferred illustration | Technical cutaways, resource maps, construction diagrams, exploded system views, typographic compositions. |
| Avoid | Handshake teams, cloud stock imagery, anonymous datacentres with blue light, hooded hackers, abstract AI meshes, glowing code rain. |
| Treatment | Natural or restrained contrast. Avoid cinematic teal/orange grading and excessive shallow depth-of-field. |
| Captions | Use captions to make technical relevance explicit when the image is not self-explanatory. |

## Motion, boot and interaction language

Movement should communicate state transition, not make the system feel alive for its own sake.

---

### Motion principles

• Use motion to explain hierarchy, causality, progress or continuity.

• Prefer 120–220 ms UI transitions for standard interactions; longer only for meaningful system processes.

• No elastic bounce.

• No ambient pulsing except where pulse itself represents live activity, and then keep it subtle.

• Respect reduced-motion settings.

• Progress should be deterministic when measurable; do not use indefinite spinners for known work.

### Boot expression

> BOOT SHOULD FEEL LIKE CIX — A minimal wordmark or glyph may appear briefly, followed by real boot state. Avoid cinematic boot animations that delay useful information. The system earns confidence by showing what it is doing.

ILLUSTRATIVE BOOT TONE

> cix 1.0 — source      verified — build       current — network     ready — resources   14 — login:

## CLI and shell experience

The CLI is one of the primary brand surfaces. It should be more rigorously designed than the marketing site.

---

### CLI laws

01  Commands follow the resource grammar.

02  Human-readable output is concise; machine-readable output is stable and explicit.

03  Errors identify the object and failed operation.

04  Destructive operations use unambiguous verbs.

05  Exit codes are meaningful and documented.

06  Colour is optional enhancement; never required to interpret state.

07  Every output format that may be scripted must have a stable contract.

08  Help examples use realistic resources and avoid toy “foo/bar” when a domain-specific example would teach more.

### Output hierarchy

EXAMPLE HUMAN OUTPUT

> $ cix container inspect web — NAME        web — STATE       running — PID         2418 — NETWORK     edge0 — ADDRESS     10.0.8.12 — STORAGE     root@overlay — DEVICES     gpu0 — SOURCE      /var/lib/cix/source/web — BUILD       8d31a4f

### Error anatomy

PREFERRED

> error: cannot attach device gpu0 to container web — reason: device is already attached to render-01 — next: cix hardware inspect gpu0

### Never

OFF-BRAND ERROR

> Oops! Something went wrong 😕 — Try again later.

## Product UI system

The web console should feel like a direct view onto one system, not a collection of product dashboards.

---

### Information architecture

RECOMMENDED LEFT NAVIGATION MODEL

> CIX — SYSTEM — ├─ Containers — ├─ Networks — ├─ Storage — ├─ Hardware — ├─ DNS — └─ PKI — BUILD — ├─ Source — └─ Packages — OPERATIONS — ├─ Events — └─ Logs

### Core screen anatomy

| REGION | PURPOSE |
|---|---|
| Left tree | Persistent resource taxonomy and object relationships. |
| Main canvas | Authoritative details, list/table, topology or editor for selected resource. |
| Bottom/log pane | Optional operational stream; resizable and dismissible, not permanently stealing space. |
| Command affordance | Show equivalent CLI action where it helps users learn and reproduce operations. |
| Inspector | Use a consistent right-side detail/relationship pattern for quick inspection; do not hide critical state behind drawers by default. |

### Dark and light modes

Both are first-class. Dark mode uses Carbon/Ferrite as structural surfaces; light mode uses Paper/white with Carbon text. Semantic colours must retain meaning in both. Never design dark mode first and generate light mode by simple inversion.

### Tables and trees

• Trees show hierarchy only where hierarchy is real. Use relationship links for graph-like associations.

• Tables are default for comparable resources; cards are not.

• Status appears as text + optional colour, never colour alone.

• Identifiers can be copied without entering edit mode.

• Bulk actions require clear scope and count.

### UI copy

• Button labels are verbs: Create container, Attach device, Rebuild package.

• Use “Delete” when deleting. Do not soften it to “Remove” unless the technical action is actually detach/remove.

• Confirmation dialogs name the object and consequence.

• Avoid modal dialogs for ordinary inspection.

## Documentation and developer experience

Documentation is not a support layer around Cix. It is part of the product’s inspectability promise.

---

### Documentation hierarchy

| DOC TYPE | PURPOSE |
|---|---|
| Concepts | Why the system is structured this way; resource model; architecture. |
| Guides | Goal-oriented tasks with prerequisites and expected state. |
| Reference | Exact commands, API, configuration, file formats and exit codes. |
| Internals | Implementation mechanisms, kernel interfaces, process model and source layout. |
| Troubleshooting | Symptoms → observations → causes → repair steps, with commands to inspect each layer. |
| Design decisions | ADRs or equivalent records for important architectural tradeoffs. |

### Docs writing rules

• Every command example should be copyable.

• Show expected output when state matters.

• State prerequisites before destructive steps.

• Link from high-level concepts to actual source modules when useful.

• Avoid “simply”, “just”, “obviously” and “easy”.

• Version reference docs.

• Use diagrams only when they improve a reader’s mental model.

### Code blocks

DOCUMENTATION PATTERN

> # inspect before changing state — cix network inspect edge0 — # make the change — cix network attach edge0 web — # verify — cix container inspect web

## Website and launch expression

The website should prove the product before it sells the philosophy. Cix does not need a cloud of claims.

---

### Homepage sequence

01  Name + category: “Cix — a source-native systems distribution.”

02  Primary proposition: “Systems, directly.”

03  A real terminal interaction or resource view.

04  Architecture proof: source → build → resource → runtime.

05  Core resource capabilities.

06  Why C / why direct Linux primitives.

07  Install/build path.

08  Documentation/source links.

09  Community/project status.

### Hero copy example

> RECOMMENDED — Cix — Systems, directly. — A source-native systems distribution with a C control plane, direct Linux primitives and one resource model for workloads, networks and hardware.

### CTA vocabulary

| CTA | USE |
|---|---|
| Build Cix | Primary install/source CTA when source build is the intended path. |
| Read the architecture | For technical evaluation. |
| View source | Repository CTA. |
| Read the docs | Documentation. |
| Explore resources | Product capability overview. |
| Get started | Acceptable secondary CTA; less distinctive than the verbs above. |

## Repository, package and release identity

Release mechanics should reinforce the source-native proposition and make provenance obvious.

---

### Repository conventions

• Repository names are lower-case and descriptive.

• Use cix- prefix when the repository is independently consumed or could be ambiguous outside the organisation.

• README first screen: what it is, current status, build requirements, exact first build command.

• Badges must communicate real state. Avoid badge walls.

• Architecture diagrams should be versioned with source, not stored only in design tools.

### Version language

If Cix remains rolling-release, the brand should not pretend conventional point releases are the primary product cadence. Use snapshots, channels, build identifiers and dated release notes as appropriate to the actual update model. Versioning must communicate compatibility rather than marketing ceremony.

| TERM | RULE |
|---|---|
| stable | A channel/state with explicit support expectations; never a synonym for “probably okay”. |
| snapshot | A dated or identified system state suitable for reproducibility. |
| build | A specific compiled artifact/provenance unit. |
| rolling | A delivery model; not a claim that every component is always newest. |
| LTS | Do not use unless a real long-term support policy exists. |

### Release note voice

PREFERRED RELEASE NOTE STRUCTURE

> changed — - network resources now expose route ownership — - package builds record compiler version — fixed — - device detach no longer leaves stale resource links — breaking — - `cix net` alias removed; use `cix network`

## Physical, hardware and environmental applications

Cix should look credible where software meets physical infrastructure: labels, appliance faces, rack diagrams and printed service material.

---

### Hardware marking

• Use one-colour glyph or wordmark.

• Laser etch, screen print and emboss/deboss should be supported; no identity effect may depend on colour.

• Keep product model, serial and regulatory information visually separate from the brand mark.

• Do not overbrand every port or panel.

• Port labels prioritise function over marketing names.

### Event and merchandise

• Prefer diagrams, build strings, resource trees and restrained wordmark use over giant motivational slogans.

• Avoid generic hacker merchandise tropes.

• A single copper accent on neutral garments/materials is stronger than multicolour logo treatments.

• Technical copy can be used if it remains valid; do not print fake terminal output.

## Accessibility and inclusive design

Directness includes making system state perceivable and operable. Accessibility is not a visual exception to the brand; it is a test of whether the brand is actually clear.

---

### Core requirements

• Meet WCAG 2.2 AA for public web and web-console experiences as a baseline target.

• Do not encode status with colour alone; pair colour with text, iconography or pattern.

• All keyboard-operable product actions must show a visible focus state.

• Support reduced motion.

• Maintain readable code sizes and avoid ultra-light font weights.

• Error language should be understandable without relying on icon interpretation.

• Documentation diagrams need text alternatives or adjacent explanations of equivalent information.

### Contrast rule

> DESIGN TOKEN GATE — Every production colour pairing must be contrast-tested in the actual type size/weight and interaction state. Hex values in this document are palette definitions, not blanket approval for every foreground/background pair.

## Governance and quality control

A systems brand decays when each team invents local vocabulary and interface patterns. Governance should be lightweight but explicit.

---

### Brand ownership model

| OWNER | RESPONSIBILITY |
|---|---|
| Brand steward | Owns core principles, naming, visual assets and major external expression. |
| Product design | Owns component/system implementation within the brand rules. |
| Engineering | Owns technical truth: command/API names, failure semantics, architecture accuracy. |
| Documentation | Owns terminology consistency and explanatory quality. |
| Release owner | Ensures release names, notes, channels and provenance language match policy. |
| Contributors | Follow public conventions and propose changes through documented review. |

### Decision register

Maintain a lightweight brand decision record for any change that alters master naming, logo geometry, palette tokens, typography, product taxonomy, primary tagline or command grammar. Record the decision, rationale, affected surfaces, migration path and date.

### Asset source of truth

• One canonical vector source for every logo variant.

• Machine-readable design tokens for colour, spacing and typography.

• Icon source files in a versioned repository.

• A component library that maps UI tokens to implemented code.

• No exporting logos from screenshots or slide decks.

## ThinC → Cix migration system

The rebrand should preserve architectural trust and searchability while clearly establishing Cix as the future name.

---

### Migration principle

> CONTINUITY WITHOUT BAGGAGE — Communicate that Cix is the evolution of the ThinC project, but stop making ThinC the explanatory frame once the migration window closes. Cix must become the subject of the sentence.

### Migration phases

| PHASE | ACTION |
|---|---|
| Phase 0 / Private | Finalize naming clearance, identity assets, package migration map, docs redirects and compatibility strategy. |
| Phase 1 / Announcement | “ThinC is becoming Cix.” Explain the expanded distribution scope and preserve the original technical thesis. |
| Phase 2 / Dual reference | Use “Cix, formerly ThinC” in high-discovery locations only: repository description, docs landing page, release announcement. |
| Phase 3 / Cix primary | Cix appears alone in product UI, CLI, docs navigation and new content. ThinC survives only in migration references and redirects. |
| Phase 4 / Archive | ThinC branding removed from active surfaces. Historical pages retain accurate names for provenance. |

### CLI migration

• Prefer a clean `cix` namespace rather than indefinitely carrying ThinC command aliases.

• If compatibility aliases are necessary, emit a precise deprecation notice with removal criteria/version/date.

• Do not silently redirect configuration paths if ambiguity could cause destructive behaviour.

• Document package, service and filesystem path changes in one canonical migration guide.

## Launch sequence and rollout

Brand rollout must be synchronized with technical namespaces. A partial rename that leaves binaries, docs and UI disagreeing will undermine the entire premise of consistency.

---

01  Complete naming/trademark/domain/package clearance.

02  Choose and refine the final wordmark + system glyph against the criteria in §13.

03  Produce master vector, monochrome, small-size and favicon assets.

04  Freeze master design tokens and validate colour contrast.

05  Implement CLI/package/service namespace migration.

06  Implement product navigation and terminology migration.

07  Update documentation taxonomy and redirects.

08  Prepare migration guide and announcement.

09  Update repositories, issue templates, contribution docs and release automation.

10  Update website, social/profile surfaces and downloadable assets.

11  Run a cross-surface brand QA audit.

12  Launch only when the system identifies itself consistently from boot to docs to repository.

### Definition of launch-ready

> THE BOOT-TO-BROWSER TEST — A user should be able to boot Cix, run `cix --help`, open the console, inspect the API docs and visit the website without encountering conflicting product names, resource nouns, colours or positioning statements.

## Copy bank and application examples

These examples are canonical starting points. Adapt detail to the actual product state; never ship a claim before the implementation supports it.

---

### One-liners

• Cix is a source-native systems distribution where C is the control plane.

• Cix runs workloads through direct Linux primitives and one consistent resource model.

• Build the system. Run the system. Understand the system.

• Systems, directly.

• Built from source.

• C, all the way down.

• Less between you and the machine.

### About paragraph

Cix is a source-native systems distribution for operators who want a shorter path between intent and mechanism. It uses direct Linux primitives for isolation and resource control, a native C control plane, and a consistent model for containers, networks, hardware and system services. The result is infrastructure designed to be inspected, understood and rebuilt—not merely consumed.

### Repository description

Source-native systems distribution with a C control plane and first-class resources for workloads, networks and hardware.

### CLI help intro

ILLUSTRATIVE

> cix — control the Cix system — usage: — cix <resource> <command> [flags] — resources: — container   workload isolation and lifecycle — network     links, addresses and routes — hardware    devices and attachments — storage     mounts and backing storage — dns         names and records — pki         keys, certificates and trust — package     source builds and packages

### Error examples

| QUALITY | EXAMPLE |
|---|---|
| Good | cannot start container web: network edge0 does not exist |
| Better | cannot start container web — missing resource: network edge0 — inspect available networks with `cix network list` |
| Bad | Container startup failed. |
| Never | Whoops! We hit a snag. Please try again. |

### Announcement opening

ThinC is becoming Cix. The original idea remains: keep the host thin, keep the control plane close to the system, and make infrastructure understandable. The new name reflects what the project has grown into—a full source-native distribution whose scope now extends across workloads, networks, hardware, packages and core services.

## Brand QA checklists and decision tests

Use these before shipping any significant Cix surface. They are intentionally stricter than “does it look on-brand?”

---

### Messaging QA

• Can the first paragraph tell a technical reader what Cix actually is?

• Does every major claim have a mechanism or proof behind it?

• Have vague adjectives been removed or defined?

• Is Cix the subject, rather than ThinC or a competitor?

• Does the text distinguish source-native from merely open-source?

### Visual QA

• Does the layout work in monochrome?

• Is accent colour carrying information or intentional emphasis?

• Are relationships aligned to a grid?

• Have gradients, glow, stock cloud imagery and cyberpunk clichés been avoided?

• Does the mark remain legible at the actual target size?

• Are status colours used semantically and accessibly?

### Product QA

• Does the resource noun match CLI, API and docs?

• Can the user inspect state before changing it?

• Does an error expose cause and next action?

• Can scripted output be consumed without parsing decorative text?

• Does the UI show real relationships instead of forcing everything into cards?

• Can essential state be understood without colour?

### The five-question Cix test

FINAL DECISION TEST

> 1. Is it direct? — 2. Is it technically true? — 3. Is the mechanism inspectable? — 4. Does it fit the resource grammar? — 5. Did we add anything that does not earn its layer?

## Appendix A — Terminology

Canonical terms should be maintained as part of the product contract.

---

| TERM | DEFINITION |
|---|---|
| Cix | Master brand and distribution name. |
| resource | Generic managed system object with identity, state and lifecycle. |
| host | The Cix system providing resources and runtime. |
| container | A workload isolation unit implemented through Linux mechanisms. |
| network | A managed networking resource or relationship. |
| hardware | Physical/logical device resources exposed by the host. |
| device | Specific hardware resource instance when technically appropriate. |
| source | Source tree/code used to construct an artifact or system component. |
| build | The process/result of compiling source into an artifact. |
| package | A distributable/buildable software unit in the Cix package system. |
| state | Current authoritative condition of a resource. |
| inspect | Retrieve detailed state, configuration and relationships. |
| attach/detach | Create/remove a relationship without implying object destruction. |
| delete | Destroy the target object/resource. |
| remove | Use only where the underlying semantic is removal rather than deletion/detach. |
| control plane | Cix components that accept intent, manage resources and expose control interfaces. |
| data plane | Components/path where actual workload/network data moves. |
| snapshot | Identified system state used for reproducibility or distribution. |

## Appendix B — Asset inventory

A complete public identity is a set of production assets, not a single logo file.

---

| ASSET FAMILY | REQUIRED OUTPUTS |
|---|---|
| Logo | Primary wordmark SVG/PDF; monochrome dark/light; system glyph; favicon; small-size variants. |
| Tokens | Colour, typography, spacing, radius, border and motion tokens in machine-readable form. |
| Icons | Versioned SVG icon set with naming and contribution rules. |
| Templates | README, documentation page, release note, architecture diagram, social card, presentation title/footer. |
| Product | Navigation icons, status tokens, empty/error/loading states, CLI output conventions. |
| Web | CSS variables, responsive grid, typographic scale, component reference. |
| Print | CMYK/Pantone equivalents if physical print becomes material; verified proofs before declaring values canonical. |
| Governance | Brand decision log, change process, asset source-of-truth location and release checklist. |

## Appendix C — Open decisions and production gates

The strategic system is intentionally decisive. The remaining open items require visual or legal evidence, not more abstract discussion.

---

| OPEN ITEM | GATE TO CLOSE |
|---|---|
| Final wordmark drawing | Run 4–6 identity territories, reduce to 2, test at CLI-adjacent/UI/print/hardware sizes, then optical refine. |
| System glyph | Only approve if it adds recognition at small size; do not create a symbol merely because brands “need” one. |
| Primary tagline | “Systems, directly.” is the recommended master line; validate against launch narrative and naming clearance. |
| Font licensing / distribution | Confirm intended web/app distribution rights for the chosen production fonts and fallbacks. |
| Colour production values | Validate display contrast and print reproduction; assign CMYK/spot values from physical proofs, not automatic conversion. |
| Trademark/domain/package clearance | Professional legal and ecosystem search before public launch. |
| Exact product taxonomy | Reconcile the canonical capability map with the implemented API and daemon architecture before freezing docs/navigation. |
| Migration compatibility window | Set dates/releases for ThinC aliases, paths, package names and redirects based on actual adoption risk. |

### Recommended next identity sprint

01  Generate 6 black-and-white Cix wordmark/system-glyph territories.

02  Test each at 16 px, 24 px, 48 px, terminal header, web console sidebar, Git repository avatar, boot screen and laser-etched hardware label.

03  Reject anything that needs colour/effects to become distinctive.

04  Select 2 directions and build the full wordmark/glyph relationship.

05  Apply the new palette only after form is approved.

06  Lock vectors, then add exact logo construction/clearspace/minimum-size pages to this guideline as v1.1.

> NORTH STAR — The identity is successful when Cix feels inevitable in the places it actually lives: source tree, shell, boot sequence, API, system console, documentation and hardware. The logo should not be the loudest part of that experience. The coherence should be.

## Closing principle

Cix should never ask the brand to do work the architecture cannot support.

---

Build what you run.  
Understand what you build.

Cix is strongest when its visual, verbal and product systems all point to the same idea: fewer unnecessary layers, clearer mechanisms, one coherent resource model, and infrastructure that remains legible to the people responsible for it.

END / CIX BRAND GUIDELINES V1.0
