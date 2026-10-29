# 0363 — The win64 cross target is part of the tcc package

## Status

Accepted by the owner on 2026-10-29, for #656. It supersedes decision 1 of [ADR-0362](0362-cix-boot-is-built-by-a-tcc-win64-cross-compiler.md), the separate `tcc-win64` package. ADR-0362's other decisions stand: cix-boot.efi is built by tinycc's x86_64-win32 target, with the `.sbat` section class, `memset`/`memcpy` in cix-boot.c, an empty `EFIAPI`, and the boot tests as the gate.

## Context

ADR-0362 shipped the win64 cross compiler as `tcc-win64@0.9.28rc-1`: a second recipe pinning the same tinycc commit (2ba12e83, the same url and sha256) as `tcc@0.9.28rc-31`, with one edit of its own. The owner asked whether the two should be one. Read from the two recipes:

- **The same source was pinned twice.** A version bump had to change both, and the first one missed would leave the two compilers built from different trees with nothing to say so.
- **The cross compiler lacked tcc's shared-code edits.** tcc carries 18 source edits across #122, #208, #211, #212, #216, #219, #220, #227, #228 and #235. Several are in code every target compiles: the byte-swap builtins in `include/tccdefs.h`, the C11 version in `libtcc.c`, the atomics define. The compiler that built cix-boot.efi had none of them, so it was not this project's tcc.
- **Upstream builds its cross compilers from the same tree**, with `make cross` or `make <target>-tcc`.

Compile probe 0.2.57-652 (192.168.15.95, 2026-10-29) booted the cix-boot.efi `tcc-win64` built, through all six boot tests (`test_boot`, `test_boot_ab`, `test_boot_update`, `test_console_shell`, `test_console_pki_bootstrap`, `test_installer`). So the target works, and what is left is where it is built.

## Decision

1. **`tcc` builds and installs both targets** from one tree with every edit: `/usr/bin/tcc` (native) and `/usr/bin/x86_64-win32-tcc`. The cross compiler is built after the three-stage bootstrap, by the stage-3 compiler.
2. **The `.sbat` section class is one of tcc's edits.** Its gate moves into tcc's recipe: link a `.sbat` with the cross compiler and extract that section alone.
3. **The native compiler is gated unchanged.** The `.sbat` edit is in `tccpe.c`, which a native build does not compile. So stage 3's `tcc` must equal 0.9.28rc-31's published `/usr/bin/tcc` once both are stripped the same way, and the build fails if it does not. Changing the package must not change the compiler every other package is built with.
4. **`tcc-win64` is retired.** The cix recipe declares `tool "tcc"`, which carries both compilers, and `tcc-win64` is uninstalled.

## Alternatives considered

- **Keep `tcc-win64` and copy tcc's edits into it.** That is the parallel implementation the maxims forbid: two edit lists for one compiler, kept in step by hand.
- **Build the cross target only when cix needs it.** Then the package's contents would depend on its consumer, with no gain: the cross binary is about 400 KB.

## Consequences

- A change for the win64 target rebuilds the native compiler. Decision 3's gate makes that safe for the native target, as measured, not assumed. The day an edit touches shared code, the gate fails, and it should: that is a native compiler change, made on purpose.
- `cix-builder`, which holds `tcc` as rolling, carries `x86_64-win32-tcc` too. Nothing already installed is rebuilt.
- cix-boot.efi is built by the same compiler, with the same edits, as everything else Cix builds.
