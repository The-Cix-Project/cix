# thinC OS — Brand Narrative & Guidelines

> Corrected against the real codebase and the real delivered logo — see "Corrections from the first draft" at the end for what changed and why.

**Pronunciation**: "Thin C" / "Thin See Oh-Ess"
**Spelling**:
- Code identifiers: `thinc` (lowercase — matches the binary names `thincd`/`thincctl`, default paths like `/var/lib/thinc`, and every other C identifier in the codebase)
- Docs and running prose: `thinC` (lowercase "thin", capital "C" — never "ThinC", never "THINC")
- Environment variables / C macros: `THINC_*` (uppercase, standard C convention — e.g. `THINC_BUILD_VERSION`)
- ASCII/terminal shorthand: `C<`
- Formal/full name: `thinC OS`

## 1. Narrative

Most operating systems try to be a platform.
thinC OS tries to disappear.

thinC OS is a rolling-release hardware and workload orchestration platform, compiled entirely from source. It does not ship a fat host. The host is the thinnest possible layer underneath the containers, and containers are where all real work happens.

The name is the architecture.

**Thin is the host.**
It is raw Linux namespaces and cgroups.
It is OverlayFS image layering.
It is no runc, no Open vSwitch.
The host is thin because it refuses to become a platform. It exposes the kernel, not an opinion.

**C is the core.**
The container runtime is hand-rolled C.
The networking data plane — routing, virtual switching, DNS — is 100% custom C, talking to the kernel directly over rtnetlink sockets, no eBPF-based dataplane.
The REST control layer is C: containers, hardware devices, disks, networks, DNS, PKI, kernel modules, host auth, packages, backups — every capability is a first-class API resource.
All of it is compiled exclusively with the Tiny C Compiler.

C is not an implementation detail. C is the product.

The capital C is the load-bearing letter. It is open on one side, like a socket, a namespace boundary, or an API endpoint waiting to be called. The lowercase "thin" wraps around it quietly, exactly as the host wraps the C core.

thinC OS is not a metaphor. It is a thin layer around C.

## 2. How the system is represented in the name

| Element | Where it lives in thinC OS | How it appears in the name |
|---|---|---|
| Rolling-release | Continuous source updates, no frozen distribution versions | Thin stays thin; no accumulated legacy layers |
| Compiled entirely from source | No opaque binary blobs; everything built from source | C is the source language |
| Hand-rolled container runtime | Raw Linux namespaces and cgroups, direct syscalls | Thin — no extra runtime, no runc |
| OverlayFS image layering | Images are layered per-name, not extracted or duplicated | Thin — the host never does unnecessary work |
| 100% custom C networking data plane | Packet processing and routing written directly in C over rtnetlink | C is the data plane |
| REST control layer | One unified API across host, containers, hardware, disks, networks, DNS, PKI, kernel modules, packages, backups | C is the control plane; Thin is the minimal host underneath |
| No runc, no Open vSwitch, no eBPF-based dataplane | Replaced by raw C and direct kernel primitives | Thin — fewer layers, fewer abstractions |
| Compiled exclusively with TCC | Tiny C Compiler as the only toolchain | C is the compiler identity |
| Hardware as a first-class API resource | `/v1/devices`, PCI/USB/GPU passthrough exposed like any other object | The capital C is an open socket — a callable boundary |
| Containers do all real work | Workloads live only in containers | Thin host is only underneath them |
| Host is the thinnest possible layer | No fat host userspace, no bundled platform services | Thin, literally |

## 3. Color system

The palette should feel raw, low-level, and deliberate. No pastels, no gradients on UI surfaces (the logo mark itself is the one deliberate exception), no glossy surfaces.

**Status: proposed system for the web dashboard's own theme.** Today, the only colors actually in use are the mark's own gradient (`#4FE0FF` → `#00C8FF` → `#0090E8`) and `#F2F5FA` — the rest of this palette is the target for the dashboard rebrand, not yet reflected anywhere in the running UI.

