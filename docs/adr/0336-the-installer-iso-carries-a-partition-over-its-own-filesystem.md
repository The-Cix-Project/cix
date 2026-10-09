# 0336 — The installer ISO carries a partition over its own filesystem

## Status

Accepted, 2026-10-09. Issue [#430](https://git.home.arpa/itdlabs/cix/issues/430). Supersedes nothing; follows [#429](https://git.home.arpa/itdlabs/cix/issues/429)'s per-media GRUB entries and amends the argument contract of [ADR-0064](0064-rest-driven-iso-assembly.md)'s REST-driven ISO assembly.

## Context

The installer media has no initramfs, by the same choice every other root filesystem on this platform makes. So nothing goes looking for the installer's own filesystem at boot: the kernel is told which device holds it, on the command line, and `mkinstalleriso` writes that line.

Until now the USB entries said `root=/dev/sda`. That is a name the kernel assigns by enumeration order, and it is a poor one here for a reason specific to this medium: an installer's target machine usually has a disk of its own, and it is just as likely to be `sda` as the stick is. The entry that exists for bare-metal installs was therefore a coin flip on exactly the hardware it exists for.

`root=PARTUUID=` is the fix, and the kernel's own `root=` parser already understands it. It needs two things the media did not have:

1. a GPT partition that covers the ISO9660 filesystem, and
2. a PARTUUID for it that is known when `grub.cfg` is written — which is before the image exists, since `grub.cfg` is ISO content.

#430 recorded, measured on two published ISOs, that the media had neither: its GPT held `Gap0` / `EFI boot partition` / `Gap1`, none of which covers the filesystem, and xorriso generated a fresh random disk GUID on every build.

### What the media's shape actually is, and why

Measured on 192.168.15.95 across six probes on 2026-10-07 and 2026-10-09, all recorded in #430. The findings that decide this ADR:

- **`grub-mkrescue` embeds the ESP inside the ISO filesystem.** It creates `efi.img` *within* the staged tree that becomes the ISO (`grub-mkrescue.c:857`) and pushes `--efi-boot efi.img -efi-boot-part --efi-boot-image` (`:865-868`), which asks xorriso to describe that in-filesystem copy as the EFI system partition.
- **So a partition spanning the filesystem would contain the ESP's blocks, and GPT partitions do not overlap.** xorriso describes the same space as three non-overlapping entries instead. The published `Gap0`/ESP/`Gap1` layout is a consequence, not an oversight.
- **Asking for the overlapping layout anyway is refused, not ignored.** `-boot_image any part_like_isohybrid=on` fails the build: *"libisofs: FAILURE : Image write error / Caused by: Overlapping MBR partition entries requested"*. This is the measurement that closed off every flag-only route.
- **`partition_offset=16` is necessary but not sufficient.** It writes the partition-relative volume descriptor — byte 65537 reads `CD001` — which is what lets a partition starting at LBA 64 be mounted as iso9660 from its own first block. It does not touch the GPT.
- **grub-mkrescue has no switch for any of this.** The three EFI pushes are gated only on an EFI platform being present, under none of the `system_area` guards at `:634`, `:744` and `:898`.

An earlier note in #430 claimed grub-mkrescue already passed `partition_offset=16`. It does not; that line was a wrapper's own echoed injection, read as its input. A grep of `grub-mkrescue.c` finds `partition_offset`, `append_partition` and `appended_part` nowhere.

## Decision

**The installer ISO's EFI system partition is appended past the end of its filesystem, so the filesystem gets a GPT partition of its own, and the USB boot entries name it by PARTUUID.**

Seven clauses.

1. **`cix-xorriso` removes `-efi-boot-part --efi-boot-image` and appends the same image instead**, as `-append_partition 2 0xef <path>`. `--efi-boot efi.img` **stays**, so the El Torito catalogue still points at the copy inside the filesystem. grub-mkrescue keeps that copy deliberately — its own comment at `:870` says *"so that we have a duplicate on the ISO 9660 file system"* — so there is always a file to append.

2. **The optical boot path is not changed, and the CD/DVD entries keep `root=/dev/sr0`.** An optical drive is not enumerated among the disks, so the ambiguity PARTUUID solves does not arise there; and whether the kernel creates partition devices for `sr*` at all was not measured, so the path that works is left exactly as it is rather than changed on an assumption. Collapsing the four menu entries into two is what that measurement would buy, and it is not claimed here.

3. **Three native settings are prepended, before `-as mkisofs`**: `partition_offset=16`, `gpt_disk_guid=`, `appended_part_as=gpt`. They are parameters of xorriso's *native* `-boot_image` command rather than options of the mkisofs emulation, and `-as mkisofs` consumes its own argument list to the end. `-append_partition` is an emulation option and so goes at the tail. This ordering is measured, not inferred: `xorriso -as mkisofs -help` prints nothing at all, while `xorriso -help` lists both settings inside `-boot_image`'s own parameter list.

4. **The GPT disk GUID is derived from the release, by cixd, and passed to mkinstalleriso as an argument.** sha256 of `CIX_BUILD_VERSION`, laid out as 8-4-4-4-12 text. Three alternatives were considered and rejected:
   - a **fixed constant** gives every Cix installer ever built the same PARTUUID, so two sticks of different releases in one machine are indistinguishable to the kernel — the same class of bug as the `/dev/sda` hardcode, moved rather than fixed;
   - **reading the GUID back out of the finished image** is circular, because `grub.cfg` is already sealed inside the image whose table would be read;
   - **discovering it at boot from GRUB** (`probe --part-uuid`) needs no determinism at all, but on EFI media `$root` is the ESP, so naming the sibling partition needs string work GRUB has no primitives for, and nothing short of a real boot tests it.

   It is derived in cixd rather than in mkinstalleriso because that is where the release string lives and because cixd already links libcrypto, which keeps a control-plane host tool free of a dependency it would need for one hash. mkinstalleriso then uses the one value twice — exported to `cix-xorriso`, and turned into the PARTUUID it writes — so the two cannot disagree.

5. **The fourth group's low byte is zero, and that is load-bearing.** xorriso derives partition *n*'s unique GUID from the disk GUID by raising exactly that byte by *n* — measured three times, across three different layouts, and cross-checking against the published ISO whose three entries differ from its disk GUID "only in one nibble". Zeroing it makes partition 1's suffix `01` and makes a carry into the neighbouring byte impossible for any partition count this medium could reach. No RFC 4122 version or variant bits are set: a GPT GUID is sixteen opaque bytes, and stamping this one as version 4 ("random") or 5 ("name-based, SHA-1") would be a false claim about how it was made.

6. **`mkinstalleriso` re-reads the finished image and refuses to call it built unless both partitions are really there** — partition 1 named `ISO9660`, starting at LBA 64, with the PARTUUID `grub.cfg` names and the partition-relative descriptor at its first block; and partition 2 carrying the EFI system partition type GUID. Both, because they answer different questions and have the same cause: partition 1 is how the *kernel* reaches its root, partition 2 is how *firmware* reaches a bootloader, and a medium can fail either way. Every input to those derivations belongs to a tool this project does not own, so each is checked against the image rather than trusted. The gate is fail-safe in the direction that matters: a mistake in its own GUID formatting makes it refuse a correct ISO, never accept a broken one. The failure it exists to prevent is the worst shape available — an ISO that builds, signs, publishes and then cannot boot or cannot find its own root filesystem, discovered by someone standing at a machine with a USB stick.

   **The file to append is found by asking which argument directory contains it, and two matches is a refusal.** `--efi-boot`'s value is relative to the ISO's root and the directory holding it is grub-mkrescue's own temporary tree, whose name changes every run — so it is located by testing the argv's directory arguments rather than by matching a path shape. If more than one contains that name, `cix-xorriso` stops. The bytes that must be appended are the ones `--efi-boot` names *to grub*; a different file sharing the relative name would give the medium an ESP that is not the one its El Torito catalogue points at. Today only grub's tree carries an `efi.img` — `mkinstalleriso` creates none — which is precisely why the ambiguity is refused rather than settled by an ordering rule nobody would revisit when a second one appeared.

7. **`cix-xorriso` passes grub-mkrescue's capability probe through untouched, and keys that on `-o` being absent.** grub-mkrescue runs the xorriso it was given twice: `check_xorriso()` first forks `xorriso -as mkisofs -help` through a pipe and greps the output for `graft-points`, then the real image write follows. A wrapper that rewrites or refuses the first call fails the check, and its complaint goes into grub's pipe where nothing can see it — the result is `error: xorriso not found` for a binary that is present and executable. Measured: that is how `probe-isogrub@8-1` died in 5 ms, and how three earlier revisions of that probe died with the cause unknown at the time.

   The discriminator is `-o` rather than the absence of the EFI arguments, deliberately. Keying on those would mean that if grub-mkrescue ever stopped pushing them, a *real* image write would be passed through unmodified — producing media with no `ISO9660` partition while `grub.cfg` still named its PARTUUID, an unbootable ISO from a silent success. With `-o` as the test, such an invocation reaches the rewrite and fails loudly instead.

   **What is measured here and what is not, since the two differ.** That the probe argv carries no `--efi-boot` is measured: a wrapper keyed on it passed the probe through and the build proceeded. That it carries no `-o` is *not* measured — the probe's output goes into grub's pipe, so no log of this project's could show its argument list, and `-o`'s absence is read from grub-mkrescue's documented description of the call (`xorriso -as mkisofs -help`) rather than from the call itself. If that is wrong, the first real ISO build refuses with the clause's own message naming the missing `--efi-boot`, which is a loud failure and not bad media — so it is resolved by the next build either way, and no speculative second condition is added here to cover it.

## Consequences

The media's GPT becomes, measured through grub-mkrescue's real argument list:

| # | name | type | first LBA | last LBA |
|---|---|---|---|---|
| 1 | `ISO9660` | basic data | 64 | end of filesystem |
| 2 | `Appended2` | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` | after it | + 5760 sectors |
| 3 | `Gap1` | basic data | after that | end of image |

A conformant GPT rather than an isohybrid one: nothing overlaps, and the ESP carries the real EFI system partition type GUID. Byte 65537 reads `CD001`.

**The ISO grows by the size of the ESP**, about 2.9 MiB, because the appended copy is a second copy of a file the filesystem still contains. That duplicate is grub-mkrescue's own, kept on purpose, and removing it would cost the optical boot path.

**This deviates from the GPT specification in no way, which is worth stating because the obvious alternative does.** The isohybrid layout — one partition spanning the image with the ESP inside it — is what most hybrid ISOs ship and what libisofs refused to write here. Appending the ESP was chosen over asking xorriso harder for the overlapping form, and the refusal is why.

**What is not established, and is not establishable here: whether an appended ESP boots on real firmware.** Every UEFI machine finds its ESP through the GPT, which this now provides correctly, and the El Torito catalogue is unchanged — but "should" is not "does". #430's verification line has always been a real USB boot on the machine from #429, and that remains the gate. Nothing in this ADR claims the media boots; it claims the media carries the structure a correct `root=PARTUUID=` needs, and that claim is verified by `mkinstalleriso`'s own gate on every build.

**A new argument on the mkinstalleriso contract.** `gpt-disk-guid`, added to the one list in `include/mkinstalleriso_args.h` so both ends move together — the mechanism #480 built after that count went wrong twice, each time surfacing as `POST /v1/system/iso` answering with a fragment of usage text.
