# 0202 — The ESP is reachable over REST

## Status

Accepted

## Context

Issue #128. A real installed host could not complete an A/B update, and nothing anywhere said why.

The update itself worked. `POST /v1/system/update` wrote the new control-plane image to the inactive slot and staged a correct loader entry beside the existing ones. The host rebooted — into exactly what it was already running. Repeatedly.

The cause was one line in `loader.conf`:

```
default thinc-*
```

**systemd-boot's `default` is a glob PATTERN, not an entry name.** That pattern was written at install time, under the project's previous name. It goes on matching every stale entry from that era, and it outranks every newly written one. Observed on the real box:

| entry | matched by `thinc-*` |
|---|---|
| `thinc-a.conf`, `thinc-b.conf`, `thinc-a+3.conf`, `thinc-b+3.conf`, `thinc-a+2-1.conf`, `thinc-b+2-1.conf` | yes |
| `cix-b+3.conf` — the correctly staged update | **no** |

Six stale entries shadowing the one that mattered. No error is possible here: from the bootloader's point of view nothing is wrong, and from the daemon's point of view the update succeeded.

What made this expensive was not the bug, it was the **invisibility**. Every other piece of a running host's state had an endpoint — containers, networks, DNS, PKI, disks, sysctls, kernel modules, routes, the log store. The ESP had none. `loader.conf` could not be read, let alone changed, from anything short of a physical console. On a project whose charter is that there is no SSH and no general shell on an installed host, that made one file permanently unreachable, and one stale line in it permanently fatal to updates.

A partial exception already existed and is worth noting precisely, because it shows the gap was about *scope*, not capability: `GET /v1/system/boot-console` already lists loader entries and their `options` lines, and `PUT` already rewrites those lines on the ESP. So the daemon could already read and write the ESP. What it could not do was touch `loader.conf`, enumerate entries as first-class objects, or remove one.

## Decision

**The ESP's boot configuration is an ordinary REST resource, like every other piece of host state.**

A new module (`daemon/src/esp.c`) owns it, rather than more ESP logic accreting in `main.c`:

```
GET    /v1/system/esp                  loader.conf + every entry + what will actually boot
PUT    /v1/system/esp                  {"default": "...", "timeout": N} -- partial
DELETE /v1/system/esp/entries/{name}   remove one stale entry
```

with `cixctl esp show | set | rm-entry`.

### Reporting comes first

The endpoint's most important field is not settable. `selected_entry` names the entry systemd-boot would actually boot, and every entry carries `matches_default`. Deriving that by hand from a glob pattern and a directory listing is exactly the step that went wrong for a long time, so the daemon does it and says so. `cixctl esp show` leads with `will boot: <entry>`.

This is the part that turns #128 from a multi-hour investigation into a single command.

### Pattern matching and selection follow the bootloader, not the filesystem

An API that describes the firmware must agree with the firmware, so both halves mirror systemd-boot's own rules rather than a plausible-looking approximation:

- **Entry id.** The id is the filename with `.conf` removed *and* the Automatic Boot Assessment counter stripped — `cix-b+3.conf` has the id `cix-b`. `esp_pattern_matches()` therefore tests three spellings (filename, `.conf`-stripped, and the real id), so `default cix-b` selects `cix-b+3.conf` exactly as the bootloader would.
- **Selection order.** Entries whose counter has reached zero sort last; the rest sort ascending by id; the pattern takes the **first** match.

That second rule was got wrong first time round, and the way it was caught is worth recording. The initial implementation took the *last* match in sort order. Against the real host's entry set that named `thinc-b.conf` — a **slot-B** entry — while the machine demonstrably boots **slot A**. The error was invisible in isolation and only surfaced by checking the computed answer against what the box actually does. A field like this being confidently wrong is worse than absent, because its whole purpose is to be believed; the regression test now asserts the slot, not just the prefix.

### Two guards, because the operator is not at the console

By construction, anyone using this endpoint cannot physically reach the machine. Both refusals are therefore hard errors, not warnings:

- **A `default` matching zero existing entries is refused** (409). It is silent, and it strands the host on next boot. The error names what does exist, turning a bricking mistake into an obvious typo.
- **The last remaining entry for the running slot cannot be deleted** (409). Removing a *duplicate* of it is allowed — clearing accumulated duplicates is precisely what this is for — but the last one standing is protected.

Path traversal in the entry name is rejected outright.

The second guard also **degrades rather than disappears**. It works by attributing entries to the running slot via the `--slot=` this daemon writes into every entry it creates — so a daemon started without `--slot` (not the installed control plane) can attribute nothing, and the guard would silently protect nothing at all. Instead it falls back to the weaker claim that still always holds: never remove the last entry on the ESP. A guard that quietly stops applying under a different startup is worse than one that is merely coarse.

### Rewrites preserve what they do not understand

`loader.conf` legitimately carries `console-mode`, `editor`, `auto-entries` and others this module has no opinion about. The rewrite replaces only the directives it was asked to change and copies every other line verbatim, rather than regenerating the file from a template and silently discarding settings.

### The loader directory is derived, not composed

`main.c` already owns the single answer to "where is the ESP" (`g_esp_entries_dir`, including its `--test-esp-entries-dir=` override). The loader directory is derived as that directory's parent rather than composed independently from `ESP_DIR` — otherwise a test could point entries somewhere harmless while `loader.conf` writes still landed on the real `/boot`.

## Consequences

- A host in #128's state can be diagnosed with one GET and repaired with one PUT.
- The class of failure — a stale `default` pattern silently outranking new entries — is now visible by default rather than only findable by someone who already suspects it.
- **This does not rescue the host that prompted it.** That machine runs a build predating this endpoint, and the endpoint only exists in builds it cannot boot — the catch-22 is the issue itself. It needs out-of-band access (its hypervisor's console or disk) or a reinstall. Every host installed from a build carrying this API is fixable remotely; that one is not.
- The ESP is now writable through the API by anyone who can write to the daemon. That is the same trust boundary every other host-mutating endpoint already has (host-auth write-gating), and the two guards above bound the damage a mistake can do.

## Alternatives considered

**Make the update path rewrite `loader.conf` itself.** Rejected. It would fix the symptom on the next update while leaving the state just as invisible, and a daemon silently rewriting the bootloader's own configuration as a side effect of an unrelated operation is exactly the kind of implicit behaviour that made this hard to find. An operator changing the default should be an explicit, logged, refusable action.

**Name new loader entries so a legacy pattern keeps matching them.** Rejected outright: a compatibility shim in the boot path, permanently encoding a dead project name into every future entry, to paper over one stale line that is now editable.

**A general "read/write a file on the ESP" endpoint.** Rejected. It offers no diagnostic — the whole value here is the daemon computing `selected_entry` and `matches_default` — and an arbitrary-write endpoint into the boot partition has a far worse failure surface than three typed operations with guards.
