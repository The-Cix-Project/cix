# 0179 — Full user namespaces by default, subordinate ID ranges keyed to identity (issue #29)

## Status

Accepted; phased. **Phase 1 (the subordinate-ID allocator) is built and
tested** (`daemon/src/subid.c`, `test/test_subid.c`, wired into boot init
and into `ldap.c`'s own user-create/update validation as the symmetric
collision check). **Phase 2 (the actual `CLONE_NEWUSER` + id-mapped-mount
flip) is deliberately NOT yet enabled** -- `container_spec` carries the
`userns_*` fields but nothing sets `userns_enabled` yet.

The real, concrete prerequisite (corrected 2026-08-21 after a direct check
prompted by the user -- an earlier version of this Status wrongly framed
Phase 2 as "gated on the incoming bare-metal box"): **the deployed kernel
was not built with `CONFIG_USER_NS=y`**. `image/kernel/qemu-part1.config`
compiles in `NAMESPACES`/`PID_NS`/`NET_NS`/`UTS_NS` but never requested
`USER_NS`, so `CLONE_NEWUSER` returns `EINVAL` on the current 6.18.40
kernel regardless of where it runs. Phase 2 therefore blocks on a kernel
rebuild that adds `CONFIG_USER_NS=y` (the same deliberate kernel-rebuild
path used to close issue #1 for `CFS_BANDWIDTH`/`BLK_CGROUP`), then a
redeploy to the real target.

Once that kernel is deployed, verification does **not** need bare metal.
The two constraints were conflated before: the *dev/build sandbox* (where
this project's shell tooling runs) is a privileged nested LXC with
documented namespace constraints (root `CLAUDE.md`), and cannot faithfully
exercise id-mapped mounts -- confirmed directly here 2026-08-21 with a
standalone probe: even writing `0 0 1` (the single-line self-map the
kernel's own userns rules *always* permit) to a child's `/proc/<pid>/uid_map`
returns `EPERM`, i.e. the write is blocked above the kernel's userns logic
by this privileged LXC's own LSM profile, so the id-mapped-mount sequence
cannot even begin here regardless of map contents -- but the real
deployment target (192.168.15.95)
is a Proxmox **VM** that owns its own guest kernel, which is a fully
faithful environment for `mount_setattr(MOUNT_ATTR_IDMAP)` over
overlayfs+ext4 (pure kernel-VFS behaviour, independent of virtio-vs-physical
block devices). Bare metal is only required for the orthogonal
hardware-passthrough work, which #29 does not touch. The id-mapped-mount
path over this project's specific overlayfs+ext4 stack still genuinely
needs empirical verification before a security-posture-changing default is
flipped -- exactly the "confirm directly, don't assume" discipline this
ADR's own text already commits to -- but that verification runs on the .95
VM. Issue #29 stays open for Phase 2.

**Phase 2a verified live on .95 2026-08-21, and it settled the open design
question empirically.** With the `USER_NS` kernel deployed, `container.c`
gained `CLONE_NEWUSER` + the parent-side `setgroups`/`gid_map`/`uid_map`
handshake (a blocking sync pipe: child waits, parent writes the maps then
releases it), gated behind an opt-in `"userns":true` create field so the
platform's running containers stayed untouched. A `"userns":true` container
was created and got as far as `overlay_create` -- proving `CLONE_NEWUSER`
and the map handshake both work on the real VM (a failed map write would
have `_exit(121)`; this reached the overlay step) -- then **failed with
`EACCES` mounting the overlay *inside* the new user namespace** (`exit 153`
= `140 + EACCES`, `"child: overlay_create: Permission denied"`). This is
the direct, live confirmation of exactly the risk this ADR flagged: the
child, running as the namespace's mapped root (host uid `userns_uid_base`,
e.g. 100000), cannot write the host-uid-0-owned upperdir/workdir, so an
overlay mount *by the child* is not viable. **Phase 2b therefore mounts the
overlay parent-side (init userns, full privilege) before `clone3` and
re-presents it to the child** -- which is what this ADR's own Consequences
already specified ("the merged overlay is re-presented through a real
id-mapped mount created BEFORE clone3()"); the live failure is what turns
that from a design assertion into a verified requirement.

**Phase 2b done + verified live on .95 2026-08-21 — a userns container now
runs with a correctly mapped root.** `container_create()` mounts the overlay
parent-side (init userns) before `clone3`; the child inherits it and pivots
in. Getting there took three successive live findings, each an
unprivileged-userns kernel interaction the non-userns path never hits:
(1) the overlay must be mounted parent-side (the phase-2a `EACCES`);
(2) `pivot_root` refuses a `MNT_LOCKED` new root, and mounts inherited into
a userns-owned mount ns are locked — fixed by a fresh self-bind-mount of
`merged` in the child (unlocked); (3) an unprivileged userns may only mount
proc/sysfs when a fully-visible instance already exists — fixed by detaching
the old root *after* the fresh proc/sys mounts, not before. Result confirmed
directly: `/proc/1/uid_map` inside the container reads `0 100000 65536` — the
container's root is genuinely mapped to an unprivileged host uid, not host 0.

**Phase 2c (id-mapped mounts) is the remaining work, and it is required for
a *usable* container, not just a running one.** Without it the container is
effectively read-only: every writable target (the overlay rootfs, and even
the fresh tmpfs `/run`) is owned by host uid 0, which is *outside* the
container's mapped range [100000,165536) and therefore unmapped in its
userns — so the container gets `EOVERFLOW` ("Value too large for defined
data type") the moment it touches such a file (confirmed live: a container's
own `bash` failed exactly this way writing `/run`). A world-readable binary
still execs (that path never has to represent the unmapped owner), which is
why the container runs at all. Id-mapped mounts (`mount_setattr(MOUNT_ATTR_IDMAP)`,
this ADR's stated primary mechanism) re-present the overlay/tmpfs inodes with
kuids shifted into the container's mapped range, eliminating the `EOVERFLOW`
and giving the mapped root real ownership of its rootfs. This is tracked
under issue #29 (which stays open); userns stays opt-in (`"userns":true`),
never default-on, until it lands. The subordinate-ID allocator, the
`CLONE_NEWUSER`+map handshake, and the parent-side overlay mount are all
committed and verified; only the id-mapped presentation remains.

**Phase 2c live-found constraint (2026-08-21): `mount_setattr(MOUNT_ATTR_IDMAP)`
on the *merged* overlay mount returns `EINVAL` on the 6.18 kernel.** Overlayfs
does not support id-mapping the merged mount directly -- only its *layers* can
be id-mapped. So the working approach is id-mapped **lower/upper layers**
(idmapped bind mounts of the layers fed to overlay via the fd-based mount API,
`fsopen`/`fsconfig`), not a single `mount_setattr` on the merged mount. The
first-cut 2c plumbing is committed and correct as far as it goes -- the
detached-clone + `open_tree`/`move_mount` flow, and a race-fixed
`create_idmap_userns_fd` that builds the inverse idmap userns `"<base> 0
<len>"` (the fork/`unshare` race -- parent writing the map before the child
left the init userns, intermittent `EPERM` -- is fixed with a ready-handshake).
Substituting the layer-idmap for the merged-mount `mount_setattr` is the
remaining work. It was then built (`fsopen`/`fsconfig`/`fsmount`, id-mapped
layers) and the overlay now **mounts and is writable** -- but two subtle walls
remain, and closing them efficiently is blocked by not being able to read
`dmesg` on the shell-less box: **(a)** id-mapping only the shared lower (upper/
work left plain) mounts writable but `mkdir(/dev/pts)` fails `EACCES` -- copy-up
of an existing host-0 lower dir into the non-id-mapped upper preserves host-0,
which the container userns can't map, so upper must be id-mapped consistently;
**(b)** id-mapping upper+work too (through one id-mapped base mount, since
`ovl_get_workdir` requires upper and work on the same vfsmount) mounts
**read-only** via `ovl_make_workdir` failing to create `work/` -- the exact
kernel `pr_warn` reason is unreadable without kernel-log visibility. Confirmed
from 6.18 source: layer fds carry their idmap (`ovl_parse_layer`), and
`clone_private_mount` accepts an id-mapped subdir mount and preserves the
mapping. **Next unblock: expose the kernel log (`/dev/kmsg`) via a thincd
endpoint** so the overlay read-only reason is visible, then finish
all-layers-idmap; the per-container reflink/copy of the rootfs (owned by base)
remains the "last resort" if it stays intractable. userns stays opt-in; no
working userns container yet.

**kmsg endpoint added, and it closed the diagnosis (2026-08-21).** A new
`GET /v1/system/kmsg` (tails `/dev/kmsg`) gave the exact reason the all-layers
id-mapped overlay mounts read-only: `overlayfs: failed to create directory
/work/work (errno: 13 EACCES); mounting read-only`. Root cause: **thincd mounts
the overlay from *outside* the user namespace (as host uid 0), but on the
id-mapped upper/work it is a non-owner, so overlay's own `work/` creation --
performed as thincd -- is denied.** Confirmed it is not a mode issue (`chmod
0777` on upper/work did not help). Every rootless idmapped-overlay setup mounts
the overlay from *inside* the userns, where the mapped root owns upper/work.
**Resolution for the next pass: mount the overlay from inside the userns.** The
parent id-maps only the shared lower (detached fd, inherited by the child) and
chowns the per-container upper/work to `base`; the child -- after the userns
handshake, now the mapped root owning upper/work -- does `fsopen`/`fsconfig`
(id-mapped lower fd + plain upper/work + `userxattr`)/`fsmount`, then
move_mounts and pivots. All the building blocks (idmap userns, `idmap_bind`,
the new-mount-API wrappers, the userns handshake) already exist; this moves the
overlay build from the parent into the child and adds the chown + `userxattr`.

## Context

Issue #29 (raised 2026-08-17, still open): thinC's containers run as real host UID 0 with no `CLONE_NEWUSER` at all. ADR-0168 closed the single sharpest consequence of that (an untrimmed capability set, `CAP_SYS_MODULE` chief among them) but explicitly deferred the underlying gap: "Root UID inside a container is still real host UID 0 after this change... Full user-namespace support... remains open as issue #29's own original scope."

Discussed directly with the user 2026-08-19 (during the gcc/issue #32 bootstrap effort, as part of a wider "what else is already in the kernel we're not using" pass). The user's own explicit requirements, stated directly: used **by default**, not opt-in; **100% API-driven**; **100% transparent to the end user** (no manual UID range picking, no new required fields); **integrated with this platform's existing user/group management and LDAP**; and **resilient to LDAP being unreachable or not configured at all**.

Three real, upstream precedents inform this design, checked directly rather than invented from scratch:
- Docker/Podman's `userns-remap`: one fixed UID/GID sub-range (typically 65536 wide) remapping a container's own `0..65535` onto an unprivileged host range.
- Podman's *rootless* mode specifically: each real Linux user gets their own sub-range from `/etc/subuid`/`/etc/subgid`, keyed to their own login identity — the direct model for "keyed to identity" rather than one shared range for everything.
- FreeIPA's Subordinate IDs feature: stores sub-range assignments as real LDAP attributes (`ipaSubordinateId` auxiliary objectclass) on the owning user's own directory entry — the direct model for "integrated with LDAP," **but not directly applicable here**, checked and ruled out below.

**Why FreeIPA's own LDAP-schema approach doesn't transfer**: this platform's LDAP layer is `glauth` (`recipes/container/ldap-*`, `daemon/src/ldap.c`), not a general-purpose directory server with extensible schema. glauth is a TOML-config-driven LDAP *emulation* — `daemon/src/ldap.c`'s own `write_glauth_config()` renders thincd's own managed `struct ldap_user`/`struct ldap_group` records directly into glauth's fixed, real Go struct schema (`v2/pkg/config/config.go`'s own `User`/`Group` types, confirmed directly against glauth's real source per that file's own comments). There is no facility to attach an arbitrary new LDAP attribute the way a real directory server would — extending it would mean patching glauth's own upstream source (a real, available option, this project already builds glauth from source as a Tier-3-adjacent recipe — but not needed here, see Decision).

**Why "resilient to LDAP being down" is mostly already solved, not a new problem**: checked directly in `daemon/src/hostauth.c`'s own `hostauth_login()` — it tries a live LDAP bind first (`try_ldap_login()`), but falls back to `ldap_user_check_password()` whenever no configured server actually answers. That fallback checks the password against thincd's **own locally-persisted** `struct ldap_user` record (the same managed record `daemon/src/ldap.c` renders into glauth's config in the first place) — meaning thincd already maintains its own authoritative, always-available copy of every managed user's identity, independent of whether a live LDAP bind is currently possible. Critically, that same `struct ldap_user` record **already has a real `uidnumber` field** (confirmed: `daemon/src/ldap.c` lines 239-240, 348, 671-672) — a stable per-user integer that already exists, is already persisted locally, and is already resilient to LDAP being unreachable, entirely independent of this ADR.

## Decision

**Subordinate ID ranges are keyed to `uidnumber` on thinC's own already-existing, already-resilient `struct ldap_user` record — not to a new LDAP attribute, and not to a live LDAP bind.** No glauth schema change, no new LDAP concept, no new resilience engineering: this ADR is additive on top of identity infrastructure that already exists and is already proven to degrade gracefully.

**Allocator**: a new, small module (`daemon/src/subid.c`) owns one flat, atomically-rewritten persisted table (same convention as ADR-0012's network persistence) mapping `uidnumber → {sub_uid_base, sub_gid_base}`, one fixed range width (65536, the same convention Docker/Podman/systemd all already use — no reason to invent a different number). Allocation is first-come: the first container creation for a given `uidnumber` gets the next free range from a monotonically increasing counter; every later container for that same identity reuses the same range. This table is the **sole source of truth**, present and fully functional with zero LDAP configuration at all.

**Collision avoidance is an active, not incidental, property of the allocator** (a real gap in this ADR's own first draft, caught in review before anything was built): a naive counter starting near 0 would directly collide with real LDAP `uidnumber` values themselves (glauth deployments conventionally use small numbers, e.g. 5001/5002 in this project's own examples) — a container's own internal UID 0 mapping onto a host UID that happens to equal a *real* LDAP user's own `uidnumber` would let container-internal activity collide with that real identity's own host-side file ownership, a genuine correctness/security bug, not a cosmetic one. Fixed two ways, not one: (1) the counter's own floor is hard-coded at **100000** — the same real, standard starting offset Docker's and Podman's own `/etc/subuid` conventions already use, chosen for the same reason: comfortably above any realistic real system/LDAP UID range, with room for tens of thousands of distinct sub-range allocations before any practical exhaustion risk; (2) belt-and-suspenders, the allocator actively checks any newly-considered range against every currently-known `uidnumber` in the `struct ldap_user` table before committing it (and, symmetrically, LDAP user record creation/update checks a proposed `uidnumber` against the allocator's own already-committed range table) — a defensive check, not one relied on alone, since floor-100000 should already make a real collision practically unreachable, but "should" is not "verified."

**Identity resolution at container-creation time** (fully automatic, zero new API fields — matches "100% transparent"): the caller's identity is already resolved by host-auth for every authenticated write today (`struct hostauth_session`). `POST /v1/containers` resolves that session's own `uidnumber` (falling straight through the same already-resilient path above) and looks up-or-allocates its subordinate range, used directly as the `CLONE_NEWUSER` `uid_map`/`gid_map` base. The container's own owner is always the authenticated caller — never a new, explicit "owner" field to get wrong or leave stale.

**When host-auth is disabled entirely** (a real, existing operator choice, not hypothetical): there is no authenticated-caller concept to key anything to at all — but this ADR's first draft wrongly conflated "no identity to key by" with "no isolation needed," proposing every container share one fixed range in that case (caught in review). That's wrong: container-to-container isolation is a real, independent property from operator-to-operator isolation, and dropping it silently just because operator identity happens to be unavailable creates exactly the lateral exposure user namespaces exist to prevent (two containers both believing they're host-UID-equivalent to "their own root," sharing that same effective host UID across any shared surface — a bind-mounted volume, an accidentally-shared IPC namespace). The real fix: with no operator identity to key on, the allocator instead keys on the **container's own name** (already unique per `POST /v1/containers`, already required) — every container still gets its own genuinely distinct sub-range, unconditionally, regardless of whether host-auth is on. What's lost with host-auth off is only the *cross-container* consistency property (the same real operator's containers no longer share one recognizable range, since there's no operator concept left to make that consistency meaningful for) — never per-container isolation itself.

**"Servers... combine groups, servers, groups, users"**: resolved directly with the user — this does **not** mean inventing a new "register a physical host" concept (multi-host coordination is explicitly not scoped yet, per `MISSION.md`'s own charter: "no phase has scoped it... no single-host decision should be made in a way that forecloses it later"). It means: don't build a parallel, redundant registration mechanism — the existing `ldap server register --container=NAME` pattern (which container instance is *providing* the LDAP service) stays exactly as-is and is orthogonal to this ADR; this design doesn't need or touch it.

**By default, not opt-in**: every new container creation gains a real `uid_map`/`gid_map`, unconditionally, once this ships — no new create-time flag to remember to pass. Matches ADR-0168's own precedent for a security-posture-changing default: applies only on a container's *next* creation, an already-running container is unaffected until recreated (no silent, live re-mapping of a running process's own namespace, which isn't a real kernel capability anyway).

**Shared, read-only image lowerdir**: cannot be `chown`'d to any one operator's own range at all — it's shared across every container on that image, across every operator; permanently rewriting its ownership to match one operator's range would silently corrupt correctness for every other operator's own containers using the same image. This ADR's first draft contradicted itself here (stating the chown is impossible, then naming a "chown fallback" two paragraphs later, caught in review) — retracted outright, not softened.

Real id-mapped mounts (`mount_setattr(MOUNT_ATTR_IDMAP)`, the modern kernel mechanism) are the intended primary approach, genuinely needing empirical verification on this project's own overlayfs+ext4 stack before being trusted (this exact combination has already produced two real, non-obvious kernel surprises this project has had to root-cause the hard way — ADR-0174's workdir quota-tagging gap, and today's own page-cache/writeback investigation — "confirm directly, don't assume" applies here at least as much as anywhere else in this codebase). If it doesn't hold up, the real, non-destructive fallback follows directly from how overlayfs itself already works here, not from chown: **the lowerdir is read-only and never written to by any container** — every write already lands in that container's own upperdir (the entire basis of ADR-0004's shared-lowerdir design). Ownership on the lowerdir therefore only affects DAC *read* checks, never correctness of writes, and the overwhelming majority of a real rootfs (`/usr/bin`, `/usr/lib`, ...) is already world-readable (`o+r`/`o+rx`) — a DAC check against "other" permission bits succeeds regardless of whether the checking process's namespaced UID maps cleanly onto the file's real owner UID or not. Without id-mapped translation, the real, bounded exposure is narrower than "broken": only lowerdir content that is *not* world-readable (owner/group-restricted files) becomes unreadable to a namespaced container. That's a real, but auditable and individually fixable gap — find and re-permission the specific offending files at image-build time (`pkg_seed_image_baseline()`/individual recipes) — not a reason to abandon the shared-lowerdir model. Copying a whole per-range lowerdir per operator is a genuine last resort only, held in reserve if the audited-permissions approach turns out insufficient in practice — not the default fallback.

**Composability with what's already shipped**: ADR-0168's capability bounding-set drop and ADR-0017's `BPF_CGROUP_DEVICE` gate both stay fully in effect, unchanged — user namespaces are a new, additional layer, not a replacement for either. ADR-0168's own Context already anticipated this: "a process can hold 'full capabilities' relative to its own non-init user namespace too."

## Consequences

- New `daemon/src/subid.c`: the sub-range allocator + its own persisted table, keyed to `struct ldap_user.uidnumber` (or, when host-auth is off, to the container's own name) — floor 100000, active collision checks against known `uidnumber` values both directions, not a bare incrementing counter.
- `src/cgroup.c`/`src/container.c`/`include/container.h`: `container_spec` gains real `uid_map`/`gid_map` fields; `CLONE_NEWUSER` added to the namespace flags `container_create()` already requests; real `uid_map`/`gid_map`/`setgroups` writes via raw syscalls (`/proc/<pid>/{uid,gid}_map`, no glibc wrapper needed, matching this project's own `pivot_root`/`clone3` precedent) — real, correct write ordering (`setgroups deny` before `gid_map` write, the well-documented kernel requirement) needed, not assumed.
- `daemon/src/main.c`: container creation resolves the caller's own `uidnumber` from the existing host-auth session (or the container's own name, if host-auth is off), calls into the new allocator — zero new REST fields, matching "100% API-driven and transparent."
- Real id-mapped-mount verification needed before this ships — genuinely open, not resolved by this ADR alone. If it fails: audit and re-permission any non-world-readable lowerdir content (bounded, real work, not a reason to abandon the shared-lowerdir model) — never a chown of shared, read-only content.
- Not yet implemented — this ADR is the design; issue #29 tracks the actual build, phased like everything else in this project (matching Zen — this is not a single micro-step).
- No change to any already-shipped ADR's own guarantees: ADR-0168 and ADR-0017 remain exactly as documented, composed with, not superseded by, this one.