| Name | Hex | Role |
|---|---|---|
| Raw Iron | `#0A0E14` | Primary background, terminal canvas, dark surfaces |
| Silicon | `#151B23` | Raised panels, cards, code blocks |
| Trace | `#26303B` | Borders, dividers, schematic lines |
| Source White | `#F2F5FA` | Primary text, high-contrast labels |
| Muted Steel | `#8A94A6` | Secondary text, metadata, descriptions |
| C Cyan | `#00C8FF` | Primary accent — the C mark, links, focus, interactive elements |
| Hardware Amber | `#FFB547` | Physical devices, hardware resources, warnings |
| Namespace Green | `#2EE59A` | Running containers, healthy state, successful overlays |
| Fault Red | `#FF4D4D` | Errors, failed operations, dropped packets |

Rules of use:
- C Cyan is for anything compiled, moving, callable, or interactive.
- Hardware Amber is for anything physical or potentially dangerous.
- Namespace Green is for healthy/running state.
- Fault Red is only for failure. Never decorative.
- Do not mix cyan and amber for the same object.
- Flat color on UI surfaces. No gradients, no soft glows, no glassmorphism — the mark's own gradient is the one deliberate exception, reserved for the logo itself.

## 4. Geometry & experience

The visual language is straight, rigid, and low-level.

**Curved.** The C-bracket's structural corner-rounding and the circular "OS" badge are the only curves in the brand system — both are deliberate, approved parts of the logo lockup, not violations of a "no curves" rule. UI elements themselves (panels, cards, buttons) are not curved.

**Rigid.** Panels, containers, cards, and boundaries use sharp corners. Maximum corner radius in UI: 2px. No pills, no fully rounded buttons, no circular avatars unless representing a physical port.

**Straight.** Connection lines, network paths, and resource graphs use straight lines. Elbows are 90° only, no bezier curves for data flow. Layouts follow a strict 4px grid.

**Motion.** Fast and mechanical. Standard transition: 100–150ms ease-out. No bounce, no spring, no parallax. Hardware events blink hard, like a status LED. Data flows move along straight orthogonal paths.

**Density.** High information density is correct. Show raw IDs, API paths, IP addresses, namespace names, syscalls. The interface should feel like a live control plane, not a consumer dashboard.

**Empty states.** Don't hide emptiness behind illustrations — show the underlying call:

```
GET /v1/containers
200 OK
[]
```

**Error states.** Show the raw signal: exit code, syscall, cgroup path, or the failed API resource:

```
mount(2) failed: ENOENT
/v1/pki/certificate: 409 Conflict
```

## 5. Typography

| Role | Typeface | Notes |
|---|---|---|
| Display / headings | Space Grotesk | Tight tracking, technical but legible |
| Body / prose | IBM Plex Sans | Clear, neutral, readable |
| Code / data / paths | JetBrains Mono | All IDs, IPs, ports, API paths, logs, commands — and the OS badge/tagline in the logo itself |

Rules:
- Monospace is not a style. It is the interface.
- Use tabular numbers for ports, IDs, and addresses.
- Code is never italicized.
- Straight quotes only: `"` and `'`.
- No serif type anywhere.

### Wordmark

In the **logo lockup** specifically, "thinC" is never typed as one literal text string — it's composed of three distinct elements: the word "thin" (lowercase, light weight, Space Grotesk), the graphical C+K mark (which stands in for the capital C), and the "OS" badge. In **running prose**, the product name is typed as the word "thinC" (see spelling rule at the top).

## 6. Logo / mark guidance

The mark is a fused, single glyph: a squared C-bracket, open on the right, with a two-armed chevron/arrow nested into its opening — reads as "C<". The bracket and the arrow are drawn so their edges meet exactly with no gap, but the arrow's arms are deliberately narrower than the bracket's own bar thickness, so a thin sliver of negative space still separates them — enough to read as two fused elements, not one undifferentiated blob.

- Base color: the C Cyan gradient (`#4FE0FF` → `#00C8FF` → `#0090E8`, light to deep, top-left to bottom-right).
- The bracket's outer corners (top-left, bottom-left) are the only rounded geometry in the mark itself — everything else is straight edges and sharp points.
- The arrow's two arms converge to a single shared point, nested inside the bracket's own opening.

