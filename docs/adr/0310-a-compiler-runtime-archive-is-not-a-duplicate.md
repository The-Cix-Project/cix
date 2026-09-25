# 0310 — a compiler runtime archive is not a duplicate of its shared counterpart

## Status

Accepted. Amends clause 2 of [ADR-0251](0251-a-package-artifact-carries-what-the-platform-runs.md), which stands in every other respect: static archives are still dropped where a shared library supersedes them, and that clause still accounts for 54% of what the policy removes.

## Context

ADR-0251's clause 2 drops a static archive when a shared object of the same stem ships beside it. The reasoning is that on a platform which links dynamically always, `libfoo.a` next to `libfoo.so` is the same code twice.

That is true of an ordinary library and false of the compiler's own runtime. `gcc -static-libstdc++` and `gcc -static-libgcc` exist so a binary can carry the C++ and GCC runtimes and then need only glibc wherever it runs — which is exactly what a package built on one host and installed on another wants. Those flags resolve `libstdc++.a` and `libgcc.a` specifically. Removing them does not make the artifact smaller in any useful sense; it removes a linking mode.

**The asymmetry is what exposed it.** `-static-libgcc` worked and `-static-libstdc++` did not, from the same flag pair with the same intent, because `libgcc.a` has no `libgcc.so` beside it — the shared one is `libgcc_s.so` — while `libstdc++.a` has `libstdc++.so`. A filename decided which half of one feature survived.

Measured on 192.168.15.95, 2026-09-24 (#521). `node`'s `--partly-static`, which passes exactly that pair, died in 40 seconds:

```
/usr/libexec/gcc/x86_64-pc-linux-gnu/16.2.0/ld: cannot find -lstdc++: No such file or directory
collect2: error: ld returned 1 exit status
```

`GET /v1/pkg/gcc@cpdl-stage` (16.2.0-17, built under the policy) lists no `libstdc++.a`; `gcc@cix-builder` (16.2.0-13, an artifact predating it) does. `libstdc++exp.a` and `libstdc++fs.a` are the control — same package, same directory, same build, kept only because nothing shares their stem.

node had compiled for hours against the older gcc before this surfaced. That is #328's shape again: the policy runs when a package is BUILT, and the artifact tier installs approved bytes rather than rebuilding, so a pre-policy artifact keeps masking a policy defect until something rebuilds.

## Decision

**A compiler runtime archive is kept, even when a shared object of the same stem ships beside it.** The set is named in `daemon/policy/pkg-finalize.sh`: `libstdc++`, `libsupc++`, `libgcc`, `libgcc_eh`, `libgcc_s`, `libatomic`, `libgomp`, `libitm`, `libquadmath`, `libssp`, `libobjc`.

Named rather than derived, deliberately. There is no property of the file that distinguishes a toolchain's own runtime from any other library shipping both forms — both are `!<arch>` archives beside an ELF shared object, in the same directory, with the same stem. A rule that tried to infer it would be guessing, and a wrong guess here is silent: the artifact installs, and the failure appears in some other package's link months later.

`libtcc1.a` needs no entry. It has no shared counterpart, so clause 2 never reached it, which is also why this defect was invisible for as long as TCC was the only toolchain that mattered.

## Consequences

- `-static-libstdc++` works against a gcc this platform built. It did not, for any gcc built after ADR-0251 landed.
- Artifacts grow by the size of the kept archives. gcc's `libstdc++.a` is the significant one; the rest of the set is small or absent from most packages.
- `node@24.21.0-2` does not need this. It was shipped with `--partly-static` dropped and `gcc` declared as a runtime requirement, on the owner's decision of 2026-09-24, before this ADR existed. Whether to restore the flag — trading a runtime dependency on gcc for a larger binary — is a packaging question for that recipe and is not decided here.
- `test_pkg_finalize` gates it: `libstdc++.a` staged beside `libstdc++.so.6` must survive, with `libgcc.a` as the control that was always kept by accident of naming.

## Alternatives considered

**Keep every archive.** Clause 2 removes real weight — ADR-0251 measured static archives at 54% of an artifact — and a package's own `libfoo.a` beside its `libfoo.so` genuinely is dead weight here. Rejected: this defect is narrow and the clause is right about everything else.

**Let a recipe opt out per path.** More general, and it puts the decision where the knowledge is. Rejected for now: it adds recipe surface for a set that has eleven members and changes only when a toolchain does, and every recipe that needed it would be expressing the same fact.

**Leave it and route around it per package**, as `node` did. That is what happened, and it is why this ADR exists: the next package wanting a static compiler runtime would have hit the same wall, and the reason would have been just as invisible.
