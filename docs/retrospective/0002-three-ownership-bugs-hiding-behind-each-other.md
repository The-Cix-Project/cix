# Three ownership bugs hiding behind each other

A retrospective on one session that started with a container that would not
start, and ended four deploys later having fixed three distinct defects — each
of which was **invisible until the one in front of it was fixed**.

This document exists because that structure is the finding, not any of the
three bugs.

## The chain

| # | Bug | Symptom | Why it was invisible |
|---|---|---|---|
| [#264](https://git.home.arpa/itdlabs/cix/issues/264) | `/run`'s tmpfs mounted by an unmapped uid | `jump` crash-looped on `mkdir /run/sshd-empty: Permission denied` | Nothing running wrote to `/run`. `dnsmasq` is launched with `--pid-file=` and glauth writes nothing there |
| [#265](https://git.home.arpa/itdlabs/cix/issues/265) | Staged files written as on-disk uid 0 into a rootfs chowned to the subordinate base | `nslcd: cannot open config file (/etc/nslcd.conf): Permission denied` | Only reachable once `/run` worked. And only 0600 files break — world-readable staged files had been working by accident |
| [#266](https://git.home.arpa/itdlabs/cix/issues/266) | `spec.userns_idmap` cleared by a later `memset`; then, behind it, detached fds closed before the id-map used them | Volumes unwritable in every container; then dns-1/dns-2 refusing to start with `EBADF` | The flag was always 0, so the code it gated **had never once executed** |

Every one is the same question — *what the platform hands a container must be
owned in the container's own id space* — and every one has a different
mechanism and a different fix.

## The part worth keeping

**Fixing a bug can be what makes the next one reachable, and the second failure
is then easy to read as a regression from the first fix.** It is not, and
treating it as one sends you to revert the thing that was correct.

The sharpest case: `spec.userns_idmap` was being wiped before the runtime read
it, so ADR-0207 phase 3's id-mapping never ran. Fixing that ran the block for
the first time in the feature's life — and it failed immediately, because the
parent closed the detached rootfs and volume fds *before* the block that hands
those fds to `mount_setattr()`. That second bug had been sitting in a shipped
code path that could not execute. **A gate that is never true does not protect
the code behind it; it hides it.**

`dns-1` and `dns-2` went down as a result — and since the host resolves through
them, the box then could not resolve `git.home.arpa` to fetch its own source.
Recovering meant pointing `/v1/system/resolv` at the LAN's resolver, building,
deploying, and pointing it back.

## What actually found each one

Not reading. In every case the diagnosis came from running something inside a
real container and looking:

```
uid_map  0 4425376 65536
/        drwxr-xr-x 0     0        <- id-mapped rootfs, correct
/run     drwxr-xr-x 65534 65534    <- the bug, named outright
```

Two measurement traps cost real time and are worth stating:

- **`exec` lies about this.** `POST /containers/{name}/exec` reports an
  identity `uid_map` and succeeds at writes the container's own process cannot
  make. Every ownership question has to be asked as the container's **main
  process**, with `capture_output`, or the answer is a false pass.
- **A standalone probe beats reasoning about the kernel.** `chown()` after
  mounting looks like the obvious fix for #264 and returns `EPERM` — a process
  whose own fsuid is unmapped cannot give away a file owned by an unmapped uid.
  A 90-line probe reproducing the exact namespace situation settled in one run
  what an argument would not have settled at all, and also proved the chosen
  fix is a no-op without a user namespace, which removed a branch from the
  code.

## A claim I made and had to withdraw

While fixing #266 I wrote that volumes were broken *because* the `memset`
cleared `userns_idmap`. That was inference stated as measurement. This host's
container storage is ext4 (`GET /v1/disks`: `vda5 fs=ext4`), so it never takes
the btrfs snapshot branch the flag affects — the `memset` bug could not have
caused what I had measured. The real cause was a third thing: volume
id-mapping was gated on a flag describing the **rootfs presentation**, and a
volume is a host-root-owned directory outside the rootfs in either
presentation. One flag doing two jobs.

The correction cost nothing because it was caught before the work shipped. It
is recorded here because the project's own rule — never state anything about
this environment as fact without the command that produced it — is exactly the
rule that catches this class of error, and I broke it while writing about
measurements.

## What changed structurally

- `mount_container_tmpfs()` ([ADR-0239](../adr/0239-a-container-facing-filesystem-is-owned-by-the-containers-root.md))
  — the ownership rule travels with the act of mounting, so the next writable
  filesystem created for a container cannot silently omit it.
- `stage_container_file()` is now the **only** place container files are
  staged. It was one of three: the `files[]` loop and the `dns_servers`
  resolv.conf writer each carried their own copy of the same
  target/`mkdir_p`/open/write/`fchown` sequence. That parallel implementation
  is why #265 had three homes, and collapsing it is as much the fix as the
  ownership rule is.
- `test_userns_run` asserts all three in one place: `/run` writable, a staged
  0600 file readable, an attached volume writable — with distinct exit codes so
  a future failure names which one.

## The honest limit

`test_userns_run` needs a real user-namespaced container to mean anything —
without one the container's root *is* host root and every assertion passes
against broken code. It skips where such a container cannot be created, and
only when the container never started, never when one ran and was refused. That
is the best available, and it is worth knowing that the dev sandbox cannot run
this test at all.
