# Why bringing up DNS on a fresh install was painful

A retrospective on one real session: taking a freshly installed host to two
working DNS containers. The work itself is small — install a package, run two
containers, register them. It took hours, produced eight issues (#134–#141),
and needed a developer machine and a hand-built HTTP server to complete.

This document exists because *that gap* is the finding. Not one bug.

## What was actually required

| # | Step | Should have been |
|---|---|---|
| 1 | Push 298 package + 20 image recipes one at a time over `pkg recipe add` | `pkg sync` from git |
| 2 | Serve a 1.4 GB toolchain squashfs from a laptop over plain HTTP, by IP | Shipped or pulled |
| 3 | Point the artifact cache at a literal IP (no name resolution exists) | A hostname |
| 4 | Repair every artifact's executable bits by hand | Never lost |
| 5 | Delete and recreate the image to defeat manifest-hash dedup | Not required |
| 6 | Type a twelve-argument dnsmasq command line | `cixctl dns provision` |
| 7 | Pin container addresses by hand after auto-allocation hit real LAN space | Policy-enforced |
| 8 | Discover the host resolver silently does nothing | Working, or loud |

Eight steps; **seven were workarounds**. Only "install a package" worked as designed.

## Five root causes

### 1. A circular bootstrap (#135, #141)

Recipes come from git, git is addressed by hostname, hostname needs DNS, DNS is
built from a recipe. A fresh box cannot enter this cycle from any point. Neither
of the two escape hatches (`--toolchain=PATH` on the box, `image_path` on the
box) is reachable on a host with no SSH and no shell — and the API's 1 MiB
request cap means no large file can be delivered over the API either.

**A fresh install is therefore not self-sufficient**, and every workaround above
is a symptom of that one fact.

### 2. Diagnostics that are discarded (#132, and the reason for most of the lost time)

Three separate failures cost hours each because the tool that knew the answer was
never heard:

- `unsquashfs` exit 2 — its stderr names the file and reason; `run_subprocess()`
  throws child stderr away, so the log said only "exited with status 2". Four
  rebuild-and-retry cycles over a 1.4 GB image followed, and the trigger was
  *still* never identified.
- `/etc/resolv.conf bind mount: No such file or directory` — printed by
  `perror()` to a console nobody was reading, invisible to `GET /system/logs`.
  It had been there since the first boot.
- `network_attach_interface failed (13)` — an error code with no indication of
  what was sought or what existed.

Each was a one-line answer sitting behind a discarded stream.

### 3. Failures that present as crashes (#131, #133)

`cixd` and `cix-recover` run as PID 1, where returning from `main()` — *even
successfully* — is a kernel panic with a stack trace. So:

- a configured NIC not being found at that instant killed the box;
- `cix-recover` completing its job perfectly ended in
  `Attempted to kill init! exitcode=0x00000000`.

An operator reading a stack trace reasonably concludes the software crashed. Both
were ordinary, recoverable conditions.

### 4. State that cannot round-trip (#139)

`GET /containers/{name}/files` returns bytes and nothing else — no mode, no
ownership. Any tool built on it silently produces non-executable binaries, and
the platform will faithfully checksum, cache, distribute and install them.
Integrity was verified; usability never was. The failure surfaced far away, as
`execve: Permission denied`.

### 5. Identity by manifest, not content (ADR-0155)

An image version is a hash of its package manifest. Reinstalling the same package
at the same version reproduces the same hash, so a freshly built tree is
discarded and the old content silently retained. A correct fix, correctly
installed, had no effect — and nothing said so.

## The pattern

Every one of these is the same shape: **the platform knows something the operator
cannot see.**

- The tool printed the reason → thrown away.
- The mount failed → to a console nobody reads.
- The mode was wrong → unreportable through the API.
- The image didn't change → dedup succeeded silently.
- The address was on someone's LAN → nothing knew the constraint existed.

Not a lack of correctness. A lack of **legibility**.

## What "user-friendly" means here

The measure is not whether an expert can complete these steps. It is whether
someone who has never read this codebase can install this platform and have
working DNS.

Three properties, in order:

1. **Self-sufficiency.** A fresh install must be able to reach a working state
   using only itself, its installer media, and an operator-supplied address.
   Nothing should require a second machine.
2. **Legibility.** Every failure must name what was attempted, what was found,
   and what to do next. A tool's own diagnosis must reach the log store — always.
3. **One action per intent.** "Set up DNS" is one intent. It should be one
   command, with its expert knowledge (the dnsmasq flags, each encoding a real
   past failure) versioned in a recipe rather than remembered by a human.

## Tickets

| # | Cause |
|---|---|
| #134 | DNS forwarders baked into a command line rather than configured |
| #135 | Circular bootstrap: recipes ⇄ git ⇄ DNS |
| #136 | `cixctl dns provision` — one action instead of seven |
| #137 | Network address pools — auto-allocation must not touch real LAN space |
| #138 | Fresh install never bind-mounts `/etc/resolv.conf` |
| #139 | Artifacts lose file modes |
| #140 | OS-disk free space unusable; disk UI inconsistent |
| #141 | No product path to seed a build toolchain |
| #131 | A failed install kernel-panics instead of reporting |
| #132 | `run_subprocess()` discards its child's stderr |
| #133 | Missing uplink NIC panicked instead of degrading (fixed) |

## What was fixed during the session

- `unsquashfs` exit 2 no longer discards a completed extraction (#132's trigger).
- A missing uplink NIC logs precisely and boots degraded instead of panicking
  (#133).
- `cix-recover` parks on a clear banner instead of panicking on success (#131,
  partial).
- The installer's video console works — every EFI install had been silently
  serial-only.

The remaining eight are open, and this document is their shared justification.
