# 0013 — `/proc/<pid>/root/` for writing into a running container's filesystem, not raw upperdir or setns()

## Status

Accepted

## Context

Phase 8's DNS server bindings need the daemon to write a hosts file into a *running* container (the dnsmasq workload) and have it visible immediately, so a `SIGHUP` makes dnsmasq reload the current record set. The first design, agreed with the user before any code was written, was to write directly into the container's own upperdir path on the host (`/var/lib/kanxeo/containers/<name>/upper/...`) — the daemon already owns and creates that directory tree, so it seemed like the obvious, already-available channel.

That assumption turned out to be wrong, and was caught empirically before the feature was built on top of it (this project's established discipline after ADR-0008/ADR-0009: verify subtle kernel/filesystem behavior directly, don't reason your way to an assumption and ship it). A live test — create a container, `mkdir`+write a new file straight into its upperdir from the host while it kept running, then check via `/proc/<pid>/root/` whether the running container's own view showed it — came back negative even after correctly pre-creating the parent directory. The Linux kernel documents this as expected: modifying the upper layer directly while the overlay is mounted is unsupported/undefined, since the already-mounted instance's directory cache isn't notified of out-of-band changes to the underlying layer.

The next candidate was `setns(CLONE_NEWNS)`: fork a short-lived helper, join the target container's mount namespace, write the file through its own view of the filesystem, exit. This would work, and would double as a reusable "enter a running container's namespace" primitive (Phase 7 part 3 had already flagged exactly this need for live route updates, just for the network namespace instead of the mount one). The user was asked and approved this before any of it was written.

Before implementing it, a simpler alternative was tried and confirmed to work just as well: `/proc/<pid>/root/` is a magic symlink the kernel already resolves through the *target* process's mount namespace and root, for any caller with sufficient privilege (this daemon, running as real root, always qualifies) — no `setns()`, no forked helper, no new namespace-entry code at all. A live test (`mkdir`+write through `/proc/<pid>/root/etc/...`, immediately re-read the same way, and independently confirmed on disk in the raw upperdir afterward) showed it goes through the container's real overlay correctly. Brought back to the user, who chose this over building the `setns()` primitive.

## Decision

Any daemon code that needs to write into (or read from) a specific running container's filesystem from outside it does so through `/proc/<pid>/root/<path>` (using the pid already tracked in `registry_entry.handle.pid`), with plain `open()`/`mkdir()`/`write()` calls — never the container's raw upperdir path directly, and no `setns()`-based namespace entry for this purpose. `dns_server_register()`/`dns_server_sync_all()` (`daemon/src/dns.c`) are the first, reference implementation of this pattern; `persist_mkdir_p()` (`daemon/src/persist.c`) handles that the target path's parent directories aren't guaranteed to exist (a minimal container image may have no `/etc` at all).

## Consequences

- This is now the reference pattern for any future "daemon reaches into a specific running container" need — cheaper and simpler than `setns()`, with no forked helper and no namespace-fd lifecycle to manage. A `setns()`-based primitive is still the right tool for anything that needs to *execute code* inside a container's namespace rather than just read/write files through it (e.g. Phase 7 part 3's still-deferred live network-route updates, which need `CLONE_NEWNET`, not `CLONE_NEWMNT`, and aren't a plain file operation) — this ADR doesn't close that door, it just means Phase 8 didn't need to open it.
- Writing directly to a container's raw upperdir path remains something this project never does, for any purpose, while that container's overlay is mounted — it isn't just unreliable for DNS's use case, it's kernel-documented as unsupported in general.
- Verified empirically before either implementation attempt, not assumed from first principles — consistent with, and a direct continuation of, the lesson ADR-0008 and ADR-0009 already established for this project's other raw-syscall-adjacent surprises.
