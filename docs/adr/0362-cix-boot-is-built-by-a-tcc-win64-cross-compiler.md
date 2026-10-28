# 0362 — cix-boot.efi is built by a TCC win64 cross compiler

## Status

Proposed, 2026-10-28, for #656. It carries out the owner's decision of 2026-10-27 (#628), *"Make it TCC too"*, which ended the gcc exception for the UEFI boot manager ([ADR-0215](0215-our-own-efi-boot-manager.md)). It corrects the conclusion [ADR-0211](0211-tcc-msabi-and-atomics-tier3.md) drew for this program. That ADR's finding about `__builtin_ms_va_list` under the native tcc stays true.

## Context

`cix-boot.efi` is the one program in the cix tree built with gcc: `EFI_CC ?= /usr/bin/gcc`, linked to PE/COFF by `ld -m i386pep --subsystem=10 -e efi_main --image-base=0x10000 -s`. A UEFI application needs the Microsoft x64 calling convention. The installed `tcc@0.9.28rc-31` is native only: its file list is `usr/bin/tcc` and `usr/lib/tcc/libtcc1.a`.

tinycc also has an `x86_64-win32` target. Its default ABI is the Microsoft one, and it has its own PE linker. Five probes measured it on 192.168.15.95 on 2026-10-28 (`probe-tccefi@1-1` to `@5-1`). Each used the tinycc source `tcc@0.9.28rc-31` pins (commit 2ba12e83, the same url and sha256) and `cix-boot.c` at cix `f362298b`:

- **The cross compiler builds** with the installed tcc: `configure --source-path=. --prefix=/usr --cc=tcc`, then `make x86_64-win32-tcc`. It reports `tcc version 0.9.28rc (x86_64 Windows)`.
- **`cix-boot.c` compiles unchanged and is clean under `-Wall -Werror`.** `ms_abi` occurs nowhere in `tcctok.h` or `tccgen.c`, so tcc ignores `EFIAPI`'s `__attribute__((ms_abi))` without a warning. On this target the default convention is already the right one.
- **The link needs `memset` and `memcpy`.** tcc emits calls to them for zero-initialised locals (`unresolved reference to 'memset'`). A freestanding program provides them: gcc documents the same requirement of its own freestanding output, and its build of this file happened never to emit the calls.
- **The image** from `-nostdlib -Wl,-subsystem=efiapp -Wl,-image-base=0x10000 -Wl,-entry=efi_main -Wl,-dynamicbase -Wl,-nxcompat` is a PE32+ EFI application (`Subsystem 0x0a`):
  - `relocations stripped` is clear and DllCharacteristics is `0x140`. `.reloc` is empty, because the code has no absolute relocation.
  - There is no trailing COFF symbol table. tccpe writes one only for debug output, so the `ld -s` problem (shim's `0x1A Security Violation`) cannot arise.
- **`.sbat` is the one gap.** `pe_section_class()` gives a class of its own only to `.rsrc`, `.iedat`, `.pdata` and `.reloc`. Every other read-only allocated section is `sec_rdata`, and consecutive sections of one class are merged. So `__attribute__((section(".sbat")))` lands inside `.rdata`, and shim refuses an image with no `.sbat`.

  Probe 5 measured two ways to give the image one:
  - **A `sec_sbat` class in `tccpe.c`:** one enum member, and one `strcmp(name, ".sbat")` returning it. `.sbat` is then its own section at 0x17000, after `.pdata`, and `objdump -s -j .sbat` shows the two CSV lines verbatim. `sbsign` signs it without reporting a gap between sections, and `sbverify --cert` answers `Signature verification OK`.
  - **`objcopy --add-section .sbat=… --set-section-alignment .sbat=512`** on the unpatched image, the route shim's SBAT documentation offers. objcopy puts `.sbat` first, at VMA `0x100000000`, which is outside the image, and moves every other section's file offset.

## Decision

1. **A `tcc-win64` package builds tinycc's `x86_64-win32` cross compiler** from the source `tcc` pins, with the installed tcc. It installs as its own binary. The installed native compiler is not rebuilt and is not changed.
2. **That package carries one source edit: a `.sbat` section class in `tccpe.c`**, gated by a recipe check that links a file with a `.sbat` section and reads it back as its own section. Patching tinycc's source is what the `tcc` recipe already does (#208, #212, #219, #235). The edit is offered upstream, and is not tracked as debt here.
3. **`cix-boot.c` defines `memset` and `memcpy`**, as a freestanding program must.
4. **`EFIAPI` is empty under that compiler**, because its default convention is the UEFI one. The `uefi.h` comment that says TCC cannot compile this file is corrected to say so.
5. **The Makefile builds `cix-boot.efi` in one step** with `x86_64-win32-tcc -nostdlib -Wl,-subsystem=efiapp -Wl,-image-base=0x10000 -Wl,-entry=efi_main -Wl,-dynamicbase -Wl,-nxcompat`. `EFI_LD` and `ld` go. The cix recipe declares `tcc-win64` as a build tool.
6. **`BOOT_SELFTESTS` is the gate.** The switch ships in a cix release whose compile probe boots the result in QEMU, which is the first time a tcc-built image is executed.

## Alternatives considered

- **objcopy after the link.** It is rejected on probe 5's measurement: the section objcopy adds sits outside the image and moves every file offset. Making that right needs further address options in a second tool rewriting the headers of an image the compiler had already laid out. Unaccounted trailing bytes are exactly what shim refused before (`ld -s`).
- **Keeping gcc.** The owner rejected it on 2026-10-27 (#628).
- **Writing `.sbat` some other way from C**, such as a section type tccpe happens to class as `sec_other`. That depends on a classification nobody declared. Rejected.

## Consequences

- No gcc and no `ld` remain in Cix's own code. The README's "compiled exclusively with the Tiny C Compiler" becomes true, and `building-cix.md`, the Makefile comment and `include/uefi.h` are corrected in the same change.
- The image layout changes: different sections, `.pdata` appears, `.reloc` is empty. `BOOT_SELFTESTS` boots it under OVMF in QEMU in six tests, the A/B and update paths included, and until they pass on a compile probe nothing ships. **Whether shim accepts it under Secure Boot is not tested by them**: `test_installer`'s Secure Boot sessions need a Secure Boot OVMF build (#647). Before #647, that is verified on a real Secure Boot install. `sbsign` and `sbverify` agreeing is evidence for it, and it is not proof, which is the lesson of `ld -s`.
- If gcc ever builds `cix-boot.c` again with optimisation, its `memset` loop can be turned back into a call to `memset` unless `-fno-builtin` is passed. That is a known property of freestanding gcc builds, recorded once in the source.
- [ADR-0211](0211-tcc-msabi-and-atomics-tier3.md) gets a dated factual-correction note: its finding about gnu-efi stands, and its conclusion for an EFI application built by Cix does not.
