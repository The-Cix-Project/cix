# 0215 — Cix boots through its own EFI boot manager, not systemd's

## Status

Accepted. Replaces sd-boot as the boot manager an installed host runs.
Does **not** supersede [ADR-0014](0014-ab-slots-with-boot-counting.md):
its counted A/B slot mechanism, its on-disk layout and its Boot Loader
Specification entry format all stay exactly as they are. Only the
program that reads them changes.

## Context

An installed Cix host boots UEFI, so something on the ESP has to choose
a kernel and start it. That has always been systemd-boot, and until now
the binary was read off whatever machine built the installer ISO —
`/usr/lib/systemd/boot/efi/systemd-bootx64.efi`, a path that exists on a
Debian development box and on no Cix control-plane root. Closing that
(issue #177) meant either packaging it or replacing it.

Packaging it was attempted, and got far enough to be measured rather
than guessed at. Building sd-boot means building part of systemd: its
`meson.build` resolves every dependency unconditionally before it looks
at which feature was requested, so a build that produces **one 174 KB
file in 34 steps** first requires

    meson  ninja  gperf  jinja2  markupsafe  pyelftools  libmount  libcap

eight packages, of which the EFI binary links **none**. They exist to
satisfy configure. That is a permanent, foreign build system in the
dependency chain of a project whose charter is a hand-written C
networking data plane rather than Open vSwitch, and whose toolchain
policy is TCC with a short, deliberate exception list.

The work also surfaced how little of sd-boot we actually use. The whole
contract is:

  - read `/loader/loader.conf` (`default cix-*`, `timeout 0`)
  - read `/loader/entries/*.conf`, each a handful of `key value` lines
    (`title`, `sort-key`, `version`, `linux`, `options`)
  - order entries, prefer the newest that still has boot attempts left
  - decrement the tries counter encoded in the filename
    (`cix-a+3.conf` → `cix-a+2-1.conf`) by renaming it
  - load the kernel named by `linux` and start it with `options` as its
    command line

Every one of those is something this project already does elsewhere in
plain C. The kernel is itself a PE32+ EFI application (the Linux EFI
stub), so starting it is `LoadImage()` + `StartImage()` with
`LoadOptions` set — not a bootloader's worth of filesystem drivers,
scripting language and module loader.

## Decision

Write `cix-boot`, a small EFI application in this repo, and install it
where sd-boot went. It implements exactly the contract above and
nothing else.

It is built the way every other Cix binary is built — from this repo, by
`cix.recipe`, alongside `cix-install` and `cix-recover` — so it needs no
package of its own, no artifact harvest and no isotools involvement.
`mkinstalleriso` receives it as an argument, the way it already receives
`cix-install` and `cix-recover`, and signs it with the Cix key exactly
as it signed sd-boot.

**Toolchain.** GNU ld links PE/COFF directly with `-m i386pep`, so no
ELF-to-PE conversion step is needed and systemd's `elf2efi.py` (and
therefore Python, and therefore pyelftools) has no equivalent here. Our
binutils is currently configured for its default target only, so it has
no PE emulation; `--enable-targets=x86_64-pep` adds one. That is the
entire toolchain change — one configure flag, replacing eight packages.

The application itself is compiled with gcc, freestanding
(`-ffreestanding -fno-stack-protector -fpic -fshort-wchar
-mno-red-zone`), against hand-written UEFI structure definitions in this
repo rather than gnu-efi.

To be accurate about what that does and does not buy: `gnu-efi` is
already a package here (3.0.18-4, built for `shim`), so this is **not**
avoiding a new dependency. What it avoids is gnu-efi's *build
machinery* — its crt0, its relocation handling and its ELF-then-
`objcopy --target=efi-app-x86_64` conversion, a target modern binutils
no longer provides, which is precisely why upstream projects have been
migrating off it. Linking PE directly with `-m i386pep` sidesteps all of
that, and once the link is direct the headers are the only thing left to
take, which is a few hundred lines of structure definitions fixed by a
published specification.

That is the same posture as `include/linux_compat.h`, which declares
`struct clone_args` itself rather than pulling in a header that fights
glibc. gcc is required regardless: TCC implements no
`__builtin_ms_va_list`, so it cannot compile MS-ABI EFI code at all
([ADR-0211](0211-tcc-msabi-and-atomics-tier3.md) proved this directly
while trying to build gnu-efi itself).

## Consequences

- The eight packages above are not needed and are removed. `libc-dev`
  keeps the `librt.a` addition made while chasing this, since that was a
  real gap in our glibc development package that would have caught
  anything linking `-lrt`.
- `binutils` is rebuilt with `--enable-targets=x86_64-pep`. Nothing else
  in the catalog is affected: adding a target does not change how the
  default one behaves.
- The ESP layout, the BLS entry format, `esp.c`'s boot-counting logic
  and `test_boot_ab.c` are all unchanged, because the format is a
  published specification and we are changing only its reader. Existing
  installed hosts keep booting, since their ESP contents are still valid
  input to the new program.
- This is per-architecture in a way sd-boot's packaging hid: an aarch64
  host needs `BOOTAA64.EFI` built from the same source with a different
  emulation. Writing it ourselves makes that a build-matrix question
  rather than a packaging one — see issue #179.
- Secure Boot is unaffected in mechanism ([ADR-0015](0015-secure-boot-shim-and-mok.md)):
  shim still chainloads a Cix-signed second stage; only the bytes being
  signed change.

## What it cost to get right

Three findings, none of which a signature check reveals, and all of
which produce identical symptoms — `Verification failed: (0x1A) Security
Violation` — while `sbsign` and `sbverify` both report the image as
correctly signed. They are recorded here because the error message says
"signature" and the causes were not:

1. **A `.sbat` section is mandatory.** shim generation 4 and later
   refuses any image without SBAT metadata. This was the actual blocker.
   The generation number in it is a revocation lever: shipping a shim
   that requires `cix-boot,2` refuses every copy still declaring 1, so
   it is incremented only for a real security fix.
2. **`ld` appends a COFF symbol table after the last section.** Nothing
   maps it, so the file is larger than the sum of its sections and
   Authenticode hashers disagree about the trailing bytes. `sbsign`
   warns (`data remaining[9216 vs 11422]: gaps between PE/COFF
   sections?`) and `-s` on the link removes it.
3. **`ld` computes `SizeOfImage` from file-backed sections only.** A
   large `.bss` — 55 KB of static arrays here — produced a header
   claiming `SizeOfImage 0x13000` while sections extended past
   `0x22000`. Taking that memory from the firmware pool instead leaves
   `.bss` at 16 bytes and the header describes the file.

Verified end to end: `test_installer` installs to a blank disk and boots
the installed system under real Secure Boot with the Cix key enrolled as
a MOK, through shim, `cix-boot` and the kernel, twice.

## Alternatives considered

**Package sd-boot.** Rejected on the eight-package chain above. It works
— the local prototype configured and built `systemd-bootx64.efi`
successfully — so this is a proportionality judgement, not a technical
block.

**Use GRUB.** Already packaged and harvested for the installer media, so
it would add no new dependencies. Rejected on two counts: it is far
larger and more complex than the job needs, and it has no equivalent of
Boot Loader Specification boot counting, so ADR-0014's A/B assessment
would have to be rebuilt in GRUB's own scripting — replacing a small C
program we would write once with a scripting layer we would maintain
forever.
