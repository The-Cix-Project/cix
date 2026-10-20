# 0345 — A record id says where a device is; a mapping says which device it is

## Status

Proposed, 2026-10-20. The owner asked whether device names would *"ever be useful or used"*. The answer was that today they are not: `GET /v1/devicemaps` on 192.168.15.95 returns `[]` and no recipe in cix-recipes names one. Given the choice between rebuilding them and removing them, the owner chose to rebuild them. This ADR is that rebuild, written to be accepted before anything is built.

It widens [#349](https://git.home.arpa/itdlabs/cix/issues/349), which asked for one part of it: `interfaces[]` naming a mapping that selects by MAC. It supersedes [ADR-0048](0048-persistent-device-name-mappings.md) on two points: what a mapping kind may select, and when a selector that matches more than one device is an error. It amends [ADR-0161](0161-usb-hotplug-and-composite-device-extension.md) in two places, the USB id format and what an unplug does to a grant. It extends [ADR-0259](0259-an-interface-may-be-named.md)'s `ifname` to `interfaces[]`. [ADR-0344](0344-a-device-is-one-record-with-one-state-and-every-grant-asks-it.md)'s one record and one state are unchanged.

## Context

Everything below was measured on 192.168.15.95 on 2026-10-20 or read from the code at `19c73c75`. Each item says which. The hardware was measured from a scratch container that read `/sys` (a container's sysfs shows the host's buses but only its own network namespace) and from `GET /v1/devices`.

**Mappings exist and nothing uses them.** There are zero mappings on the box. Every recipe that grants hardware is an `ar-1` revision, and they all say `"interfaces": ["wlan0"]`. No recipe names a raw device id either.

**What each bus offers as an identity** (measured):

| Bus | What sysfs carries | Is it an identity? |
|---|---|---|
| USB | The TP-Link dongle reports `2357:012e`, `serial=123456`. The QEMU tablet reports `0627:0001`, `serial=28754-0000:00:1d.7-1`. | **No, not on its own.** `123456` is a factory default, which every unit of that model reports. The tablet's serial is a bus path. A serial is unique only within its own vendor and product, and only when the vendor bothered. |
| USB root hubs | `usb2` and `usb4` both report `serial=0000:07:1b.0`, as `1d6b:0002` and `1d6b:0003`. | The kernel sets a root hub's serial to its controller's name, and its product id to the USB major version. Together they are a location. |
| Disk | `sda` has the serial `QM00013` and the WWN `t10.ATA QEMU HARDDISK QM00013`. `vda`, `vdb` and `sr0` have neither. | A WWN is unique by its own standard. On this box one disk of four has one. |
| Disk, formatted | Storage roles already follow a filesystem UUID (`diskrole_resolve_recorded()`, #255). They were moved to it because a role keyed by kernel name would follow `sda` to whatever disk became `sdb`. | **Yes**, for as long as the filesystem exists. This is already used. |
| PCI | No function on this box has `serial_number`. | No. BDF plus vendor:product is a location. |
| Net | `eth0` has `bc:24:11:42:0f:4f` and `wlan0` has `98:25:4a:5b:de:5b`, both from the record descriptions in `GET /v1/devices`. | **Not established.** Whether either MAC is the permanent one (`addr_assign_type`) has not been measured, because a container sees only its own netns. |

**The USB record id already depends on the serial** (read from the code). `device.c:246-250` builds `usb:<vid>:<pid>:<serial>` when a serial exists and `usb:<vid>:<pid>:port<busnum>-<devpath>` when it does not. The live record is `usb:2357:012e:123456`. Plug in a second dongle of the same model and **two records get one id**. That breaks ADR-0344's one record and one state, and it breaks its grant tree, before any mapping is involved. It has not been exercised: there is one dongle. The comment on the code says *"busnum/devnum are reassigned on every enumeration and must never be used in a stable id"*, and the fallback beside it uses `busnum`.

**A radio is addressed by a name the kernel assigns at probe time** (#349, measured). `ar-1` binds `wlan0` twice: once as `"interfaces": ["wlan0"]` and once as `interface=wlan0` in `hostapd.conf`. With two radios, or a NIC that probes in a different order, `wlan0` becomes different hardware and nothing reports an error, because the name still resolves. `interfaces[]` goes straight to `device_find("net:<ifname>")` (`main.c:15140`) and never looks for a mapping.

**An unplug ends a grant for good** (read from the code). `reconcile_live_device_revocations()` (`main.c:5149`) revokes the grant of any device that has disappeared from a running container. It does not put the reference back in `pending_devices`, and a grant (`struct registry_device_attachment`) does not record which reference it came from. The only references that ever wait for hardware are those absent at create (`main.c:15151`). So a device that leaves a running container and comes back, in any port, is not given back.

**ADR-0048's kinds cannot express any of this** (read from the code):

- `exact` pins a record id. A record id is a stable location for PCI and nothing else. For a disk or an interface it is a kernel name. For USB it is a serial that may not be unique.
- `vendor_model` resolves to every present match and grants all of them. That is right for "every drive of this model" and wrong for "this radio".

## Decision

**A name exists so that a container spec can say which device it wants, once, in a form that survives a re-plug, a second identical device and a reboot that probes in a different order.** It is the one place an identity is declared. The Devices page shows it in its Name column, a recipe carries the name rather than a host-specific selector, and an operator can change the hardware behind a name without editing the container that uses it. Everything below follows from that.

### 1. A record id names a location, and two present records never share one

- **USB:** the id becomes `usb:<vid>:<pid>@<controller>/usb<N>/<devpath>`. Here `<controller>` is the root hub's own serial (the controller's name, a PCI BDF on this box). `<N>` is the USB major version, read from the root hub's product id (`0002` → `usb2`, `0003` → `usb3`); a product id outside `0001`–`0003` is used verbatim. `<devpath>` is the port path below the root hub. The dongle becomes `usb:2357:012e@0000:07:1b.0/usb2/1`.
  - The serial leaves the id.
  - `busnum` leaves the id, because it is assigned in controller registration order and that order is not guaranteed.
- **PCI** is unchanged (`pci:<bdf>`).
- **Disk, net, GPU and PHY** ids stay as kernel names (`disk:sda`, `net:wlan0`, `gpu:0`, `phy:phy0`). The storage API and every operator tool already speak those names. This ADR leaves them as they are, and section 2 gives each of those buses an identity selector instead.
- **A gate refuses two records with one id**, the same shape as ADR-0344's gate that refuses one node in two records, and in the same two places: `test_devicestate` has a fixture with two identical-serial dongles, and the grant snapshot logs any duplicate once. With USB ids built from a location, no present device can produce a collision, so the gate checks the rule rather than enforcing it.
- **A raw reference means whatever has that id now.** `devices: ["disk:sda"]` re-resolves at every start, to whichever disk the kernel called `sda`. That is what a raw reference has always meant, and this ADR keeps it. Only a mapping promises identity.

### 2. A mapping names a device by what that device is

`DeviceMap.kind` becomes this list, one meaning per kind:

| Kind | Selector | Valid on | Resolves to |
|---|---|---|---|
| `exact` | A record id | usb, pci (the two buses whose ids are locations) | That record, if present |
| `vendor_model` | `<vid>:<pid>` | usb, pci | **Every** present match. This is ADR-0048's group grant, unchanged. |
| `usb_serial` | `<vid>:<pid>:<serial>` | usb | The one present device with that vendor, product and serial. `vid` and `pid` are four hex digits each, so the serial is everything after the second colon. |
| `wwn` | A WWN | disk | The one present disk carrying it |
| `fs_uuid` | A filesystem UUID | disk, including partitions | The one present device carrying it, found by the same probe storage roles already use (`disk_probe_fs_uuid()`). There is no second probe. |
| `mac` | A permanent MAC | net | The one interface carrying it, including one already moved into a container |

- **There is no bare `serial` kind.** A serial without its vendor and product is not an identity. If the dongle is unplugged and another vendor's device that also reports `123456` is plugged in, a bare-serial mapping would resolve to it, and the container would get the wrong hardware with no error. That is #349's failure, moved somewhere else. udev's `by-id` names carry vendor and model for the same reason. A disk's serial is likewise unique only per vendor, and a disk with a serial but no WWN is named by `fs_uuid`.
- **Each kind refuses the buses it is not valid on**, with a 400 that names the kind to use instead. `exact` on `disk:sda` is refused because `sda` is a kernel name, not a location, and the refusal names `wwn` and `fs_uuid`. No mapping exists anywhere today, so tightening `exact` breaks nothing.
- **Every kind except `vendor_model` is exclusive.** If an exclusive selector matches two present devices, the result is an error that names both, never a grant of one of them:
  - `GET /v1/devicemaps` shows the mapping as `ambiguous`, listing every match.
  - A grant through it is refused with 409.

  This is how two dongles that both report `123456` are handled. The operator can see the problem, and the fix is an `exact` mapping on the port.
- **`mac` accepts only a permanent address.** Creating a `mac` mapping against an address that is not permanent is a 400. So the open question in #349 about MAC randomisation becomes a fact reported for each device.

**What each record carries for its selectors:**

- A USB record keeps `serial` as a field once it leaves the id.
- A disk record gains `wwn`.
- A net record gains `mac` and `mac_permanent`, the latter true when the kernel's `addr_assign_type` is 0 (`NET_ADDR_PERM`).
- **`registry_interface_record` gains `mac` and `mac_permanent` too, written at grant time.** Once `wlan0` is inside `ar-1`, the host cannot read its address: the interface is in another netns. So a `mac` mapping finds a moved interface through the record its grant left, exactly as ADR-0344 finds its holder.
- **`fs_uuid` is probed only while resolving an `fs_uuid` mapping, and only against disk records.** It is never probed on every enumerate. `disk_probe_fs_uuid()` opens the device and reads a superblock, and the grant gate takes a device snapshot on every create and attach. ADR-0344 measured `GET /v1/devices` at 72–86 ms, against 190–290 ms for `GET /v1/storage`, which probes. A probe in the walk would move the first toward the second.

**GPUs are out of scope, and their rows get no Name… button.** `gpu:<n>` is an enumeration index, so `exact` on it would name whichever card probed first. The stable handle for a GPU is its PCI function. Naming a GPU therefore means granting ADR-0028's group (card, render node, `kfd`) through a `pci:` mapping. That is a new grant meaning, which no current spec needs, and it belongs to its own decision when a box has two GPUs.

### 3. A grant remembers what it was asked for, and an unplug sends it back to waiting

- **`struct registry_device_attachment` gains `ref`**: the string the spec named, whether that was a mapping name or a raw id.
- **When `reconcile_live_device_revocations()` revokes a grant because its device disappeared, the grant's `ref` goes back into `pending_devices`.** The hotplug reconcile that already serves references that were absent at create then gives the device back when something matching that reference appears. Through a mapping that means any port. Through a raw `exact` USB id it means the same port.
- Resolution still happens only where it already happens: at create, at start, and in that reconcile. Nothing is cached across calls. The held state of the returning device follows the grant recorded under its new id (ADR-0344).
- A grant an operator removes with `DELETE .../devices/{id}` does not go back to waiting. Only an unplug does, because an unplug is not a decision.

### 4. `interfaces[]` takes a mapping, and the in-container name is `ifname`

- **An `interfaces[]` entry becomes `{"interface": "...", "ifname": "..."}`**, the same field ADR-0259 gave `networks[]`, under the same rules: 1–15 characters, `[A-Za-z0-9._-]`, not `lo`, `.` or `..`, not `eth<digits>`, and unique within the container. A bare string is shorthand for `{"interface": "<string>"}`, so every existing `ar-1` revision still parses.
- `interface` names a `mac` mapping if one has that name. Otherwise it is a kernel name, exactly as today, through one resolution path. Each entry has to resolve to exactly one interface. Any other kind of mapping there is a 400, because a grant of every match is not a grant of an interface.
- **`ifname` has one source and one default rule.** Absent, it is the mapping's name when `interface` named a mapping, and the kernel name otherwise, which is today's behaviour. A mapping name that cannot be an interface name is a 400 at create naming `ifname` as the fix. So mapping names stay ordinary resource names, and nothing about a host-wide name is forced by one container's choice.
- **The interface is renamed to `ifname` inside the container's netns**, after the move and with the link down. Interface names are unique per netns, so the new name cannot collide with anything on the host. For a radio, the PHY still moves as ADR-0265 decided, and the rename applies to its netdev inside the container. When the container gives the interface back, it returns to the host under the kernel name recorded at grant time.
- **Teardown finds the interface by its `ifname`, recorded with the grant, over netlink inside the container's netns.** Teardown never reads sysfs, which after `setns()` still answers from the host's mount: that is how #345 stranded a radio.
- So `ar-1` becomes `"interfaces": ["radio-ap"]` with `interface=radio-ap`, and neither line changes again, whatever the kernel calls the radio.

### 5. The dashboard and cixctl show a name's state, and the daemon announces it

- **`listDeviceMaps` declares `x-cix-read: state`**, and every change that can move a mapping's `present`, `ambiguous` or holder announces it alongside `listDevices`: `registry_announce()`, the hotplug reconciles, and format completion (which can create or remove an `fs_uuid`). Creating or deleting a mapping invalidates `listDevices` as well as `listDeviceMaps`, because the Devices page's Name column reads mappings. This is ADR-0339's contract, and the drift check enforces it.
- The **Name…** button opens a dialog that lists only the kinds that resolve this device uniquely right now, with the strongest identity preselected: `mac`, then `wwn`, then `usb_serial`, then `fs_uuid`, then `exact`.
- A device with none of them gets no button. On this box that is `sr0`, which has no WWN and, with no medium, no filesystem UUID.
- The Named Mappings tab's "Used by" column reads `held_by`, the same source the Devices page reads, instead of comparing container device lists against `resolved_ids` in the client.
- The tab and `cixctl devicemap ls` show each mapping as present, absent or ambiguous, with what it resolved to. That comes from the daemon, which already re-derives it on every call.

## What is measured, and what is not

**Measured:** every value in the Context table, and the unplug path read from the code.

**Not measured, and the first gate of implementation:**

- **Whether either MAC on this box is permanent.** The daemon reads `addr_assign_type` from the host netns. The first release reports it, and nothing selects by MAC until that value has been read for `eth0` and `wlan0`. If the rtw88 radio's MAC is not permanent, it cannot be named by MAC. The finding then goes into #349 rather than this ADR choosing a weaker selector.
- **Whether the root-hub product id distinguishes every root-hub pair on real hardware**, beyond this box's single xHCI controller.
- **Whether a re-plugged USB device gets its grant back** cannot be exercised on this box without someone at the Proxmox host. The fixture test in Verification covers the mechanism. The live check waits for a box someone can reach physically.
- **GPT disk and partition GUIDs** were left out on purpose. `fs_uuid` covers every formatted disk and is already probed. A blank disk with no filesystem has no identity selector, and on such a disk a GPT GUID would identify a partition table nobody has written either. They can be added as a kind later if a real case appears.

## Verification

1. **The acceptance test from #349.** Create a `mac` mapping `radio-ap` for the dongle's radio and point `ar-1` at it. Then delete and recreate the container three times. Each time:
   - the interface inside is `radio-ap`;
   - `hostapd.conf` is never edited;
   - hostapd reaches `AP-ENABLED`, read from inside the container over its console.
2. **USB ids.** `GET /v1/devices` lists the dongle as `usb:2357:012e@0000:07:1b.0/usb2/1`, and the PHY's `parent` follows it.
3. **Ambiguity.** In `test_devicestate` and `test_daemon_devices`, two fixture devices carry one vendor, product and serial. A `usb_serial` mapping on them lists as `ambiguous` with both matches, and a grant through it is a 409 naming both. An `exact` mapping on one port grants that device and leaves the other free.
4. **Refusals.** Each of these returns 400:
   - `exact` on `disk:sda`, with a message naming `wwn` and `fs_uuid`;
   - a `mac` mapping on a non-permanent address;
   - `ifname: "eth1"`, under ADR-0259's rules;
   - a `vendor_model` mapping in `interfaces[]`.
5. **Unplug and return.** In `test_device_hotplug`, a grant made through a mapping is revoked when its fixture device disappears, appears in `pending_devices`, and is re-attached when the device reappears under a different id.
6. **Announcements.** `test_assert_no_drift()` passes in `test_daemon_devices` after mapping create and delete, a hotplug and a grant.
7. **Dashboard.** Rendered in Chromium from the box. `sr0` has no Name… button, `wlan0`'s dialog preselects `mac`, and "Used by" for a mapping held by `ar-1` reads `ar-1`.

## Consequences

- An operator can write a container spec that survives a re-plug, a second identical device, and a reboot that probes in a different order. That is what the mappings were for, and today they deliver it only for PCI.
- **The USB id format changes, as a clean cut-over.** Nothing on the box or in the corpus names a USB id. A container spec that named the old form would get a 400 naming the new one. A raw USB id now pins a port, which is what an `exact` mapping on it means, and following a device across ports is what a `usb_serial` mapping is for.
- **A device that is unplugged and comes back is given back**, which it never was before. A container that should not get hardware back after an unplug can say so by not naming it.
- `vendor_model` keeps its group meaning, and only for `devices[]`.
- Disk, net, GPU and PHY ids keep their kernel names. The operator-visible vocabulary does not change, and an identity is always a mapping, never a second id.
- The in-container rename adds one netlink operation to the interface path, with `rtnl_link_rename()`, which already exists.
- `openapi.yaml` and `docs/api/README.md` change together: the new kinds and the `ambiguous` state on `DeviceMap`, the object form of `interfaces[]`, `ref` on a device grant, and `wwn`, `mac` and `mac_permanent` on `Device`. `fmt_devicemap_line()` in cixctl and the dashboard's mapping table change with them.

## Alternatives considered

- **Remove mappings.** This was offered to the owner, who did not choose it. A container could then be pinned only to kernel names and serials that are not identities, which is #349's bug on every bus.
- **Selectors inline in the container spec, with no name table** (`"devices": [{"wwn": "..."}]`). Rejected because it puts a host's hardware facts into the recipe. A recipe like `ar-1` is published to every Cix user, and the same radio has a different MAC on every host. With a name, the recipe says `radio-ap` and each host declares once what `radio-ap` is. Inline selectors would also leave the Devices page with nothing to show in its Name column, and changing hardware would mean editing every container that used it.
- **Make the record id itself the identity**, so that a disk's id would be its WWN and an interface's its MAC. Rejected because most devices on this box have no such value: three of four disks have no WWN, and the dongle's serial is shared with every unit of its model. Every bus would then need a fallback id, the fallback would be a location, and each id would mean one of two things depending on the device. A location id plus an identity mapping means one thing each.
- **Keep the serial in the USB id, and fall back to the port only when two present devices share it.** Rejected because plugging in a second dongle would then change the first dongle's id, and every grant and `exact` mapping on it would break without either device having moved.
- **A bare `serial` kind.** Rejected for the reason given under section 2: a serial is unique only within a vendor and product.
- **The mapping's name as the in-container name, with no `ifname`.** Rejected because ADR-0259 already made `ifname` the in-container name. Two sources for one name is the parallel the maxims forbid, and it would force every `mac` mapping name through interface-name rules for one container's sake.
- **Pick the first match for an ambiguous exclusive selector.** Rejected because which match comes first is enumeration order, the very instability this ADR removes.