Clear space: at least the width of the bracket's own bar thickness on all sides.

Do:
- Use the mono (`currentColor`) variant on any surface where the gradient doesn't read well.
- Keep the mark's proportions locked — scale uniformly, never stretch.

Do not:
- Enclose the mark in a rounded rectangle background.
- Make the bracket lowercase-styled or add serifs.
- Add drop shadows or glow.
- Redraw the bracket and arrow as literal, separate letterforms "C" and "K" — the mark is one fused glyph, not two adjacent characters.

If you need a container shape: a square with 0px radius and a 1px border in Trace, mark centered inside in C Cyan.

## 7. Voice & tone

thinC OS speaks like the system it is: direct, technical, and unimpressed by its own complexity.

Use:
- Active verbs: compile, expose, route, attach, mount, call, bind.
- Raw terms: namespace, cgroup, overlay, syscall, packet, API resource.
- Negative statements as clarity: "No runc. No Open vSwitch. No eBPF-based dataplane."

Avoid: "Seamless," "Intuitive," "Magic," "Serverless," "Cloud-native."

Default sentence structure: subject, verb, object. No filler.

Example phrases:

```
The host is a shim.
Containers are the machine.
Hardware is just another API resource.
Compile everything. Containerize the rest.
No runc. No OVS. No eBPF-based dataplane. Only C.
```

## 8. Tagline

Primary: **Thin host. C core.**

Long form: **thinC OS — the host is a shim. The containers are the machine.**

## 9. Brand summary

thinC OS is a rolling-release orchestration platform where the host is a thin C runtime, hardware is a first-class API resource, and containers are the only place real work happens.

The name is the architecture:
- **Thin** = raw Linux primitives, OverlayFS, no unnecessary layers.
- **C** = hand-rolled runtime, custom data plane, REST control layer, TCC.

The C-bracket's corners and the OS badge are the only curves in the mark. Everything else is straight.

---

## Corrections from the first draft

The first draft was generated by a different AI with no access to this codebase or the actual delivered logo. Corrected against the real code and the real SVGs (`thinc-mark.svg` etc.) before adoption:

1. **Spelling rule was wrong and self-contradictory.** The draft's top-line rule capitalized both letters ("ThinC OS... never write Thinc, thinc os, THINC") and blanket-forbade lowercase forms — contradicting the user's own explicit, already-decided rule (`thinc` in code, `thinC` in prose) and even the draft's own §5 wordmark description ("Thin — lowercase, light weight"). Fixed to match the real, decided rule.
2. **`/v1/hardware` doesn't exist.** The real endpoint is `/v1/devices`. Fixed throughout.
3. **"No eBPF" was stated as an absolute, but it's false as written** — this project's own device-passthrough enforcement genuinely loads a real `BPF_PROG_TYPE_CGROUP_DEVICE` eBPF program. The real charter claim is narrower: no eBPF-*based networking dataplane*. Fixed everywhere it was used as a rallying line.
4. **The Logo/Mark section described a mark that was never built** — an invented "amber square representing a hardware resource" inside the C. Rewritten to describe the actual, delivered fused C+K mark.
5. **"Do not turn the C into a circular icon" contradicted the real, approved lockup**, which already includes a deliberate circular "OS" badge. Fixed: the "only the mark's own corners curve" rule applies to the mark itself; the OS badge is an intentional, separate, already-approved exception.
6. **Typography didn't match the delivered SVGs** (draft specified Space Grotesk/IBM Plex Sans/JetBrains Mono; the original hand-drawn SVGs used Roboto). Resolved by adopting the draft's proposed fonts and rebuilding the SVGs to match, rather than leaving two conflicting specs.
7. **Color system was aspirational, not "already built."** Labeled accurately as the proposed system for the web dashboard's own future theme — the mark itself only uses two of the nine colors today.
8. **The REST-surface list undersold real scope** ("host, containers, hardware, DNS, and PKI" — real but incomplete). Broadened to match the actual API surface (kernel modules/sysctl, disks, backups, host-auth, packages, host stats, and more).
