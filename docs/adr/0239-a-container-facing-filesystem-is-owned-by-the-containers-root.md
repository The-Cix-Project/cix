# 0239 — A filesystem mounted for a container is owned by the container's own root

## Status

Accepted

## Context

`jump` — the SSH entry point, and the only interactive way into this platform — crash-looped on `mkdir: cannot create directory '/run/sshd-empty': Permission denied`, while running as uid 0 (#264).

Measured from inside a real container rather than reasoned about:

```
/proc/self/uid_map     0    4425376    65536
ls -ldn /              drwxr-xr-x  0     0        <- correct
ls -ldn /etc           drwxr-xr-x  0     0        <- correct
ls -ldn /run           drwxr-xr-x  65534 65534    <- unmapped
mkdir /run/sshd-empty  Permission denied
```

ADR-0207's id-mapped rootfs works exactly as intended: `/` and `/etc` are owned by `0` from inside. `/run` is not part of it. `mountns_pivot()` mounts a fresh tmpfs there on every container start — deliberately, so a stale pidfile cannot outlive a restart, a failure that once made `chronyd` permanently unstartable. That mount is made by a process that is still **real host root**, and it must be: the same function `pivot_root()`s, and `container_dev_mknod()` shortly after creates real device nodes, which a user namespace forbids outright. `container.c` therefore performs its `setgid(0)/setuid(0)` into the mapped root only after all of that has run, and says so.

Host uid 0 has no entry in the container's `uid_map`. So at mount time the mounting process is **unmapped**, and a filesystem whose root inode takes its owner from the mounter comes up owned by the overflow uid. The container's own root — the uid the payload actually runs as — is then locked out of its own scratch directory.

It stayed hidden for the life of the feature because nothing exercised it. `dnsmasq` is launched with `--pid-file=` explicitly, `glauth` writes nothing to `/run`, and no test wrote there. Meanwhile `CLAUDE.md` instructs recipe authors to use `/run` as the scratch path precisely because these images have no `/tmp` — so the platform's own guidance led into the one directory that did not work.

## Decision

**A filesystem this platform mounts for a container's own use is owned by that container's root, and the ownership is established by the mount itself.**

`src/mountns.c` gains `mount_container_tmpfs()`, which always passes `uid=0,gid=0`. `tmpfs` resolves those options through the **mounting process's user namespace** — which is already the container's; only its fsuid is unmapped, not its namespace — so `uid=0` names the container's root rather than the host's.

**The alternatives were measured and rejected, not argued about.** A standalone probe reproducing the exact situation (child in a fresh `CLONE_NEWUSER|CLONE_NEWNS`, parent writes the maps, child mounts while still unmapped) gave:

| mechanism | owner seen inside | writable by the container's root |
|---|---|---|
| plain mount (what shipped) | 65534 | **denied** |
| `uid=0,gid=0` mount options | 0 | **writable** |
| plain mount then `chown(0,0)` | — | **EPERM** |

- **Moving `setuid(0)` before the mounts** breaks device passthrough outright: a user namespace's root may not create device nodes at all. Ruled out by the code's own stated reason before it was tried.
- **`chown()` after mounting** fails `EPERM`, because a process whose own fsuid is unmapped cannot give away a file owned by an unmapped uid, whatever capability set it holds. This is the mechanism most people would reach for first, which is why it is recorded here as measured rather than assumed.

**The options are unconditional.** Without a user namespace the mounter is in the initial one, where `uid=0` resolves to host root — byte-for-byte what the bare mount already produced, confirmed with the same probe. A branch on `userns_enabled` would be a second path to keep correct in exchange for nothing.

**It is a function rather than a longer options string at the one call site.** That is the fungible part of this decision, and the point of it: nothing about a bare `mount()` call said the ownership was a decision at all, which is exactly how this happened. Any future writable filesystem created for a container goes through one place that cannot quietly omit it.

**Deliberately not extended to `/proc`, `/sys` or `cgroup2`.** Their roots do read as 65534 from the same cause, and they are deliberately left that way: they are kernel-maintained views rather than storage this platform creates for the container, their root directories are `r-xr-xr-x`, nothing writes to them, and neither procfs nor sysfs accepts `uid=`/`gid=` at all. `/dev/pts` is already correct — devpts derives ownership the same way and already resolves to the container's root. Extending the rule there would mean inventing a mechanism for filesystems that do not want one, which is not the same thing as being consistent.

## Verification

`test/test_userns_run.c` creates a container with `"userns": true` — explicitly, never the platform default, so the test cannot silently stop testing anything the day that default changes — and runs `test/run_child.c`, which reports `/run`'s owner and attempts a write, exiting 42 for writable and 43 for denied.

**The test needs a real user-namespaced container to be meaningful at all.** Without one the container's root *is* host root, `/run` comes up owned by 0 either way, and the test would pass just as happily against the broken code. So it skips where such a container cannot be created (the dev sandbox cannot — `test_image_fixture.c` seeds `userns_default=false` for exactly that reason), and it skips **only** when the container never started, never on one that ran and was refused the write. A skip that could swallow the defect would be worse than no test. It is deliberately not in the `DAEMON_SELFTESTS` build gate for the same reason: a build container cannot create one, so gating on it would gate on a skip.

Verified live on 192.168.15.95 after deploying `v2.43.1`: a userns container reports `/run` owned by `0:0`, `mkdir /run/sshd-empty /run/nslcd` succeeds, and `/proc` and `/sys` remain `65534` as designed.

## Consequences

- `CLAUDE.md`'s standing advice to use `/run` as a container's scratch path is true again. It was the correct advice against a platform that could not honour it.
- The rule is now structural: the next writable filesystem mounted for a container inherits it by construction rather than by someone remembering this ADR.
- **A second, distinct defect of the same family was found immediately behind this one and is not fixed here** (#265): files the daemon *stages* into a userns container — every `files[]` entry, and the synthesized `ldap_client` ones — are written as on-disk uid 0 after `chown_tree()` has already given the rootfs to the container's base id, so they arrive owned by 65534 inside. World-readable ones work by accident; `/etc/nslcd.conf` at mode 0600 does not, which is what `jump` fails on now. Same family — ownership versus the user namespace — different mechanism, different fix, tracked separately rather than folded in.
