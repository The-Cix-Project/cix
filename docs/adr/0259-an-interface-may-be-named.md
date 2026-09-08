# 0259 — An attachment's interface may be named, and defaults to the position it always had

## Status

Accepted.

Requested by the owner while adding BIRD to the `cr-1`/`cr-2` router pair: *"maybe we can name
interfaces if needed if they are unpredictable names?"* — and, asked to choose between writing
`eth0` in the routing config and building the capability: *"make namaable a real feature"*.

## Context

A container's own interface names have always been positional. `src/container_net.c` derives them
from the attachment's index and nothing else:

```c
snprintf(ifname, sizeof(ifname), "eth%d", idx);
```

So the first network a container attaches to is `eth0`, the second `eth1`, and the name carries no
information beyond the order the operator happened to list them in.

**They are deterministic, and that is worth stating plainly, because it is the argument against
doing this.** Measured on both routers before deciding:

| container | management | services |
|---|---|---|
| `cr-1` | `eth0` (192.168.15.101) | `eth1` (192.168.150.253) |
| `cr-2` | `eth0` (192.168.15.102) | `eth1` (192.168.150.252) |

Nothing is currently broken. `eth0` is management on both, and a routing daemon told to use `eth0`
would work today.

What is wrong is that **the fact is not in the configuration.** A BIRD stanza reading
`interface "eth0"` is correct only for as long as nobody reorders the `networks` array in a
container recipe — an edit that looks like reordering a list, produces no error, and silently moves
a routing protocol onto the wrong subnet. `cr-1`'s own `keepalived.conf` already carries three
literal `eth1` references for exactly this reason, and its comment has to explain in prose which
interface faces which way. The position is load-bearing and invisible.

This is the same class as the protected-partition rule keyed on partition number rather than label:
a property that is true right now is not the same as a property something may depend on.

## Decision

**An attachment may carry `ifname`, the name that interface has inside the container. Absent, the
platform assigns `eth<index>` exactly as before.**

The field is accepted wherever an attachment is described — `POST /v1/containers`' `networks[]`
entries and `POST /v1/containers/{name}/networks` — through the one shared
`NetworkAttachmentRequest` schema, which the live-attach endpoint previously duplicated inline and
now references.

Validation, all of it at parse time so the refusal names the rule rather than surfacing later as
`failed to create container` from a bare `rtnl_link_rename()` failure:

- 1–15 characters. That is `IFNAMSIZ - 1`, the kernel's own ceiling, not a limit invented here.
- `[A-Za-z0-9._-]` only — deliberately narrower than what the kernel accepts. A name containing a
  space, a slash or a colon is legal to the kernel and then reads as two fields in every tool that
  prints one interface per line.
- `lo`, `.` and `..` are refused.
- **`eth<digits>` is refused outright.** That is the namespace unnamed attachments are assigned
  from. Permitting an explicit `eth1` would mean reasoning about whether it collides with the
  default the next attachment is about to receive — a matrix, where refusing the prefix is a rule
  that fits in one sentence. Bare `eth` is not in that namespace and is allowed.
- Two attachments on one container may not share a name: `400` at create, `409` on a live attach.
  Only explicit names can collide, because an explicit `eth<N>` is already refused.

**Persistence needed no new mechanism, which is worth recording so nobody adds one.**
`containerdef.c` persists the original creation body and replays it, so a name given at create time
survives a restart the same way every other field does.

## Alternatives considered

**Derive the interface name from the network's name.** Tempting — the networks here are already
called `management` and `services`, so the operator's configuration would have worked with no new
field at all, and it needs no second source of truth.

Rejected on three counts. Network names may exceed 15 characters, so the derivation needs a
truncation rule and truncation can collide. A container attached twice to related networks would
have its interface names dictated rather than chosen. And decisively: it would rename the
interfaces of **every existing container**, silently breaking every literal `eth0`/`eth1` already
written in a config — starting with `cr-1`'s own `keepalived.conf`. A default that changes under
existing deployments is not a default.

**Leave it positional and write `eth0` in the routing config.** This is what the platform does
today and it works. Rejected because it keeps a real dependency — "management is the first
attachment" — expressed nowhere except the order of a JSON array, enforced by nothing.

## Consequences

An interface name becomes operator intent rather than derived state, so it can be relied on. A
routing daemon, a firewall rule or a VRRP instance can name the interface it means, and reordering
a recipe's `networks` array stops being able to silently redirect it.

`registry_network_attachment.ifname` is now the answer to "what is this interface actually called"
for every attachment. Its comment claimed the field was populated only for live attachments, which
had already stopped being true when create-time attachments began recording their positional name
there; it is corrected in the same change rather than left to be discovered again.

The cost is one more field an operator can get wrong, which is why every refusal names the rule it
enforces. And unnamed attachments are entirely unaffected: no existing container changes, because
an absent `ifname` produces the identical `eth<index>` it always did.
