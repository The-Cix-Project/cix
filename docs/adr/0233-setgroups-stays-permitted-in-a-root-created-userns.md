# 0233 — setgroups stays permitted in a root-created user namespace

## Status

Accepted. Refines the user-namespace setup in
[ADR-0179](0179-user-namespaces-by-default-subordinate-id-allocation.md) / [ADR-0207](0207-btrfs-storage-substrate-userns-by-default.md);
neither decision changes, only how the maps are written.

Found by the owner, whose DNS containers would not start after a
storage migration recreated them:

> *"yep, userns is the issue, let's fix our dns to play nice?"*

## Context

`write_userns_maps()` wrote `deny` to `/proc/<pid>/setgroups`
unconditionally, then `gid_map`, then `uid_map`.

That ordering is the documented recipe for the **unprivileged** case.
`user_namespaces(7)` requires the deny before `gid_map` *only when the
writing process lacks `CAP_SETGID` in the parent user namespace*. cixd
is real root and has it, so the requirement never applied — the recipe
was followed where it was not needed, and the cost was paid inside
every container.

Denying setgroups makes `setgroups(2)` fail with `EPERM` for the
container's whole lifetime. Any workload that sets or drops
supplementary groups — which is most things that run as a named user —
cannot start.

**dnsmasq is one of them, and it is this platform's own DNS.** Told to
run as a user (`-u root`), it calls `setgroups()` before `setgid()` and
dies if either fails:

```
dnsmasq[1]: failed to change group-id to root: Operation not permitted
dnsmasq[1]: FAILED to start up
```

exit 5, and **nothing on stderr** by default, because dnsmasq logs to
syslog unless told otherwise. The container simply exited 5 in a restart
loop with no diagnostic anywhere.

It stayed hidden because user namespaces were opt-in when this was
written. ADR-0207 made them the default for newly created containers,
which silently converted "an unused option has a sharp edge" into "a
container recreated for any reason will not start". It surfaced when a
`container-storage` migration recreated `dns-1` and `dns-2` and neither
came back — the migration was blamed first, and was not the cause.

## Decision

**Write `gid_map` first. Only if the kernel refuses it, deny setgroups
and retry.**

The fallback is not dead code: it is the correct sequence for any caller
that genuinely lacks `CAP_SETGID`, and keeping it means the privileged
and unprivileged paths are both right rather than one being right and
the other being assumed.

## Consequences

- **Containers can set and drop supplementary groups**, so ordinary
  service software runs under a user namespace instead of failing at
  startup. dnsmasq starts.
- **The protection being given up does not apply here.** Denying
  setgroups exists so an unprivileged user cannot drop supplementary
  groups to escape a negative-permission ACL — a group that denies
  access rather than granting it. This namespace is created by root
  over a dedicated, non-overlapping id range that owns nothing on the
  host, so there is no such ACL to escape.
- **The failure mode this fixes is worth remembering more than the
  fix.** A default flipped on (ADR-0207), an unrelated operation
  (storage migration) recreated two containers, and the platform's own
  DNS stopped with an exit code and no message. Nothing reported the
  actual cause; it took `capture_output` plus `--log-facility=-` to make
  dnsmasq say anything at all. The `dns-1`/`dns-2` container recipes now
  set `capture_output: true` for that reason.
- **Other container recipes in this repo are likely affected the same
  way** — `ldap-1/2`, `ntp-1/2`, `syslog-1/2`, `jump` all run real
  service software and none of them has been started since userns became
  the default. They should be brought up and checked rather than assumed
  fine.
