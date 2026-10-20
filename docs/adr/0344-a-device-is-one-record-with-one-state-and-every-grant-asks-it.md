# 0344 — A device is one record with one state, and every grant asks it

## Status

Proposed, 2026-10-20, after the owner asked what it would take for the Devices page to show what has been assigned, what is in use and what is free (*"I want to understand and resolve this 100% accurately"*). Issue [#620](https://git.home.arpa/itdlabs/cix/issues/620) is the safety half of it and closes with this. The decision changes device identity, which container definitions and named mappings persist, so it is hard to reverse and accepting it is the owner's decision.

It amends [ADR-0022](0022-real-nic-passthrough-netns-move.md) (visibility is no longer the only record of who holds an interface), [ADR-0028](0028-gpu-passthrough-grouped-device-grants.md) (a GPU node is reported once), [ADR-0048](0048-persistent-device-name-mappings.md) (an `exact` selector names a record id from the new set) and [ADR-0161](0161-usb-hotplug-and-composite-device-extension.md) (a hotplug grant asks the same question as every other grant) and [ADR-0265](0265-a-radio-moves-as-a-phy-not-as-an-interface.md) (a radio's PHY is a record, because it is what moves). It keeps #356's exclusivity rule and its `shared` opt-in unchanged in meaning.

## Context

Everything below was measured on 192.168.15.95 on 2026-10-20 with `GET /v1/devices`, `GET /v1/containers` and `cixctl storage ls`, or read from the code at `7b70220e`. Each item says which.

**The page cannot answer the question, and the API cannot either.** The Devices page has one table per bus (USB, PCI, Network, GPU) with the columns ID, Description, Driver, Assignable, Name. It was rendered in Chromium from the box's own responses. It has no column for who holds a device, though the API already returns `held_by`. The API has a `disk` bus (`disk:sr0`), and the page has no table for it.

**One piece of hardware can be two records with opposite answers** (measured):

| Hardware | Records in `GET /v1/devices` |
|---|---|
| the first display adapter | `pci:0000:00:01.0:card0`, assignable, `dev_path` `/dev/card0`; and `gpu:0:card0`, not assignable, `dev_path` `/dev/dri/card0` |
| `vda` | `pci:0000:06:0a.0:vda`, assignable. The disk walk skips it because it has a storage role. |
| `sda` | absent: the disk walk skips it for its role, and the PCI walk finds no node under `ahci` |

Two walks reach the same `/dev` node: the PCI walk (`device.c:380-394`, every node owned by a PCI function) and the class walks (`enumerate_gpu()`, the disk walk at `:805`). Their rules differ, so whether a disk is offered depends on which walk reached it. The comment at `:486-490` says the PCI walk avoids reporting a card twice. That holds for its placeholder entry only; its node entry still reports `card0`.

**Exclusivity is matched on the id string** (read from the code). `registry_device_grant_conflict()` and `registry_device_holders()` (`registry.c:1120`, `:1148`) compare `devices[j].id` with `strcmp`. The kernel enforces a grant per node: `BPF_CGROUP_DEVICE` sees a type, a major and a minor. So the two `card0` ids could each be granted exclusively to a different container while both name 226:0. That has not been exercised.

**"Assignable" means four different things** (read from the code):

| Walk | Assignable when |
|---|---|
| USB (`:251`), GPU (`:728`) | a driver is bound |
| PCI (`:394` vs `:517`) | an owned `/dev` node was found |
| disk (`:825`) | always, after skipping the OS disk and roled disks |
| net (`:587`) | the interface is not a bridge port |

None of the four rules asks whether the host is using the device. For a network interface, the only host use the net rule sees is bridge membership. The interface that carries the management address is protected on .95 only because it happens to be a bridge port (`eth0`, measured not assignable). `interfaces[]` at `main.c:15198-15207` checks nothing beyond `assignable` (read from the code). So on a host whose management NIC is not bridged, `interfaces: ["eth0"]` would move it into a container and take the box off the network. The OS disk `vdb` and its partitions mounted at `/boot`, `/config` and `/var/lib/cix` are all `assignable: true` through their PCI ids (measured, #620). The grant paths check only that flag. Container create at `main.c:15100-15112` and attach at `:19285-19303` call no OS-disk or role check. So naming `pci:0000:06:0c.0:vdb` in a container would very likely be granted. That has deliberately not been tried.

**Not every grant path asks about holders** (read from the code). Create and attach call `registry_device_grant_conflict()`. The hotplug reconcile (`main.c:5040-5110`) checks contention only among containers that are waiting for the device. It then calls `live_attach_one_device()`, which calls no holder check. So a hotplugged device can be granted to a waiting container while a running container already holds it exclusively.

**What a container holds is partly invisible.** `ar-1` holds `wlan0` through `interfaces[]` (measured). A netdev moved into a container's network namespace stops appearing in the host's walk (`device.c:560-570`: *"the same visibility rule that makes exclusivity automatic"*). So the page cannot say that `wlan0` is held, or by whom. It also cannot say that `wlan0` is the radio of the USB dongle `usb:2357:012e:123456`. That dongle's own usbfs node is listed as assignable, with nothing linking the two, and nothing stops a second container from being given the dongle's raw USB node while its radio is in `ar-1`.

**What a stopped container will claim is not shown anywhere.** `registry.h:900-922` defines `held_by` as running containers only, deliberately. Pending optional claims (ADR-0161 phase B) are excluded too.

On .95 today no container declares `devices[]`, no named mapping exists, and the only grant is `ar-1`'s `interfaces: ["wlan0"]` (measured). So changing identity now strands no live grant.

## Decision

**A physical device is one record. The daemon decides its state in one function. Every grant path, `GET /v1/devices`, the dashboard and cixctl use that one answer.**

### 1. One record per physical device, arranged as a tree

A record is a thing the kernel models as a device and Cix can grant or reason about:
- a USB device;
- a PCI function;
- a block device (a disk or a partition);
- a DRM card;
- a wireless PHY;
- a network interface.

Each record carries `parent`: the **nearest sysfs ancestor that is itself a record**. That is not always the sysfs `device` link. On .95, `card0`'s device is the platform device `simple-framebuffer.0` (measured: the GPU walk prints `pci simple-framebuffer.0`), and that device is not a record, so the walk goes one hop further up. Examples: `wlan0` → `phy0` → `usb:2357:012e:123456`; `vda` → `pci:0000:06:0a.0`; `vdb1` → `vdb`.

**A radio's PHY is a record between its USB or PCI device and its interfaces**, because the PHY is what moves (ADR-0265). Every interface on a wiphy follows it into the namespace, so two interfaces of one PHY are not independent siblings. A wireless grant is recorded as a grant of the PHY.

**Each `/dev` node and each netdev belongs to exactly one record, the most specific walk that reaches it.**
- A disk node is the disk walk's.
- A DRM node is the GPU walk's.
- A netdev is the net walk's.
- The PCI walk emits the function itself and keeps only nodes no class walk claims.

So `pci:<bdf>:<node>` ids stop existing. `vda` is `disk:vda` with parent `pci:0000:06:0a.0`, and `card0` is `gpu:0:card0` only. The disk walk stops skipping the OS disk and roled disks. It reports them with a state that says why they cannot be granted (point 2), because a hidden disk is the state an operator cannot see.

**Exclusivity is decided on the record id and the tree, never on major:minor.** A USB device's minor is its device number, which changes at every re-plug (`/dev/bus/usb/001/002` becomes `001/003`), so a node is not a stable identity. A gate asserts that no node appears in two records of one enumeration. With `pci:<bdf>:<node>` gone, that gate is what makes one id mean one node.

### 2. One state, decided once

`device_state()` is **pure**. It takes three inputs as data, and reads no file and calls no enumerator:
- the enumeration;
- the registry's grants;
- a struct of host facts.

The caller fills the host-facts struct from `disk_enumerate()`, `diskrole_lookup()`, each link's `master`, and the interface carrying `daemon_config_management_address()`. The function returns exactly one of these states:

| State | Meaning | Grantable |
|---|---|---|
| `free` | nothing holds it, the host does not use it, and it has something to grant | yes |
| `held` | a running container holds it, or holds an ancestor or descendant of it | only if every holder and the request are `shared` (#356, unchanged) |
| `host` | the host uses it, for the `reason` given | no |
| `unavailable` | there is nothing to grant: no driver, or no node; `reason` says which | no |

`held_by` lists each holder as `{container, granted, shared}`. `granted` is the record the container was actually given. So the dongle reads "held by ar-1 via `wlan0`", not as if its USB node were in `ar-1`.

The `host` reasons, each decided by exactly one rule:

| Reason | Rule |
|---|---|
| `os-disk`, `os-partition` | `diskpart_os_layout_untouchable()`: the OS disk itself and its first four structural partitions. This is the rule storage already uses (#9). |
| `storage-role` | the disk or partition holds a role (`diskrole_lookup()`) |
| `mounted` | the host has it mounted, or has a partition on it mounted. This covers `vdb5` at `/var/lib/cix`, which is not structural. |
| `bridge-port` | the interface is enslaved to a bridge |
| `management-interface` | the interface carries the management address, or is a port of the bridge that does |

A partition an operator added to the OS disk's reserved space is decided by `storage-role` and `mounted` alone, so #9 keeps working.

There is no `assignable`: the state replaces it (clean cut-over).

**Holding follows the tree.** A grant conflicts with a grant of the same record, of any ancestor, or of any descendant:
- the whole `vdb` against `vdb1`;
- the dongle's USB node against its radio.

Siblings do not conflict, so two partitions of one disk can go to two containers.

**`reserved_by` is reported beside the state and does not change it.** It lists the stopped definitions that name the device and the containers waiting for it (ADR-0161 phase B). This keeps #356's rule that a stopped container holds nothing. A start replays the stored definition through `create_container_from_body()` (`main.c:18078-18083` says so, and `:18159` is the call), so it asks `device_state()` and is refused with the holder's name if someone else has the device. Whether a reservation should instead block a grant is the owner's call; see Open question.

**Held state comes from the registry's grants, never from what the host can see.** A grant records the record it resolved to, including its chain of parents, at the moment it is made. That is the one time the host can still read a netdev's sysfs. `GET /v1/devices` reports every granted record, so `wlan0` and `phy0` are listed as `held` by `ar-1`, under `usb:2357:012e:123456`, after it has left the host's namespace. Visibility stops being the record of who holds what (ADR-0022, amended). It stays the kernel's own enforcement of it.

### 3. Every grant asks the same question

`device_grant_verdict(record, want_shared)` returns `ok` or a refusal carrying the state, the reason and the holder. It is the only gate on these paths:
- `devices[]` and `interfaces[]` at create, and so at every start;
- `POST /v1/containers/{name}/devices`;
- the hotplug reconcile, which gains the holder check it lacks today.

`registry_device_grant_conflict()` is folded into it, and its string comparison becomes the node and tree comparison above. A refusal is a 409 naming the holder, or a 400 naming the host reason, e.g. `"vdb is the OS disk"`.

### 4. The contract and the two clients

`Device` loses `assignable` and gains:
- `state`, `reason`, `parent`;
- `held_by` as a list of `{container, granted, shared}`;
- `reserved_by`;
- `nodes` (a list of type, major, minor and path).

The `bus` enum gains `disk`, which the daemon already emits. The stale *"Nothing refuses a second grant"* text goes. **State now depends on things outside devices**, so ADR-0339's declarations have to follow. Container mutations already invalidate `listDevices` (five operations, measured). These do not:
- `deleteStorageRole` and role assignment;
- `unmountStorage` and mount;
- `attachNetworkInterface` and `detachNetworkInterface`;
- the management address.

They declare `x-cix-invalidates: [listDevices]` in the same change. Otherwise the drift check finds each missing one after release.

**The dashboard** gets one widget: a Devices table per kind, Disk included, whose first column is the state, as words:
- Free
- Held by ar-1
- Host · OS disk
- Unavailable · no driver

Children are nested under their parent, so `wlan0` sits under the dongle. One function, `deviceActionEligibility()`, reads `state` and decides the row's actions, after `diskActionEligibility()` (web-ux-guidelines). The page derives nothing.

**cixctl** `device ls` prints the same state column.

## What is measured, and what is not

- Measured: the records, ids, `assignable` values, the duplicate `card0`, the `vda`/`sda` split, `ar-1`'s grant, and the absence of any `devices[]` grant or mapping on .95. Also the page's columns, rendered in Chromium from those responses.
- Read from the code, not exercised: the grant paths' missing OS-disk check, the string-keyed exclusivity that lets one node be granted twice, and the hotplug path's missing holder check. None of the three will be exercised on .95 before the fix, because each would put the host's own disk or a held device into a container.
- Not established: whether a USB device's sysfs `device` link resolves to its USB record for every netdev class Cix will meet. The radio is the case in hand. The implementation measures it for `wlan0` on .95 and records what it finds.

## Verification

- **A pure test in `SELFTESTS`** feeds `device_state()` and `device_grant_verdict()` an enumeration and a set of grants and asserts each row of the table above:
  - the OS disk, its partitions, a roled disk and a mounted disk are `host`;
  - an ancestor grant conflicts and a sibling grant does not;
  - a shared pair passes;
  - two interfaces of one PHY conflict;
  - the management interface is `host`;
  - a hotplug claim against an exclusive holder is refused;
  - no node appears in two records.

  The test follows `test_cbsevents`; no daemon or container is needed.
- **On .95 after shipping**, `GET /v1/devices` shows:
  - `vdb` and its partitions as `host`;
  - `vda` and `sda` both present as `host · storage-role`;
  - one `card0`;
  - `wlan0` as `held` by `ar-1`, under its PHY and the dongle;
  - `eth0` as `host`.

  A create naming `vdb` is refused with 400 and its reason.
- The page is rendered in Chromium from the box's responses, as this ADR's own context was.

## Consequences

- An operator sees every device, why it can or cannot be granted, and who has it. The dashboard and cixctl cannot disagree with a grant's refusal, because all of them read the same verdict.
- The host's own disks and its management interface cannot be granted, whichever id or walk reaches them.
- `pci:<bdf>:<node>` ids, and `assignable`, are removed. A definition or an `exact` mapping naming a removed id fails to resolve, with a 400 naming the new id. None exists on .95 (measured), and per the owner's standing rule there is no alias.
- Disk names (`vda`, `sdb`) are not stable across hardware changes. That is true today of `disk:sr0`, and this ADR does not change it. It is the same question #349 asks for interfaces, and a disk gets its answer there.
- Cost, priced in cycles: the daemon (`device.c`, `registry.c`, the four grant paths), the contract, cixctl and the page. One compile probe and one release in the ordinary case, plus a gate for the new test.

## Open question for the owner

Should a stopped definition that names a device **reserve** it, so that granting it to anything else is refused while the definition exists? This ADR proposes **no**: report the reservation, and refuse at the stopped container's start instead. That keeps #356's rule and never refuses a device that is free right now. Reserving would protect a planned assignment and make an unused definition hold hardware indefinitely.

## Alternatives considered

- **Fix #620 alone**, with an OS-disk check at `device.c:394`. Rejected: it leaves two records for one node, so string-keyed exclusivity still grants `card0` twice, and the page still cannot say who holds anything.
- **Compute the state in the dashboard** from `/v1/storage`, `/v1/containers` and `/v1/devices`. Rejected: the page would re-derive a decision the grant path makes separately, which is the parallel implementation the web guidelines forbid, and cixctl would need a third copy.
- **Key exclusivity on major:minor.** Rejected for three reasons:
  - a USB minor changes at every re-plug;
  - a netdev has no node;
  - the dongle's USB node and its radio are different nodes of one physical device.

  The record id and the tree cover all three.
- **Keep hiding host-used disks** instead of reporting them as `host`. Rejected: hiding is what made `sda` disappear while `vda` was offered. A device the host uses is a fact an operator needs, and the reason is the useful part.
