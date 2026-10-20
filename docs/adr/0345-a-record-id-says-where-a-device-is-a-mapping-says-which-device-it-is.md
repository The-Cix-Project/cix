# 0345 — A record id says where a device is; a mapping says which device it is

## Status

Proposed, 2026-10-20. The owner asked whether device names would *"ever be useful or used"*. The answer was that today they are not: `GET /v1/devicemaps` on 192.168.15.95 returns `[]` and no recipe in cix-recipes names one. Given the choice between rebuilding them and removing them, the owner chose to rebuild them. This ADR is that rebuild, written to be accepted before anything is built.

It widens [#349](https://git.home.arpa/itdlabs/cix/issues/349), which asked for one part of it: `interfaces[]` naming a mapping that selects by MAC. It supersedes [ADR-0048](0048-persistent-device-name-mappings.md) on two points: what a mapping kind may select, and when a selector that matches more than one device is an error. It amends the USB id format of [ADR-0161](0161-usb-hotplug-and-composite-device-extension.md). It extends [ADR-0259](0259-an-interface-may-be-named.md) with a second source for an interface's in-container name. [ADR-0344](0344-a-device-is-one-record-with-one-state-and-every-grant-asks-it.md)'s one record and one state are unchanged.

## Context

Everything below was measured on 192.168.15.95 on 2026-10-20 or read from the code at `19c73c75`. Each item says which. The hardware was measured from a scratch container that read `/sys` (a container's sysfs shows the host's buses but only its own network namespace) and from `GET /v1/devices`.

**Mappings exist and nothing uses them.** There are zero mappings on the box. Every recipe that grants hardware is an `ar-1` revision, and they all say `"interfaces": ["wlan0"]`. No recipe names a raw device id either.

**What each bus offers as an identity** (measured):

| Bus | What sysfs carries | Is it an identity? |
|---|---|---|
| USB | The TP-Link dongle reports `2357:012e`, `serial=123456`. The QEMU tablet reports `0627:0001`, `serial=28754-0000:00:1d.7-1`. | **No.** `123456` is a factory default, which every unit of that model reports. The tablet's serial is a bus path. |
| USB root hubs | `usb2` and `usb4` both report `serial=0000:07:1b.0`, as `1d6b:0002` and `1d6b:0003`. | The kernel sets a root hub's serial to its controller's name. That name plus the USB version (`0002` is 2.0, `0003` is 3.0) is a location. |
| Disk | `sda` has the serial `QM00013` and the WWN `t10.ATA QEMU HARDDISK QM00013`. `vda`, `vdb` and `sr0` have neither. | A serial or WWN is an identity when it is present. On this box it is present for one disk of four. |
| Disk, formatted | Storage roles already follow a filesystem UUID (`diskrole_resolve_recorded()`, #255). They were moved to it because a role keyed by kernel name would follow `sda` to whatever disk became `sdb`. | **Yes**, for as long as the filesystem exists. This is already used. |
| PCI | No function on this box has `serial_number`. | No. BDF plus vendor:product is a location. |
| Net | `eth0` has `bc:24:11:42:0f:4f` and `wlan0` has `98:25:4a:5b:de:5b`, both from the record descriptions in `GET /v1/devices`. | **Not established.** Whether either MAC is the permanent one (`addr_assign_type`) has not been measured, because a container sees only its own netns. |

**The USB record id already depends on the serial** (read from the code). `device.c:246-250` builds `usb:<vid>:<pid>:<serial>` when a serial exists and `usb:<vid>:<pid>:port<busnum>-<devpath>` when it does not. The live record is `usb:2357:012e:123456`. Plug in a second dongle of the same model and **two records get one id**. That breaks ADR-0344's one record and one state, and it breaks its grant tree, before any mapping is involved. It has not been exercised: there is one dongle. The comment on the code says *"busnum/devnum are reassigned on every enumeration and must never be used in a stable id"*, and the fallback beside it uses `busnum`.

**A radio is addressed by a name the kernel assigns at probe time** (#349, measured). `ar-1` binds `wlan0` twice: once as `"interfaces": ["wlan0"]` and once as `interface=wlan0` in `hostapd.conf`. With two radios, or a NIC that probes in a different order, `wlan0` becomes different hardware and nothing reports an error, because the name still resolves. `interfaces[]` goes straight to `device_find("net:<ifname>")` (`main.c:15140`) and never looks for a mapping.

**ADR-0048's kinds cannot express any of this** (read from the code):

- `exact` pins a record id. A record id is a stable location for PCI and nothing else. For a disk or an interface it is a kernel name. For USB it is a serial that may not be unique.
- `vendor_model` resolves to every present match and grants all of them. That is right for "every drive of this model" and wrong for "this radio".

## Decision

### 1. A record id names a location, and two present records never share one

- **USB:** the id becomes `usb:<vid>:<pid>@<controller>-<version>-<devpath>`. Here `<controller>` is the root hub's own serial (the controller's name, a PCI BDF on this box), `<version>` is the root hub's product id, whatever it is (`0002` or `0003` on this box), and `<devpath>` is the port path below it. The dongle becomes `usb:2357:012e@0000:07:1b.0-0002-1`.
  - The serial leaves the id.
  - `busnum` leaves the id, because it is assigned in controller registration order and that order is not guaranteed.
- **PCI** is unchanged (`pci:<bdf>`).
- **Disk, net, GPU and PHY** ids stay as kernel names (`disk:sda`, `net:wlan0`, `gpu:0`, `phy:phy0`). The storage API and every operator tool already speak those names. This ADR leaves them as they are, and section 2 gives each of those buses an identity selector instead.
- **A gate refuses two records with one id**, the same shape as ADR-0344's gate that refuses one node in two records, and in the same two places: `test_devicestate` has a fixture with two identical-serial dongles, and the grant snapshot logs any duplicate once. With USB ids built from a location, no present device can produce a collision, so the gate checks the rule rather than enforcing it.

### 2. A mapping names a device by what that device is

`DeviceMap.kind` becomes:

| Kind | Selector | Valid on | Resolves to |
|---|---|---|---|
| `exact` | A record id | usb, pci, the two buses whose ids are locations | That record, if present |
| `vendor_model` | `<vid>:<pid>` | usb, pci | **Every** present match. This is ADR-0048's group grant, unchanged. |
| `serial` | A hardware serial or WWN | usb, disk | The one present device carrying it |
| `fs_uuid` | A filesystem UUID | disk, including partitions | The one present device carrying it, found by the same probe storage roles already use (`disk_probe_fs_uuid()`). There is no second probe. |
| `mac` | A permanent MAC | net | The one interface carrying it, including one already moved into a container |

**What each record carries for its selectors:**

- A USB record keeps `serial` as a field once it leaves the id.
- A disk record gains `serial` and `wwn`. A `serial` selector matches a device whose serial or WWN equals it. When a disk has both, as `sda` does, the dashboard preselects the WWN, because a WWN is unique by its own standard and a serial is unique only by vendor convention.
- A net record gains `mac` and `mac_permanent`.
- **`registry_interface_record` gains `mac` and `mac_permanent` too, written at grant time.** Once `wlan0` is inside `ar-1`, the host cannot read its address: the interface is in another netns. So a `mac` mapping finds a moved interface through the record its grant left, exactly as ADR-0344 finds its holder.
- **`fs_uuid` is probed only while resolving an `fs_uuid` mapping, and only against disk records.** It is never probed on every enumerate. `disk_probe_fs_uuid()` opens the device and reads a superblock, and the grant gate takes a device snapshot on every create and attach. ADR-0344 measured `GET /v1/devices` at 72–86 ms, against 190–290 ms for `GET /v1/storage`, which probes. A probe in the walk would move the first toward the second.

**GPUs are out of scope, and their rows get no Name… button.** `gpu:<n>` is an enumeration index, so `exact` on it would name whichever card probed first. The stable handle for a GPU is its PCI function. Naming a GPU therefore means granting ADR-0028's group (card, render node, `kfd`) through a `pci:` mapping. That is a new grant meaning, which no current spec needs, and it belongs to its own decision when a box has two GPUs.

- **Each kind refuses the buses it is not valid on**, with a 400 that names the kind to use instead. `exact` on `disk:sda` is refused because `sda` is a kernel name, not a location, and the refusal names `serial` and `fs_uuid`. No mapping exists anywhere today, so tightening `exact` breaks nothing.
- **Every kind except `vendor_model` is exclusive.** If an exclusive selector matches two present devices, the result is an error that names both, never a grant of one of them:
  - `GET /v1/devicemaps` shows the mapping as `ambiguous`, listing every match.
  - A grant through it is refused with 409.

  This is how two dongles that both report `123456` are handled. The operator can see the problem, and the fix is an `exact` mapping on the port.
- **`mac` accepts only a permanent address.** The net record gains `mac` and `mac_permanent`, the latter true when the kernel's `addr_assign_type` is 0 (`NET_ADDR_PERM`). Creating a `mac` mapping against an address that is not permanent is a 400. So the open question in #349 about MAC randomisation becomes a fact reported for each device.
- **Resolution happens where it already happens:** at create, at start, and in the hotplug reconcile of pending devices (`reconcile_pending_device_attachments()`, `main.c:5037`). A device that re-plugs into another port therefore keeps its mapping. Its record id changes, the held state follows the grant recorded under the new id (ADR-0344), and nothing is cached across calls.

### 3. `interfaces[]` accepts a mapping, and the mapping's name is the interface's name inside

- If a string in `interfaces[]` names a `mac` mapping, it resolves through that mapping. Otherwise it is a kernel name, exactly as today, through one resolution path. Each string has to resolve to exactly one interface. A `vendor_model` mapping there is a 400, because a grant of every match is not a grant of an interface.
- **The interface is renamed to the mapping's name inside the container's netns**, after the move and with the link down. Interface names are unique per netns, so the new name cannot collide with anything on the host. For a radio, the PHY still moves as ADR-0265 decided, and the rename applies to its netdev inside the container. When the container gives the interface back, it returns to the host under the kernel name recorded at grant time.
- **Teardown finds the interface by its in-container name, over netlink inside the container's netns.** That name is the mapping's name, recorded with the grant. Teardown never reads sysfs, which after `setns()` still answers from the host's mount: that is how #345 stranded a radio.
- **A `mac` mapping's name must be a valid in-container interface name** under ADR-0259's rules: 1–15 characters, `[A-Za-z0-9._-]`, not `lo`, `.` or `..`, and not `eth<digits>`. This is checked when the mapping is created, so a name that cannot be used is never accepted in the first place.
- So `ar-1` becomes `"interfaces": ["radio-ap"]` with `interface=radio-ap`, and neither line changes again, whatever the kernel calls the radio.

### 4. The dashboard offers a name only where one would hold

- The **Name…** button opens a dialog that lists only the kinds that resolve this device uniquely right now, with the strongest identity preselected: `mac`, then `serial`, then `fs_uuid`, then `exact`.
- A device with none of them gets no button. On this box that is `sr0`, which has no serial, no WWN and, with no medium, no filesystem UUID.
- The Named Mappings tab's "Used by" column reads `held_by`, the same source the Devices page reads, instead of comparing container device lists against `resolved_ids` in the client.
- The tab shows each mapping as present, absent or ambiguous, with what it resolved to. That comes from the daemon, which already re-derives it on every call.

## What is measured, and what is not

**Measured:** every value in the Context table.

**Not measured, and the first gate of implementation:**

- **Whether either MAC on this box is permanent.** The daemon reads `addr_assign_type` from the host netns. The first release reports it, and nothing selects by MAC until that value has been read for `eth0` and `wlan0`. If the rtw88 radio's MAC is not permanent, it cannot be named by MAC. The finding then goes into #349 rather than this ADR choosing a weaker selector.
- **Whether `0002`/`0003` distinguishes every root-hub pair on real hardware**, beyond this box's single xHCI controller.
- **GPT disk and partition GUIDs** were left out on purpose. `fs_uuid` covers every formatted disk and is already probed. A blank disk with no filesystem has no identity selector, and on such a disk a GPT GUID would identify a partition table nobody has written either. They can be added as a kind later if a real case appears.

## Verification

1. **The acceptance test from #349.** Create a `mac` mapping `radio-ap` for the dongle's radio and point `ar-1` at it. Then delete and recreate the container three times. Each time:
   - the interface inside is `radio-ap`;
   - `hostapd.conf` is never edited;
   - hostapd reaches `AP-ENABLED`, read from inside the container over its console.
2. **USB ids.** `GET /v1/devices` lists the dongle as `usb:2357:012e@0000:07:1b.0-0002-1`, and the PHY's `parent` follows it.
3. **Ambiguity.** In `test_devicestate` and `test_daemon_devices`, two fixture devices carry one serial. A `serial` mapping on that serial lists as `ambiguous` with both matches, and a grant through it is a 409 naming both. An `exact` mapping on one port grants that device and leaves the other free.
4. **Refusals.** `exact` on `disk:sda` returns 400 naming `serial`/`fs_uuid`. A `mac` mapping on a non-permanent address returns 400. A `mac` mapping named `eth1` returns 400 under ADR-0259's rules. A `vendor_model` mapping in `interfaces[]` returns 400.
5. **Dashboard.** Rendered in Chromium from the box. `sr0` has no Name… button, `wlan0`'s dialog preselects `mac`, and "Used by" for a mapping held by `ar-1` reads `ar-1`.

## Consequences

- An operator can write a container spec that survives a re-plug, a second identical device, and a reboot that probes in a different order. That is what the mappings were for, and today they deliver it only for PCI.
- **The USB id format changes, as a clean cut-over.** Nothing on the box or in the corpus names a USB id. A container spec that named the old form would get a 400 naming the new one. A raw USB id now pins a port, which is what an `exact` mapping on it means, and following a device across ports is what a `serial` mapping is for.
- `vendor_model` keeps its group meaning, and only for `devices[]`.
- Disk, net, GPU and PHY ids keep their kernel names. The operator-visible vocabulary does not change, and an identity is always a mapping, never a second id.
- The in-container rename adds one netlink operation to the interface path, with `rtnl_link_rename()`, which already exists.
- `fmt_devicemap_line()` in cixctl and the dashboard's mapping table change with the new kinds and the ambiguous state. `openapi.yaml` and `docs/api/README.md` change together.

## Alternatives considered

- **Remove mappings.** This was offered to the owner, who did not choose it. A container could then be pinned only to kernel names and serials that are not identities, which is #349's bug on every bus.
- **Make the record id itself the identity**, so that a disk's id would be its serial and an interface's its MAC. Rejected because most devices on this box have no such value: three of four disks have no serial, and the dongle's serial is shared with every unit of its model. Every bus would then need a fallback id, the fallback would be a location, and each id would mean one of two things depending on the device. A location id plus an identity mapping means one thing each.
- **Keep the serial in the USB id, and fall back to the port only when two present devices share it.** Rejected because plugging in a second dongle would then change the first dongle's id, and every grant and `exact` mapping on it would break without either device having moved.
- **Pick the first match for an ambiguous exclusive selector.** Rejected because which match comes first is enumeration order, the very instability this ADR removes.
