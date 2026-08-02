# 0048 — Persistent device name mappings: exact vs. vendor/model resolution

## Status

Accepted

## Context

Raised directly by the user, framed against a familiar real-world precedent: "can we make each device an item under the devices tree only if we've named it? Naming it will work exactly like Proxmox's device mapping, which could be specific or could be on vendor/model, since then we can move it around on USB ports?"

Investigated first: `daemon/src/device.c`'s host-hardware discovery (`device_enumerate()`, `GET /v1/devices`) is deliberately never persisted — re-enumerated fresh from `/sys/bus/usb`/`/sys/bus/pci` on every call, since this is host hardware the daemon doesn't create or own (`device.h`'s own comment). That's the correct call for *discovery*, but leaves nothing an operator can build a stable container spec around: a USB device's own `id` (`daemon/include/device.h`) bakes in either its real serial number or, for a device with no serial, its physical port-topology path (`usb:<vendor>:<product>:port<bus>-<devpath>`) — so a container referencing a raw id either breaks the instant that specific device is unplugged and replugged into a different port, or has to be permanently pinned to one exact device forever, with no way to say "whichever device of this make/model is currently plugged in."

## Decision

A new, genuinely separate persisted layer, `daemon/src/devicemap.c`/`.h`: a mapping is `{name, kind, selector}` — `name` an operator-chosen label (`simple_name_is_valid()`, the same charset every other simple resource name in this project already uses — container/network/package — reused, not reimplemented; its exclusion of `:` means a mapping name can never collide with the real device-id namespace, every one of which always contains at least one `:`). Two supported kinds, both requested directly by the user rather than picking one:

- **`exact`**: selector is a real `device.h` id verbatim, pinning to one specific bus location (e.g. a specific PCI slot, or one exact USB device identified by its own serial).
- **`vendor_model`**: selector is `"<vendor_id>:<product_id>"`, matching regardless of physical port — this is what actually lets a USB device move between ports and still resolve, the concrete scenario the user named.

Resolution (`devicemap_resolve()`) is never cached across calls — it re-runs `device_enumerate()` fresh every time, mirroring `device_find()`/`device_find_group()`'s own existing "simple and correct rather than cached" precedent exactly, since host hardware can change between any two calls. For `vendor_model`, more than one currently-present device can legitimately match (several identical USB devices plugged in at once) — every match is returned and, when used in a container's own `devices` field, every one is granted, the same group-expansion behavior GPU passthrough (ADR-0028) already established for `"gpu:0"` — reusing an existing pattern rather than inventing a second kind of ambiguity handling.

A mapping is real and creatable even when its device is not currently plugged in — `GET /v1/devicemaps` reports `present`/`resolved_ids`, re-derived fresh each call, rather than refusing to let the mapping exist at all. An operator predefining "the USB serial adapter" before physically plugging it in, or one that's temporarily disconnected, are both legitimate, ordinary states — not errors.

**Container integration**: `main.c`'s existing `devices[]` parsing (`create_container_from_body()`) now tries `devicemap_resolve(id, ...)` before falling back to the pre-existing raw-id `device_find_group(id, ...)` path. The two return codes are distinguished precisely: `devicemap_resolve()` returns `-1` when `id` does not name any mapping at all (falls through to the raw-id path, fully backward-compatible — every existing container spec using a raw id keeps working unchanged), but `0` when `id` *is* a real mapping that currently resolves to nothing (a real 400, not silently reinterpreted as a literal device id that happens to share the mapping's name). This asymmetry is deliberate: a mapping existing-but-absent is meaningfully different from the name simply not being a mapping, and conflating the two would either mask a real "your device isn't plugged in" error or make an unrelated mapping name accidentally shadow a coincidentally-identical raw id.

**Web UI** (Phase 15): the Devices tree category, previously a flat link with no children at all, now lists one child per *named* mapping only — raw, unnamed hardware is never shown in the tree, exactly as asked. The Devices detail view itself gained a tab-bar split by bus (USB / PCI / Network / GPU, reusing the `.tab-bar`/`.tab-panel[data-tab]` pattern Phase 32 already established for the image detail view — the page's one generic tab-switching handler picked these up with zero new JS) plus a "Named mappings" table showing every mapping's live present/absent status. A "Name…" button on each unmapped device row opens the shared modal, pre-filled with that device's own id as the default `exact` selector — switchable to `vendor_model` by the operator if they'd rather the mapping follow the device across ports.

## Consequences

- A real, working "move it around on USB ports" story now exists: a `vendor_model` mapping created against a device plugged into one port keeps resolving correctly after it's unplugged and replugged into a different one — verified directly (`test/test_daemon_devices.c`, Phase 15 additions).
- Fully backward-compatible: every existing container spec naming a raw device id is untouched; the mapping layer is purely additive.
- Deleting a mapping does not retroactively affect any container already using it — device grants are resolved once, at container-creation time, into a fixed BPF_CGROUP_DEVICE grant (ADR-0017); there is no live re-resolution mechanism, matching how a raw-id grant already behaves.
- No mapping-name uniqueness enforcement across kinds beyond the name itself (an operator could create both an `exact` and a `vendor_model` mapping for what is, physically, the same device, under two different names) — not prevented, since there is no way to know they refer to "the same" device in the general case, and no real harm from allowing it.
