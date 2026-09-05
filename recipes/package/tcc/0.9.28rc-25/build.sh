#
# tcc -- the Tiny C Compiler, this platform's own compiler for
# everything it builds.
#
# UPGRADED FROM THE 0.9.27 RELEASE TO A PINNED UPSTREAM SNAPSHOT.
#
# Why, and why now: issue #216. The 0.9.27 release (December 2017)
# gives two simultaneously-live locals the SAME stack slot when a case
# label of the enclosing switch sits inside the block that declares
# them. That miscompiled perl -- regexec.c:8131/8132 declare `SV *ret`
# and `REGEXP *re_sv`, both landed at -0x820(%rbp), and `re_sv = NULL`
# wiped the `ret` that had just been assigned, so miniperl segfaulted
# on every build. Root-caused from the real binary, reduced to thirty
# lines, and reproduced on this platform's own compiler; the full trail
# is in issue #216 and probe-tcc-conformance/7.
#
# It matters far beyond perl. The other TCC issues fail loudly -- a
# missing builtin is an undefined symbol, an unsupported flag is an
# error. This one hands two live variables the same memory, so wherever
# the colliding pair is only read and written rather than dereferenced,
# it corrupts values and exits 0. That is the shape of #122 (our tar
# listing one archive member and reporting success) and of the
# squashfs-tools endianness bug (every on-disk field byte-swapped,
# clean exit). Every package this compiler has ever built was in scope.
#
# THE SEARCH FOR A PATCH CAME BACK EMPTY, AND THAT IS THE ANSWER.
#
# Debian carries exactly four patches for tcc -- an i386 test, the
# stack protector in the runtime library, dead code in
# prepare_dynamic_rel, and implicit-int -- and not one touches codegen.
# There has been no tcc release since 0.9.27; 0.9.28 has been a release
# candidate for years. Debian does not patch around any of this: it
# pins a git snapshot (0.9.27+git20200814.62c30a4a). Upstream
# development continues on `mob`, where tccgen.c now has a real scope
# structure the 2017 release lacks -- cur_scope/prev_scope with
# vla.locorig, cleanup lists, pending_gotos -- i.e. a rewrite of
# precisely the area at fault.
#
# So the choice was: maintain a codegen patch by hand, forever, against
# a compiler nobody releases -- or pin a snapshot, as Debian does.
#
# MEASURED, NOT ASSUMED (probe-tcc-conformance/8, /9 and /10, all run
# on a real Cix host with Cix's own toolchain):
#
#   #216 stack-slot aliasing   FAIL on 0.9.27   ->  ok
#   #212 C11                   nothing          ->  stdatomic.h,
#                                                   stdalign.h,
#                                                   stdnoreturn.h,
#                                                   _Atomic,
#                                                   _Static_assert
#   #209 -pthread              FAILS            ->  accepted
#   #208 builtins              15/15 missing    ->  12/15 present
#   #211 compound literals     ok (our patch)   ->  ok natively
#
# and it BUILDS UNDER CIX'S OWN TCC, so the upgrade costs nothing in
# self-hosting: this compiler is seeded by the previous one exactly as
# before, and the three-stage bootstrap below is unchanged.
#
# WHAT THIS RECIPE STOPPED CARRYING, AND WHY EACH ONE IS SAFE TO DROP
#
# 0.9.27-14 carried six local changes. Probe 10 measured upstream mob
# for each property they provide, rather than dropping them hopefully:
#
#   lib/atomic.c + libtcc1.a  DROPPED. mob ships stdatomic.o and
#                             atomic.o itself, and a real C11 atomic
#                             program compiles and RUNS correctly.
#   dso_handle.o              DROPPED. mob ships dsohandle.o;
#                             __dso_handle resolves with no help.
#   bcheck.c malloc hooks     DROPPED. mob builds clean against this
#                             glibc with no such edit.
#   #122 do-while fix         DROPPED. mob evaluates the condition and
#                             runs the loop three times, unpatched.
#   #211 compound literals    DROPPED. mob handles them natively.
#   __has_include (-9)        DROPPED. mob implements it, and gets all
#                             three answers right.
#
# The one that stays:
#
#   __ATOMIC_* predefines     KEPT. mob does NOT predefine them, even
#                             though it supports C11 atomics --
#                             confirmed by probe 10, where the mob
#                             column fails an #ifndef __ATOMIC_SEQ_CST
#                             check outright. Code writes
#                             __ATOMIC_SEQ_CST without including
#                             anything, so no header can supply it.
#
# EVERY GATE IS KEPT, INCLUDING THE ONES FOR BUGS THIS COMPILER NO
# LONGER HAS. A gate is cheap and its whole value is catching a
# regression nobody predicted; removing the #122 and #211 checks
# because upstream fixed them would throw away the only evidence that
# they STAY fixed across future snapshots. A new gate for #216 joins
# them.
#
# ONE MIGRATION HAZARD, RECORDED RATHER THAN DISCOVERED LATER
#
# Inline linkage reverses direction on this upgrade:
#
#     0.9.27   'inline' -> LINK FAILED ; 'extern inline' -> ok
#     mob      'inline' -> ok          ; 'extern inline' -> LINK FAILED
#
# Bare `inline` working is exactly what gnulib needs (_GL_INLINE
# expands to it), and mob is arguably the more correct of the two under
# C99. But any recipe relying on `extern inline` will break.
# `static inline` works on both and is the spelling to prefer.
# CLAUDE.md carries this warning too.
#
# PINNED TO A COMMIT, NOT A BRANCH. `mob` moves; a moving source is not
# reproducible. The checksum was verified against two independent
# fetches of this exact commit.
#
pkg_name="tcc"
pkg_version="0.9.28rc-25"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_depends=""
#
# Unchanged from 0.9.27-14. tcc is its own build dependency: the
# three-stage bootstrap below needs a working compiler to produce
# stage 1, which is the point.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils"
pkg_changelog="0.9.28rc-25: every gate reports into the build log, once and for all. rc-24 set out to make the stdatomic gate say what it saw and did not achieve it: its diagnostics are still written to stderr, and the build log carries stdout, so rc-24 failed with exit status 121 and produced the identical 2772-byte log rc-23 did, ending at the line before. This is the third revision to fix this same class one gate at a time, after rc-13 for the atomic gate and rc-21 for the 219 gate, and there are 57 stderr redirections in this recipe, so fixing them individually is how a fourth one gets missed. pkg_build now redirects its own stderr into stdout at the top, which covers every gate written so far and every gate written later. Explicit capture into a file is unaffected, since that is a redirection of its own. No change to any build step or patch. 0.9.28rc-24: make the stdatomic gate say what it saw. rc-23 died there with exit 121 and printed nothing at all -- its whole log is 2772 bytes ending at the line before -- because the compile is unguarded, so under set -e the recipe aborts with the compilers status and no diagnosis is possible. This is the same defect rc-21 fixed for the 219 gate one gate earlier, and it is worth stating plainly: an unguarded command in a gate is not a gate, it is a silent exit. The compile and the run are now separate, each captures its own status, and a failure dumps the compiler stderr, the preprocessed availability of the header and whether the macro says C11 -- so one build distinguishes a missing header from a broken atomic runtime from a compiler that crashed. No change to the 219 work rc-20 introduced. 0.9.28rc-23: rc-21 and rc-22 both died at their own changelog line before running a single build step -- rc-21 introduced backticks, and rc-22 only replaced the newest entry while the accumulated older text still carried them. A recipe is sourced, so its metadata prose is executable text: backticks are command substitution and a double quote closes the assignment. Neither belongs in it, and the whole accumulated string is now free of both rather than just the part last edited. 0.9.28rc-22: rc-21 never ran -- its own changelog used backticks, which are command substitution inside a double-quoted assignment, so the recipe died with a shell syntax error at that line before reaching any build step. Same class as rc-18, whose changelog contained literal double quotes. Metadata prose in a sourced recipe is executable text: it carries no backticks and no double quotes, full stop. This revision is rc-21 with clean prose, so the #219 gate can finally report what it saw: rc-20 failed the ordering check for real, and its failure branch read the exit code through the negation rather than the command, printing 0 and diagnosing nothing. 0.9.28rc-21: make the #219 gate say what it saw. rc-20 failed it and reported the exit code as 0, because the exit code read inside if ! cmd is the status of the negation rather than of the command -- so a real failure printed a success code and no diagnosis was possible. The code is captured into a variable first, and the failure branch now dumps the init_array sections in both the object and the linked binary plus the relocations, so one build says whether the merge sorted, merged, or did neither. 0.9.28rc-20: constructor/destructor priority parses AND orders (#219). Two things were wrong and only fixing both helps: the attribute parser never read the optional priority, so the ( after it was a syntax error; and emitting the sections GCC emits is not enough, which is what made three earlier attempts fail. GCC relies on GNU ld to sort .init_array.NNNNN by name and merge them into .init_array, and tcc has neither half -- no name sorting anywhere in its linker, and both __init_array_start/_end and DT_INIT_ARRAY describe only the section literally named .init_array, so a prioritised constructor sat outside the range anything iterates and did not run at all. The merge is supplied here and runs before the array bounds are computed: prioritised entries first in ascending order, then the unprioritised ones, rebuilt as one relocation list because every slot is an identical PTR_SIZE of zeroes and all the identity is in the relocation. .fini_array needs no reversal, since glibc walks it backwards. Written to its own included file rather than threaded through sed -- sixty lines of C inserted a line at a time is how a patch becomes unreviewable. Gated by assertion rather than report at both the first build and stage 3. 0.9.28rc-19: rc-18 was unbuildable and this is the identical change with the defect removed. Its pkg_changelog contained a pair of literal double quotes inside the double-quoted assignment, which closed the string early -- the rest of the entry became shell words and the build container ran them, reporting recipe.sh line 122 ... File name too long and exiting 126. bash -n does not catch it, because the result is still valid shell, just different commands. Metadata values in a recipe carry a lot of prose, so the rule is simply that none of them contains a double quote. 0.9.28rc-18: every .eh_frame FDE claimed to describe code at .text+0 (#227). tcc_debug_frame_end() emits the FDE initial-location relocation and then writes func_ind into the slot it covers -- REL semantics, where the in-place value is the addend. i386 and ARM are REL and were always right. x86-64 is RELA, where the in-place value is discarded: tccelf.c computes tgt = st_value + r_addend and x86_64-link.c overwrites the slot with val - addr, so the addend of 0 that dwarf_reloc() passes is what the linker actually reads and every FDE resolves to the base of .text. Read out of the pinned source, and it matches the reported readelf exactly: eight FDEs, correct lengths, identical starts. The dangerous part is that tcc links its own output fine, because nothing consumes .eh_frame unless an exception is thrown -- the library builds, installs, checksums and publishes clean, and GNU ld then rejects it with an overlapping-FDEs error when a gcc-built package links against it, which is how a missing font appeared four minutes into a grub build. func_ind now goes where a RELA target reads it. x86-64 only and deliberately so: aarch64 and riscv64 are RELA and have the identical defect by inspection, but this platform builds neither and a change nothing measured is not a fix. Gated on three functions producing three distinct FDE start addends, at the first build and at stage 3. 0.9.28rc-17: __STDC_VERSION__ reports 201112 and __STDC_NO_ATOMICS__ is dropped (#235). The compiler implements C11 and told every consumer it did not, which is not a missing feature but a compiler describing itself inaccurately -- and portable code cannot probe for C11 atomics, there is no way to, so it tests the version macro and takes a worse path for no reason. python is the confirmed cost: its recipe is on gcc because pyatomic.h dispatches on GCC builtins, MSVC or C11 stdatomic and TCC appeared to offer none of the three, while stdatomic.h has been measured working standalone. Both edits are needed together -- upstream defines __STDC_NO_ATOMICS__ whenever the version is >= 201112, so bumping the version alone would announce C11 and deny atomics in the same breath, which is the exact macro CPython reads. __STDC_NO_COMPLEX__ and __STDC_NO_THREADS__ are deliberately left, since <threads.h> genuinely is not shipped. This change was written for rc-11 and carried through rc-12 and rc-13 alongside the constructor-priority patch; rc-14 dropped it and failed identically, which is what proved it was never the cause of those failures, and #219 has since been root-caused to tcc having no section sorting or merging for .init_array.NNNNN. It lands alone here. Gated on the macro AND on real atomics at both the first build and stage 3, because a compiler that announces C11 and denies atomics is precisely the state being fixed. 0.9.28rc-16: declare __builtin_bswap* as well as defining them (#208). rc-15 shipped the runtime and its gate failed: with no declaration in scope TCC applies the implicit int f() rule, so bswap64's 64-bit result is read as a 32-bit int and truncated, and bswap16's return carries junk in the upper bits. Declarations only, in tccdefs.h beside the __ATOMIC_* predefines -- never definitions, which would give every translation unit an unused static function to warn about under the -Wall -Werror this project builds with. The gate catching a subtly wrong fix is the argument for having written it. 0.9.28rc-15: bisecting -- __builtin_bswap* (#208) ALONE. rc-12, rc-13 and rc-14 all died with exit 121 immediately after the __atomic gate. rc-14 ruled out the C11 change (#235) by dropping it and failing identically, so the cause is one of the two remaining edits. This revision carries only the bswap runtime, which is also the more urgent fix: an undefined symbol is legal in a shared library, so a missing bswap ships silently. If this builds, the constructor-priority patch is the culprit and gets investigated on its own. 0.9.28rc-14: #219 and #208 only -- the C11 default reporting (#235) is split out and NOT included. rc-12 and rc-13 both failed with exit 121 immediately after the __atomic gate, and #235 is the change that could plausibly cause it: bumping __STDC_VERSION__ changes what every header does, and tcc's own stdatomic.h, stddef.h and stdalign.h all branch on it. Landing the two fixes that stand alone rather than holding them behind the one that needs its own investigation. 0.9.28rc-13: make the __atomic gate explain itself. rc-12 failed it with exit 121 and printed nothing, because the failure branch wrote to stderr while the build log carries stdout -- a gate that cannot say why it failed is half a gate. It now reports the code, the version macro, whether __STDC_NO_ATOMICS__ is defined and what the runtime archive contains. 0.9.28rc-12: measure what tcc's own linker does with .init_array.NNNNN before asserting it (#219). rc-11's gate failed, and the assumption underneath it -- that emitting the sections GCC emits is enough -- is exactly the kind that should not be guessed at: tcc links internally, and if it does not merge those suffixed sections a prioritised constructor may not run at all, which is worse than the compile error being fixed. This revision reports the observed order and the sections actually emitted. 0.9.28rc-11: three measured compiler fixes. constructor/destructor PRIORITY now parses and ORDERS (#219) -- the only regression the 0.9.28rc upgrade caused in a third-party package; parsed and honoured rather than accepted and ignored, since a priority that changes nothing is a wrong order nothing reports. __builtin_bswap16/32/64 supplied in libtcc1.a (#208) -- upstream fixed twelve of the fifteen missing builtins and left the three byte-order ones, which are the dangerous ones: an undefined symbol is legal in a shared library, so a .so builds, publishes and stays broken until called, exactly how libblkid and libnl shipped. __STDC_VERSION__ now reports 201112 (#235) and __STDC_NO_ATOMICS__ is dropped -- measured, _Static_assert, stdatomic, stdalign, stdnoreturn, _Generic and _Thread_local all work while the macro said C99, and upstream would have announced C11 and denied atomics in the same breath, which is the exact macro CPython dispatches on. Every fix has a gate that fails the build. 0.9.28rc-10: emit PT_GNU_STACK (#228). tcc has never emitted it, so glibc refuses to dlopen any TCC-built shared library -- python@3.13.5-5 died in make sharedinstall on a binascii module that built perfectly and could not be imported. Measured across the cache: every TCC-built .so lacks the header, every gcc-built one has it, and a pre-upgrade 0.9.27 libz.so lacks it too, so this predates ADR-0223. No flag exists to work around it -- tcc has no -z handling -- so tccelf.c now reserves and fills the header the same way it already does for PT_DYNAMIC, PT_NOTE, PT_TLS, PT_GNU_EH_FRAME and PT_GNU_RELRO, with PF_R|PF_W as GNU ld emits. Gated on a real shared object for presence AND non-executability. 0.9.28rc-9: rc-8 did not actually ship the library-path fix. --libpaths reached only the first configure, and the bootstrap reconfigures twice more, so stage 3 -- the binary that ships -- kept upstream default paths. Its verification grep also matched /usr/lib/<triplet> as a substring and passed vacuously. All three stages now share one definition, the check is anchored, and the compiler is asked what it will actually search. 0.9.28rc-8: search /lib/<triplet> for libraries. Upstream's default covers /usr/lib and /usr/lib/<triplet> only, but this platform installs shared libraries under /lib/<triplet> -- openssl puts libcrypto.so there -- so the first cixd hostbuild after the upgrade could not find -lcrypto. The triplet is derived from where libc actually is, configure's own record is checked, and a gate links against a library placed there. 0.9.28rc-7: a real three-stage bootstrap. The old comparison held two builds side by side that had been compiled by DIFFERENT compilers -- the installed release and the new source -- so it only ever passed because previous revisions barely changed codegen. Across a 2017-to-2026 jump it failed and blamed the compiler for not reproducing itself. Stages 2 and 3 are now both built by compilers built from this source, which is the property actually worth asserting. 0.9.28rc-6: the bootstrap stage snapshot is self-contained. Its cc wrapper pointed -B at the build directory, which make clean empties immediately afterwards, so stage 3 could not find libtcc1.a; it now points at the snapshot, copies include/ too, no longer swallows copy failures, and self-tests before the tree is cleaned. 0.9.28rc-5: the #216 gate used NULL without including a header and failed to compile. Spelled (void *)0 now, so the gate sources stay header-free and test the compiler rather than the include path. 0.9.28rc-4: the __dso_handle gate checks that the symbol links, not that it is NULL. It was asserting NULL because our own dropped stub defined it that way; upstream uses the self-referential value GCC uses, so the old gate failed against the more correct definition. 0.9.28rc-3: restores the generic __atomic_test_and_set/__atomic_clear runtime, which upstream still does not provide -- mob ships only the size-suffixed __atomic_test_and_set_N forms and no __atomic_clear. Dropping it was a wrong call from a probe that tested the <stdatomic.h> API rather than these builtins, and the recipe's own gate caught it on the first build. 0.9.28rc-2: the __ATOMIC_* predefines move to include/tccdefs.h, upstream's own predefine file -- the 0.9.27-era sed targeted a tcc_define_symbol() call in libtcc.c that mob does not have, and the recipe's own guard caught it and failed the build rather than shipping a compiler missing the macros. Also gates their VALUES, not just their presence. 0.9.28rc-1: upgrade from the 2017 0.9.27 release to pinned upstream mob 2ba12e83. Fixes #216 (two simultaneously-live locals sharing one stack slot, which miscompiled perl), brings real C11 (#212), fixes -pthread (#209) and 12 of 15 missing builtins (#208). Drops five of the six local patches this recipe carried -- upstream supplies all of them now, each verified individually rather than assumed -- and keeps the __ATOMIC_* predefines, which upstream still does not provide. Every gate is kept and one is added for #216"

pkg_build() {
	#
	# Send this build's stderr to stdout. The build log carries
	# stdout only, so a gate that reports on stderr reports into
	# nothing: rc-23 and rc-24 both died at exit 121 with a log
	# ending at the line before the failure, and rc-13 and rc-21
	# each fixed one earlier gate the same way. There are 57 stderr
	# redirections below and a gate that cannot say why it failed is
	# half a gate, so the fix belongs here rather than at each one.
	# An explicit capture into a file is a redirection of its own and
	# is unaffected.
	#
	exec 2>&1

	#
	# The one local change that survives the upgrade: the memory-order
	# constants, predefined exactly as GCC predefines them. Code
	# writes __ATOMIC_SEQ_CST without including anything, so no header
	# can supply them, and probe 10 confirmed upstream mob does not
	# predefine them even though it fully supports C11 atomics.
	#
	# Applied to include/tccdefs.h, which is upstream's own file of
	# predefined macros -- converted to C strings and compiled in as
	# tccdefs_.h, or read at runtime, depending on CONFIG_TCC_PREDEFS.
	# Either way this is the intended place for a predefine, and it is
	# a data file rather than a code anchor, so a future snapshot that
	# moves code around cannot silently defeat it.
	#
	# 0.9.27-14's version of this patch sed'd a tcc_define_symbol()
	# call in libtcc.c. That call does not exist in mob -- predefines
	# moved to tccpp.c and are emitted as text -- so the sed matched
	# nothing. The recipe caught it and failed the build rather than
	# shipping a compiler quietly missing the macros, which is exactly
	# what that guard was written for.
	#
	# Indent matters here and is not decoration: tccdefs.h's own
	# header comment states that only lines indented four or more
	# spaces are included into the executable. Appended after the
	# file's final #endif so the macros are unconditional rather than
	# scoped to non-preprocessor mode.
	#
	cat >> include/tccdefs.h <<'DEFS_EOF'

    unsigned short __builtin_bswap16(unsigned short);
    unsigned int __builtin_bswap32(unsigned int);
    unsigned long long __builtin_bswap64(unsigned long long);
    #define __ATOMIC_RELAXED 0
    #define __ATOMIC_CONSUME 1
    #define __ATOMIC_ACQUIRE 2
    #define __ATOMIC_RELEASE 3
    #define __ATOMIC_ACQ_REL 4
    #define __ATOMIC_SEQ_CST 5
DEFS_EOF
	#
	# Verify the edit landed AND kept the indent that makes it count.
	# A four-space-indented line is the difference between a macro
	# that ships and a comment nobody sees.
	#
	grep -q '^    unsigned long long __builtin_bswap64(unsigned long long);$' include/tccdefs.h || {
		echo "tcc: the __builtin_bswap* declarations did not land in include/tccdefs.h (#208)" >&2
		exit 1
	}
	grep -q '^    #define __ATOMIC_SEQ_CST 5$' include/tccdefs.h || {
		echo "tcc: the __ATOMIC_* predefines did not land in include/tccdefs.h with the required indent" >&2
		exit 1
	}

	#
	# The generic __atomic_test_and_set / __atomic_clear builtins,
	# which upstream still does not provide.
	#
	# This was very nearly dropped. Probe 10 confirmed mob ships
	# stdatomic.o and atomic.o and that a real C11 <stdatomic.h>
	# program compiles and RUNS -- true, and not the same question.
	# mob's lib/atomic.S defines only the SIZE-SUFFIXED forms
	# (__atomic_test_and_set_1/_2/_4/_8) and no __atomic_clear at all,
	# while GCC also exposes the generic, unsuffixed spellings that
	# code actually writes. The recipe's own gate caught the gap on
	# the first build attempt, which is the entire argument for
	# keeping gates for bugs the compiler no longer has.
	#
	# Named atomic_compat.c, not atomic.c: mob already has its own
	# lib/atomic.S producing atomic.o, and two sources competing for
	# one object name is a collision waiting to happen.
	#
	# memorder is accepted and ignored because seq_cst is the
	# strongest ordering: answering every weaker request with a
	# stronger guarantee is always correct.
	#
	cat > lib/atomic_compat.c <<'ATOMIC_EOF'
/*
 * Generic atomic builtins upstream TCC does not implement, supplied as
 * real functions in the compiler's own runtime library. Upstream
 * provides only the size-suffixed __atomic_test_and_set_N forms.
 * See tcc.recipe for the full reasoning.
 */

int __atomic_test_and_set(volatile void *ptr, int memorder)
{
	unsigned char prev = 1;

	(void)memorder;
	__asm__ __volatile__("xchgb %0, %1"
	                     : "+q"(prev), "+m"(*(volatile unsigned char *)ptr)
	                     :
	                     : "memory");
	return prev != 0;
}

void __atomic_clear(volatile void *ptr, int memorder)
{
	unsigned char zero = 0;

	(void)memorder;
	__asm__ __volatile__("xchgb %0, %1"
	                     : "+q"(zero), "+m"(*(volatile unsigned char *)ptr)
	                     :
	                     : "memory");
}
ATOMIC_EOF
	#
	# Into COMMON_O, which every target's object list already
	# includes, so this lands in libtcc1.a exactly once. Purely
	# additive: no existing object or symbol changes.
	#
	sed -i 's|^COMMON_O = stdatomic.o atomic.o builtin.o alloca.o alloca-bt.o$|COMMON_O = stdatomic.o atomic.o atomic_compat.o builtin.o alloca.o alloca-bt.o|' \
	    lib/Makefile
	grep -q 'atomic_compat\.o' lib/Makefile || {
		echo "tcc: failed to add atomic_compat.o to lib/Makefile -- COMMON_O moved" >&2
		exit 1
	}

	#
	# PT_GNU_STACK, which tcc has never emitted (#228).
	#
	# An ELF shared object with no PT_GNU_STACK program header is
	# treated by the loader as REQUIRING an executable stack, and
	# glibc then refuses to dlopen it:
	#
	#   libz.so.1: cannot enable executable stack as shared object
	#   requires: Invalid argument
	#
	# Measured across the cache: every TCC-built .so lacks the header
	# (libz, libmnl, libformw, libnl-nf) and every gcc-built one has
	# it (libblkid, libcrypt). A pre-upgrade 0.9.27 libz.so lacks it
	# too, so this is not an ADR-0223 regression -- it predates the
	# snapshot, and PT_GNU_STACK appears in upstream only as a
	# constant in elf.h, never emitted.
	#
	# It bites dlopen specifically, which is why it went unseen: a
	# normal DT_NEEDED load is not subject to the check, so every
	# TCC-built library on this platform works until something opens
	# one at runtime. CPython does, and python@3.13.5-5 died in
	# `make sharedinstall` on a binascii module that had built
	# perfectly and then could not be imported.
	#
	# Fixed rather than worked around, because there is no flag to
	# work around WITH -- tcc has no -z option handling at all. The
	# three edits mirror what tccelf.c already does for PT_DYNAMIC,
	# PT_NOTE, PT_TLS, PT_GNU_EH_FRAME and PT_GNU_RELRO: a field to
	# hold the index, a reserved slot, and a fill. PF_R|PF_W is what
	# GNU ld emits for a non-executable stack; the phdr array is
	# tcc_mallocz'd, so every other field is already zero, which is
	# also what ld emits.
	#
	# ADR-0223 moved this project off maintaining tcc patches, and
	# this does not reopen that: it is not a codegen patch but a
	# missing ELF header, small and stable, and it is worth
	# upstreaming rather than carrying. The gate below is what makes
	# carrying it safe until then.
	#
	sed -i 's|^    int dyna;$|    int dyna;\n    int stack;|' tccelf.c
	sed -i 's|^    d->phnum = phnum;$|    d->stack = phnum++;\n    d->phnum = phnum;|' tccelf.c
	sed -i 's|^        fill_phdr(\&d->phdr\[d->ehfr\], PT_GNU_EH_FRAME, eh_frame_hdr_section);$|        fill_phdr(\&d->phdr[d->ehfr], PT_GNU_EH_FRAME, eh_frame_hdr_section);\n    ph2 = fill_phdr(\&d->phdr[d->stack], PT_GNU_STACK, NULL);\n    ph2->p_flags = PF_R \| PF_W;|' tccelf.c
	for frag in 'int stack;' 'd->stack = phnum++;' 'PT_GNU_STACK, NULL'; do
		grep -qF "$frag" tccelf.c || {
			echo "tcc: PT_GNU_STACK patch did not apply -- '$frag' missing, tccelf.c moved (#228)" >&2
			exit 1
		}
	done
	echo "tcc: PT_GNU_STACK patch applied"


	#
	# __builtin_bswap16/32/64 -- issue #208.
	#
	# Upstream's 0.9.28rc fixed twelve of the fifteen builtins this
	# project found missing; ffs, clz, clzll and popcount all work
	# now, measured. The three byte-order ones do not, and they are
	# the dangerous ones: TCC emits an ordinary undefined external
	# rather than implementing or rejecting them, and an undefined
	# symbol is LEGAL in a shared library. So a .so builds, installs,
	# publishes and stays broken until something calls it -- which is
	# exactly how libblkid (#176) and libnl shipped.
	#
	# Supplied as real functions in libtcc1.a rather than as codegen,
	# the same shape as this recipe's own atomic_compat.c: TCC already
	# emits a plain call, so a definition is all that is missing. It
	# forgoes inlining and constant folding, which is a fair price for
	# correct bytes, and it keeps ADR-0223's posture -- no codegen
	# patch to maintain.
	#
	# Byte order is written out by shifting rather than with any
	# host-endianness assumption. squashfs-tools shipped a real
	# corruption bug from exactly that class of assumption.
	#
	# The runtime alone is NOT enough, and rc-15's gate proved it.
	# With no declaration in scope TCC falls back to the implicit
	# `int f()` rule, so a caller reads __builtin_bswap64's 64-bit
	# result as a 32-bit int and truncates it, and bswap16's return
	# carries whatever the upper bits of the register held. The
	# declarations therefore go into include/tccdefs.h with the
	# __ATOMIC_* predefines -- declarations only, never definitions,
	# so no translation unit gains an unused static function to warn
	# about under -Wall -Werror, which this project builds with
	# everywhere.
	#
	cat > lib/builtin_compat.c <<'BUILTIN_EOF'
/*
 * __builtin_bswap{16,32,64} -- see tcc.recipe (issue #208).
 *
 * TCC emits a call to these and defines nothing, so a shared library
 * carrying one links cleanly with an undefined symbol and fails only
 * when called. Defined here so libtcc1.a resolves them.
 */
unsigned short __builtin_bswap16(unsigned short v)
{
	return (unsigned short)(((v & 0xff00u) >> 8) | ((v & 0x00ffu) << 8));
}

unsigned int __builtin_bswap32(unsigned int v)
{
	return ((v & 0xff000000u) >> 24) | ((v & 0x00ff0000u) >> 8) |
	       ((v & 0x0000ff00u) << 8) | ((v & 0x000000ffu) << 24);
}

unsigned long long __builtin_bswap64(unsigned long long v)
{
	return ((v & 0xff00000000000000ULL) >> 56) | ((v & 0x00ff000000000000ULL) >> 40) |
	       ((v & 0x0000ff0000000000ULL) >> 24) | ((v & 0x000000ff00000000ULL) >> 8) |
	       ((v & 0x00000000ff000000ULL) << 8) | ((v & 0x0000000000ff0000ULL) << 24) |
	       ((v & 0x000000000000ff00ULL) << 40) | ((v & 0x00000000000000ffULL) << 56);
}
BUILTIN_EOF
	sed -i 's|^COMMON_O = stdatomic.o atomic.o atomic_compat.o builtin.o alloca.o alloca-bt.o$|COMMON_O = stdatomic.o atomic.o atomic_compat.o builtin_compat.o builtin.o alloca.o alloca-bt.o|' \
		lib/Makefile
	grep -q 'builtin_compat.o' lib/Makefile || {
		echo "tcc: builtin_compat.o did not reach COMMON_O -- lib/Makefile moved (#208)" >&2
		exit 1
	}
	echo "tcc: __builtin_bswap* compat applied"
	#
	# Report C11, because C11 is what this compiler provides -- issue
	# #235.
	#
	# Measured on a real host (probe-tcc-conformance/19): _Static_
	# assert, <stdatomic.h>, <stdalign.h>, <stdnoreturn.h>, _Generic
	# and _Thread_local ALL work, while __STDC_VERSION__ reports
	# 199901. Portable code cannot probe for C11 -- there is no way
	# to -- so it tests the version macro, which is what the standard
	# provides, and takes a worse path for no reason. CPython is the
	# confirmed case: its pyatomic.h dispatches on GCC builtins, MSVC
	# or C11 stdatomic, and the third is available.
	#
	# The second edit is the one that makes the first useful.
	# Upstream defines __STDC_NO_ATOMICS__ whenever the version is
	# >= 201112, so bumping the version alone would announce C11 and
	# deny atomics in the same breath -- and pyatomic.h checks exactly
	# that macro. Dropping it is honest here and only here, because
	# the atomics are measured to work; __STDC_NO_THREADS__ and
	# __STDC_NO_COMPLEX__ are left alone, since <threads.h> genuinely
	# is not shipped.
	#
	sed -i 's|^    s->cversion = 199901; /\* default unless -std=c11 is supplied \*/$|    s->cversion = 201112; /* C11: measured to be what this compiler provides (#235) */|' libtcc.c
	sed -i '/^    # define __STDC_NO_ATOMICS__ 1$/d' include/tccdefs.h
	grep -q 's->cversion = 201112;' libtcc.c || {
		echo "tcc: the default C version was not bumped -- libtcc.c moved (#235)" >&2
		exit 1
	}
	grep -q '__STDC_NO_ATOMICS__' include/tccdefs.h && {
		echo "tcc: __STDC_NO_ATOMICS__ is still defined -- it would deny the atomics we ship (#235)" >&2
		exit 1
	}
	echo "tcc: C11 version reporting applied"

	#
	# Every .eh_frame FDE claimed to describe code at .text+0 -- issue
	# #227.
	#
	# tcc_debug_frame_end() emits the FDE's initial-location
	# relocation and then writes the function's own offset into the
	# slot it covers:
	#
	#     dwarf_reloc(eh_frame_section, eh_section_sym, R_X86_64_PC32);
	#     dwarf_data4(eh_frame_section, func_ind); // PC Begin
	#
	# That is REL semantics, where the in-place value is the addend.
	# i386 and ARM are REL and are correct. x86-64 is RELA, where the
	# in-place value is discarded: tccelf.c computes
	# `tgt = sym->st_value + rel->r_addend` and x86_64-link.c writes
	# `val - addr` over the slot. dwarf_reloc() passes addend 0, so
	# func_ind never reaches the linker and every FDE resolves to the
	# base of .text.
	#
	# Confirmed against the pinned source rather than inferred, and it
	# matches the reported readelf exactly: eight FDEs in one object,
	# all `.text + 0`, with the right LENGTHS and identical starts.
	#
	# The consequence is the dangerous kind. tcc links its own output
	# fine because nothing consumes .eh_frame unless an exception is
	# thrown, so the library builds, installs, checksums and publishes
	# clean -- and GNU ld then rejects it with "`.eh_frame_hdr` refers
	# to overlapping FDEs" when a gcc-built package links against it.
	# It travelled as a working artifact and surfaced as a missing font
	# four minutes into a grub build.
	#
	# The fix is to put func_ind where a RELA target actually reads it.
	# x86-64 only, deliberately: aarch64 and riscv64 are RELA and have
	# the identical defect by inspection, but this platform builds
	# neither, so fixing them here would be a change nothing measured.
	# Recorded in #227 instead.
	#
	sed -i 's|^    dwarf_reloc(eh_frame_section, eh_section_sym, R_X86_64_PC32);$|    put_elf_reloca(symtab_section, eh_frame_section, eh_frame_section->data_offset,\n                   R_X86_64_PC32, eh_section_sym, func_ind);|' tccdbg.c
	grep -q 'R_X86_64_PC32, eh_section_sym, func_ind);' tccdbg.c || {
		echo "tcc: the .eh_frame FDE addend patch did not apply -- tccdbg.c moved (#227)" >&2
		exit 1
	}
	echo "tcc: .eh_frame FDE addend patch applied"

	#
	# Constructor/destructor PRIORITY: parsed, and actually honoured --
	# issue #219.
	#
	# Two independent things were wrong and only fixing both helps.
	#
	# 1. The attribute parser knows `constructor` and never reads its
	#    optional priority, so the '(' after it is a syntax error:
	#    "error: ')' expected (got '('". That is the visible symptom,
	#    and the one libcap@2.78-13 works around by stripping it.
	#
	# 2. Emitting the sections GCC emits is NOT enough, and assuming it
	#    was is what made three earlier attempts fail. GCC puts a
	#    prioritised entry in .init_array.NNNNN and relies on GNU ld's
	#    linker script to SORT_BY_NAME them and merge them into
	#    .init_array. tcc has neither half: find_section() gives every
	#    distinct name its own section in creation order, there is no
	#    name sorting anywhere in its linker, and both ways of finding
	#    the array at runtime -- __init_array_start/_end and
	#    DT_INIT_ARRAY -- describe only the section literally called
	#    .init_array. A prioritised constructor was therefore not merely
	#    misordered but outside the range anything iterates.
	#
	# So the merge is supplied here, run before the array's bounds are
	# computed. Written to its own file and #included rather than
	# threaded through sed, because sixty lines of C inserted a line at
	# a time is how a patch becomes unreviewable.
	#
	cat > cix_initarray.inc <<'CIX_INITARRAY_EOF'
/*
 * cix_merge_prio_arrays() -- what upstream tcc does not have (#219).
 *
 * GCC expresses a constructor priority by emitting the entry into
 * .init_array.NNNNN, and GNU ld's linker script then does two things
 * with those: SORT_BY_NAME() them, and merge them into .init_array
 * ahead of the unsuffixed entries. tcc has neither half. Its linker
 * gives every distinct section name its own section in creation order,
 * and both ways of finding the array at runtime --
 * __init_array_start/_end (add_init_array_defines) and DT_INIT_ARRAY --
 * describe only the section literally named .init_array. So a
 * prioritised constructor emitted into a suffixed section is not merely
 * ordered wrongly: it is outside the range anything iterates.
 *
 * This does both halves before the array's bounds are computed. Every
 * slot is an identical PTR_SIZE of zeroes and all the identity lives in
 * the relocation, so merging is a matter of rebuilding one relocation
 * list in the right order: prioritised entries first, ascending, then
 * the unprioritised ones. The %05u zero-padding the emitter uses is
 * what makes strcmp() the numeric order.
 *
 * .fini_array needs no reversal: glibc walks it backwards, so the same
 * ascending layout yields GCC's destructor semantics for free.
 */
static void cix_merge_prio_arrays(TCCState *s1, const char *base_name)
{
	Section *base = NULL;
	int idx[64];
	int n = 0, i, j;
	size_t prefix_len = strlen(base_name);
	ElfW_Rel *saved = NULL;
	int saved_n = 0;

	for (i = 1; i < s1->nb_sections; i++) {
		Section *s = s1->sections[i];

		if (strncmp(s->name, base_name, prefix_len) != 0)
			continue;
		if (s->name[prefix_len] == '\0') {
			base = s;
			continue;
		}
		if (s->name[prefix_len] != '.')
			continue;
		if (n < (int)(sizeof(idx) / sizeof(idx[0])))
			idx[n++] = i;
	}
	if (n == 0)
		return; /* nothing prioritised -- the ordinary case, untouched */

	/* insertion sort by name; the zero-padded suffix makes this numeric */
	for (i = 1; i < n; i++) {
		int cur = idx[i];

		for (j = i - 1;
		     j >= 0 && strcmp(s1->sections[idx[j]]->name, s1->sections[cur]->name) > 0; j--)
			idx[j + 1] = idx[j];
		idx[j + 1] = cur;
	}

	if (base == NULL) {
		base = find_section(s1, base_name);
		base->sh_flags = shf_RELRO;
		base->sh_type = base_name[1] == 'i' ? SHT_INIT_ARRAY : SHT_FINI_ARRAY;
	}

	/* the unprioritised entries already in base go last, so keep them aside */
	if (base->reloc != NULL && base->reloc->data_offset > 0) {
		saved_n = (int)(base->reloc->data_offset / sizeof(ElfW_Rel));
		saved = tcc_malloc(base->reloc->data_offset);
		memcpy(saved, base->reloc->data, base->reloc->data_offset);
	}
	base->data_offset = 0;
	if (base->reloc != NULL)
		base->reloc->data_offset = 0;

	for (i = 0; i < n; i++) {
		Section *s = s1->sections[idx[i]];
		Section *sr = s->reloc;
		int k, count;

		if (sr == NULL)
			continue;
		count = (int)(sr->data_offset / sizeof(ElfW_Rel));
		for (k = 0; k < count; k++) {
			ElfW_Rel *rel = &((ElfW_Rel *)sr->data)[k];
			addr_t addend = 0;
#if SHT_RELX == SHT_RELA
			addend = rel->r_addend;
#endif
			put_elf_reloca(symtab_section, base, base->data_offset,
			                ELFW(R_TYPE)(rel->r_info), ELFW(R_SYM)(rel->r_info), addend);
			section_ptr_add(base, PTR_SIZE);
		}
		/* emptied and unallocated: its entries live in base now, and a
		 * leftover SHF_ALLOC section would be laid out for nothing */
		s->data_offset = 0;
		s->sh_flags &= ~SHF_ALLOC;
		if (sr != NULL)
			sr->data_offset = 0;
	}

	for (i = 0; i < saved_n; i++) {
		ElfW_Rel *rel = &saved[i];
		addr_t addend = 0;
#if SHT_RELX == SHT_RELA
		addend = rel->r_addend;
#endif
		put_elf_reloca(symtab_section, base, base->data_offset, ELFW(R_TYPE)(rel->r_info),
		                ELFW(R_SYM)(rel->r_info), addend);
		section_ptr_add(base, PTR_SIZE);
	}
	tcc_free(saved);
}
CIX_INITARRAY_EOF

	sed -i 's|^static void add_init_array_defines(TCCState \*s1, const char \*section_name)$|#include "cix_initarray.inc"\n\nstatic void add_init_array_defines(TCCState *s1, const char *section_name)|' tccelf.c
	sed -i 's|^    add_init_array_defines(s1, ".preinit_array");$|    cix_merge_prio_arrays(s1, ".init_array");\n    cix_merge_prio_arrays(s1, ".fini_array");\n    add_init_array_defines(s1, ".preinit_array");|' tccelf.c

	#
	# The priority itself lives in FuncAttr's own spare bits, and is
	# range-checked rather than silently truncated -- a truncated
	# priority is a wrong order that nothing reports.
	#
	sed -i 's|^    xxxx        : 15;$|    func_ctorprio : 15; /* ctor or dtor priority, 0 = none (#219) */|' tcc.h
	sed -i 's|^            ad->f.func_ctor = 1;$|            ad->f.func_ctor = 1;\n            if (tok == (int)('"'"'('"'"')) { next(); n = expr_const(); if (n < 0 \|\| n > 32767) tcc_error("constructor priority out of range"); ad->f.func_ctorprio = n; skip('"'"')'"'"'); }|' tccgen.c
	sed -i 's|^            ad->f.func_dtor = 1;$|            ad->f.func_dtor = 1;\n            if (tok == (int)('"'"'('"'"')) { next(); n = expr_const(); if (n < 0 \|\| n > 32767) tcc_error("destructor priority out of range"); ad->f.func_ctorprio = n; skip('"'"')'"'"'); }|' tccgen.c
	sed -i 's|^        add_array (tcc_state, ".init_array", sym->c);$|        { char psec[32]; if (sym->type.ref->f.func_ctorprio) snprintf(psec, sizeof(psec), ".init_array.%05u", sym->type.ref->f.func_ctorprio); else snprintf(psec, sizeof(psec), ".init_array"); add_array (tcc_state, psec, sym->c); }|' tccgen.c
	sed -i 's|^        add_array (tcc_state, ".fini_array", sym->c);$|        { char psec[32]; if (sym->type.ref->f.func_ctorprio) snprintf(psec, sizeof(psec), ".fini_array.%05u", sym->type.ref->f.func_ctorprio); else snprintf(psec, sizeof(psec), ".fini_array"); add_array (tcc_state, psec, sym->c); }|' tccgen.c

	for frag in 'func_ctorprio : 15;' 'constructor priority out of range' '".init_array.%05u"' 'cix_merge_prio_arrays(s1, ".init_array");' '#include "cix_initarray.inc"'; do
		if ! grep -qF "$frag" tcc.h tccgen.c tccelf.c; then
			echo "tcc: the constructor-priority patch did not apply -- '$frag' missing (#219)" >&2
			exit 1
		fi
	done
	echo "tcc: constructor/destructor priority patch applied"



	#
	# Library search has to include /lib/<triplet>, and upstream's
	# default does not.
	#
	# mob's built-in CONFIG_TCC_LIBPATHS is "{B}" plus /usr/lib and
	# /usr/lib/<triplet>. This platform installs shared libraries
	# under /lib/<triplet> -- openssl puts libcrypto.so exactly
	# there -- so the first hostbuild of cixd after the upgrade
	# failed with `tcc: error: library 'crypto' not found` even
	# though the library was installed and correct.
	#
	# The triplet is derived from where libc actually is rather than
	# hardcoded, so this states a fact about the machine instead of
	# an assumption about it.
	#
	libcdir=$(dirname "$(ls /lib/*/libc.so.6 2>/dev/null | head -1)")
	triplet=$(basename "$libcdir")
	case "$triplet" in
	*-linux-*) ;;
	*)
		echo "tcc: could not determine the library triplet (found '$triplet' from '$libcdir')" >&2
		exit 1
		;;
	esac
	echo "tcc: library triplet is $triplet"

	#
	# ONE definition, used by all three bootstrap stages.
	#
	# 0.9.28rc-8 passed --libpaths to the first configure only. The
	# bootstrap then runs `make clean; ./configure ...` twice more,
	# and those two invocations did not carry the option -- so the
	# stage 3 binary, which is the one that actually ships, was built
	# with upstream's default search path and still could not find
	# -lcrypto. The gates all passed, because they ran against the
	# first build, which did have it.
	#
	tcc_libpaths="{B}:/usr/lib:/usr/lib/$triplet:/lib:/lib/$triplet"

	./configure --prefix=/usr --cc=tcc --libpaths="$tcc_libpaths"

	#
	# Verify configure recorded it -- carefully.
	#
	# 0.9.28rc-8 checked this with `grep "/lib/$triplet"`, which
	# matches "/usr/lib/x86_64-linux-gnu" as a substring. That is the
	# DEFAULT path, so the check passed while proving nothing. A
	# vacuous gate is worse than no gate: it reports success and stops
	# anyone looking further. Anchored on the separator now, so only
	# the real entry can satisfy it.
	#
	grep -q ":/lib/$triplet" config.h || {
		echo "tcc: configure did not record /lib/$triplet in the library search path" >&2
		grep -i libpath config.h >&2 || true
		exit 1
	}

	make -j"$(nproc)"

	#
	# ---- gates ----
	#
	# Each of these runs the compiler that was just built and checks
	# VALUES, not exit status. That distinction is the lesson of #122:
	# a miscompiled tar exited 0 on every archive while listing one
	# member.
	#

	# __has_include: all three answers must be RIGHT, not merely
	# "it compiled".
	cat > has_include_check.c <<'CHECK_EOF'
#if __has_include(<stdio.h>)
int present = 1;
#else
#error "__has_include said an existing system header is absent"
#endif
#if __has_include(<cix_definitely_not_a_real_header.h>)
#error "__has_include said a nonexistent header is present"
#endif
#if __has_include("cix_definitely_not_here_either.h")
#error "__has_include said a nonexistent quoted header is present"
#endif
int main(void) { return present ? 0 : 1; }
CHECK_EOF
	./tcc -B. has_include_check.c -o has_include_check
	./has_include_check || { echo "tcc: __has_include is present but wrong" >&2; exit 1; }

	# The __atomic_* builtins, used the way a caller uses them: no
	# header, no library flag.
	cat > atomic_check.c <<'CHECK_EOF'
static char mutex;
int main(void)
{
	/* the predefined constants must exist AND have GCC's values */
	if (__ATOMIC_RELAXED != 0 || __ATOMIC_CONSUME != 1 || __ATOMIC_ACQUIRE != 2)
		return 10;
	if (__ATOMIC_RELEASE != 3 || __ATOMIC_ACQ_REL != 4 || __ATOMIC_SEQ_CST != 5)
		return 11;
	if (__atomic_test_and_set(&mutex, __ATOMIC_SEQ_CST))
		return 1;                       /* was free, must report free */
	if (!__atomic_test_and_set(&mutex, __ATOMIC_SEQ_CST))
		return 2;                       /* now held, must report held */
	__atomic_clear(&mutex, __ATOMIC_SEQ_CST);
	if (__atomic_test_and_set(&mutex, __ATOMIC_SEQ_CST))
		return 3;                       /* released, must report free */
	return 0;
}
CHECK_EOF
	./tcc -B. atomic_check.c -o atomic_check
	#
	# rc-12 died here with exit 121 and no explanation, because the
	# failure branch wrote to stderr and the build log carries stdout.
	# A gate that cannot say why it failed is half a gate, so this one
	# reports on stdout, with the state that would explain it.
	#
	if ./atomic_check; then
		echo "tcc: __atomic_* runtime ok"
	else
		rc=$?
		echo "tcc: __atomic_check FAILED rc=$rc (1/2/3 = wrong runtime, 10/11 = wrong __ATOMIC_* values)"
		echo "tcc: environment at that point:"
		printf '#include <stdio.h>\nint main(void){printf("__STDC_VERSION__=%%ld\\n",(long)__STDC_VERSION__);\n#ifdef __STDC_NO_ATOMICS__\nprintf("__STDC_NO_ATOMICS__ defined\\n");\n#else\nprintf("__STDC_NO_ATOMICS__ not defined\\n");\n#endif\nprintf("SEQ_CST=%%d RELAXED=%%d\\n", __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);return 0;}\n' > env_check.c
		./tcc -B. env_check.c -o env_check && ./env_check | sed 's/^/  /'
		echo "tcc: nm on the runtime archive:"
		nm libtcc1.a 2>/dev/null | grep -i "atomic_test_and_set\|atomic_clear" | sed 's/^/  /' | head -6
		exit 1
	fi

	# C11 <stdatomic.h> through to a real answer. New with this
	# upgrade (#212) and gated from the start, so a later snapshot
	# that loses it fails here rather than in whatever package needed
	# it next.
	cat > stdatomic_check.c <<'CHECK_EOF'
#include <stdatomic.h>
int main(void)
{
	atomic_int a;
	atomic_init(&a, 1);
	atomic_fetch_add(&a, 41);
	return atomic_load(&a) == 42 ? 0 : 1;
}
CHECK_EOF
	if ! ./tcc -B. stdatomic_check.c -o stdatomic_check 2>stdatomic_err.txt; then
		rc=$?
		echo "tcc: stdatomic_check did not COMPILE, rc=$rc (#212)" >&2
		sed 's/^/  /' stdatomic_err.txt >&2
		echo "tcc: does the header resolve at all?" >&2
		printf '#include <stdatomic.h>\nint main(void){return 0;}\n' > sa_hdr.c
		./tcc -B. -E sa_hdr.c >/dev/null 2>sa_hdr_err.txt \
		    && echo "  header preprocesses fine -- the failure is in the body" >&2 \
		    || sed 's/^/  /' sa_hdr_err.txt >&2
		printf '#include <stdio.h>\nint main(void){printf("__STDC_VERSION__=%%ld\\n",(long)__STDC_VERSION__);return 0;}\n' > sa_ver.c
		./tcc -B. sa_ver.c -o sa_ver 2>/dev/null && ./sa_ver | sed 's/^/  /' >&2
		exit 1
	fi
	if ! ./stdatomic_check; then
		rc=$?
		echo "tcc: C11 stdatomic compiles but computes the wrong answer, rc=$rc (#212)" >&2
		exit 1
	fi
	# #235 gate: the version macro must say C11 AND the atomics it
	# implies must actually be usable. Checking the macro alone would
	# pass a compiler that announces C11 and denies atomics in the
	# same breath, which is precisely the state this fixes.
	cat > c11_check.c <<'C11_EOF'
#include <stdatomic.h>
#if __STDC_VERSION__ < 201112L
#error "not reporting C11"
#endif
#ifdef __STDC_NO_ATOMICS__
#error "announces C11 and denies the atomics it ships"
#endif
static atomic_int v;
int main(void)
{
	atomic_store(&v, 41);
	return (atomic_load(&v) == 41 && atomic_fetch_add(&v, 1) == 41 &&
	        atomic_load(&v) == 42) ? 0 : 1;
}
C11_EOF
	./tcc -B. c11_check.c -o c11_check
	./c11_check || { echo "tcc: C11 reporting or atomics are broken (#235)" >&2; exit 1; }

	# #219 gate: priority must PARSE and must ORDER. A compiler that
	# accepts the attribute and ignores the number passes the first
	# half and fails this -- and one that emits the sections without
	# merging them fails it by running nothing at all.
	cat > ctorprio_check.c <<'CTOR_EOF'
static int seq;
static int a_at, b_at, c_at;
void a(void) __attribute__((constructor(900)));
void b(void) __attribute__((constructor(100)));
void c(void) __attribute__((constructor));
void a(void) { a_at = ++seq; }
void b(void) { b_at = ++seq; }
void c(void) { c_at = ++seq; }
int main(void)
{
	/* 100 before 900, both before the unprioritised one, which the
	 * merge places after every .init_array.NNNNN entry. A zero means
	 * that constructor never ran at all, which is the failure the
	 * suffixed sections caused before they were merged. */
	if (b_at == 1 && a_at == 2 && c_at == 3)
		return 0;
	return 100 + b_at * 10 + a_at;
}
CTOR_EOF
	./tcc -B. ctorprio_check.c -o ctorprio_check
	./ctorprio_check
	ctorprio_rc=$?
	if [ "$ctorprio_rc" -ne 0 ]; then
		#
		# The exit code carries the observed order: 100 + b*10 + a, so
		# 121 is declaration order (the merge did not sort) and 100 is
		# neither prioritised constructor running at all (they are
		# still outside the iterated range). Captured into a variable
		# first because `$?` read inside `if ! cmd` is the status of
		# the negation, not of the command -- which is how rc-20's own
		# gate reported "exit 0" for a failure and told us nothing.
		#
		echo "tcc: constructor priority is not honoured (#219), rc=$ctorprio_rc" >&2
		echo "     100 + b*10 + a; 121 = declaration order, 100 = neither ran" >&2
		echo "tcc: sections in the object:" >&2
		./tcc -B. -c ctorprio_check.c -o ctorprio_check.o
		readelf -SW ctorprio_check.o 2>/dev/null | grep -i 'init_array' >&2 || true
		echo "tcc: sections in the linked binary:" >&2
		readelf -SW ctorprio_check 2>/dev/null | grep -i 'init_array' >&2 || true
		echo "tcc: .init_array relocations in the binary:" >&2
		readelf -rW ctorprio_check 2>/dev/null | grep -A8 -i 'init_array' >&2 || true
		exit 1
	fi
	echo "tcc: #219 constructor priority parses and orders"

	# #227 gate: each FDE must describe its OWN function. Three
	# functions of different sizes, so three FDEs with three different
	# start offsets -- if the addends collapse to one value again,
	# every FDE claims the same code and GNU ld rejects the object
	# with "overlapping FDEs", which is the failure this fixes.
	cat > fde_check.c <<'FDE_EOF'
int fde_a(int x) { return x + 1; }
int fde_b(int x) { return x * 2 + 3; }
int fde_c(int x) { int i, s = 0; for (i = 0; i < x; i++) s += i; return s; }
FDE_EOF
	./tcc -B. -c fde_check.c -o fde_check.o
	readelf -rW fde_check.o > fde_check.rel 2>/dev/null || true
	fde_addends=$(awk '/Relocation section .\.rela\.eh_frame/ {f=1; next}
	                   /^Relocation section/ {f=0}
	                   f && $1 ~ /^[0-9a-f]+$/ {print $NF}' fde_check.rel)
	fde_n=$(printf '%s\n' "$fde_addends" | grep -c . || true)
	fde_d=$(printf '%s\n' "$fde_addends" | sort -u | grep -c . || true)
	echo "tcc: #227 .eh_frame FDE relocations: $fde_n entries, $fde_d distinct start addends"
	if [ "$fde_n" -lt 3 ]; then
		echo "tcc: expected at least 3 FDE relocations, got $fde_n (#227)" >&2
		sed 's/^/  /' fde_check.rel >&2
		exit 1
	fi
	if [ "$fde_n" != "$fde_d" ]; then
		echo "tcc: FDEs share start addresses -- GNU ld will reject this object (#227)" >&2
		sed 's/^/  /' fde_check.rel >&2
		exit 1
	fi


	# __dso_handle resolves out of the runtime archive with no help
	# from the caller. Seven recipes each carried their own stub
	# before this was guaranteed; upstream supplies it now, and this
	# gate is what keeps that true.
	#
	# The test is that it LINKS, not what it contains. 0.9.27-14's
	# version of this gate asserted the value was NULL, which was true
	# only because our own stub defined it that way -- upstream's
	# lib/dsohandle.c uses the self-referential
	# `__dso_handle = &__dso_handle` that GCC itself uses, so the old
	# assertion failed against a MORE correct definition. Checking the
	# address instead cannot make that mistake: a missing symbol fails
	# the link outright, before this ever runs.
	printf 'extern void *__dso_handle;\nint main(void){return &__dso_handle == (void *)0;}\n' \
	    > dso_check.c
	./tcc -B. dso_check.c -o dso_check
	./dso_check || { echo "tcc: __dso_handle did not resolve from libtcc1.a" >&2; exit 1; }

	# do-while regression gate (issue #122). The loop must run 3 times
	# AND the condition must be evaluated 3 times, through a
	# continue-inside-switch whose body end is unreachable. A compiler
	# with the old bug runs it once, never calls the condition, and
	# exits 1. Upstream fixed this; the gate stays so a future
	# snapshot cannot quietly unfix it.
	cat > dowhile_check.c <<'CHECK_EOF'
static int calls;
static int cond(void) { calls++; return calls >= 3; }
static int next_status(void) { static int n; return (n++ < 10) ? 1 : 2; }
int main(void)
{
	int status, iterations = 0;
	do {
		iterations++;
		status = next_status();
		switch (status) {
		case 0: break;
		case 1: continue;
		case 2: break;
		}
		break;
	} while (!cond());
	return (iterations == 3 && calls == 3) ? 0 : 1;
}
CHECK_EOF
	./tcc -B. dowhile_check.c -o dowhile_check
	./dowhile_check || { echo "tcc: do-while continue codegen is broken (issue #122)" >&2; exit 1; }

	# #208 gate: the bswaps must produce the right BYTES, and must
	# not be left undefined in a shared object -- which is the shape
	# that shipped broken twice, because an undefined symbol in a .so
	# is legal and silent.
	cat > bswap_check.c <<'BSWAP_EOF'
int main(void)
{
	if (__builtin_bswap16(0x1234u) != 0x3412u) return 1;
	if (__builtin_bswap32(0x12345678u) != 0x78563412u) return 2;
	if (__builtin_bswap64(0x0123456789abcdefULL) != 0xefcdab8967452301ULL) return 3;
	return 0;
}
BSWAP_EOF
	./tcc -B. bswap_check.c -o bswap_check
	./bswap_check || { echo "tcc: __builtin_bswap* are wrong or missing (#208)" >&2; exit 1; }
	cat > bswap_so.c <<'BSWAPSO_EOF'
unsigned int swap(unsigned int v) { return __builtin_bswap32(v); }
BSWAPSO_EOF
	./tcc -B. -shared bswap_so.c -o libbswap_so.so
	if nm -D --undefined-only libbswap_so.so 2>/dev/null | grep -q bswap32; then
		echo "tcc: __builtin_bswap32 is still undefined in a shared object (#208)" >&2
		exit 1
	fi


	# #211 regression gate. Compiles AND RUNS, and checks VALUES.
	# Case D is the one a naive fix breaks -- a parenthesised scalar
	# under brace elision must stay two elements, not four.
	cat > initlit_check.c <<'INITLIT_EOF'
struct o { int a; int b; };
enum { I0, I1, I2 };
static struct o A[] = { (struct o){.a=1,.b=2}, (struct o){.a=3,.b=4}, (struct o){.a=5,.b=6} };
static struct o B[] = { [I2]=(struct o){.a=30,.b=31}, [I0]=(struct o){.a=10,.b=11}, [I1]=(struct o){.a=20,.b=21} };
static struct o C[] = { 1,2, 3,4, 5,6 };
static struct o D[] = { (1),2, (3),4 };
static int G[][2] = { 1,2, 3,4, 5,6 };
static char H[] = "hello";
int main(void)
{
	if (sizeof A / sizeof A[0] != 3) return 1;
	if (A[0].a != 1 || A[1].a != 3 || A[2].b != 6) return 2;
	if (sizeof B / sizeof B[0] != 3) return 3;
	if (B[0].a != 10 || B[1].a != 20 || B[2].a != 30) return 4;
	if (sizeof C / sizeof C[0] != 3) return 5;
	if (C[0].a != 1 || C[1].a != 3 || C[2].b != 6) return 6;
	if (sizeof D / sizeof D[0] != 2) return 7;
	if (D[0].a != 1 || D[0].b != 2 || D[1].a != 3 || D[1].b != 4) return 8;
	if (sizeof G / sizeof G[0] != 3) return 9;
	if (G[0][0] != 1 || G[1][1] != 4 || G[2][0] != 5) return 10;
	if (sizeof H != 6 || H[0] != 'h' || H[4] != 'o') return 11;
	{
		struct o v[] = { (struct o){.a=42,.b=43}, (struct o){.a=44,.b=45} };
		if (v[1].a != 44) return 12;
	}
	return 0;
}
INITLIT_EOF
	./tcc -B. initlit_check.c -o initlit_check
	./initlit_check || {
		echo "tcc: designated compound literal initializers are broken (issue #211, case $?)" >&2
		exit 1
	}

	# A library that lives in /lib/<triplet> is actually FOUND.
	#
	# The config.h check above proves the path was recorded; this
	# proves the compiler uses it. Built and linked here rather than
	# relying on a real package, so the gate has no dependency of its
	# own -- openssl is not among this recipe's declared build tools
	# and should not have to be.
	#
	# PT_GNU_STACK gate (#228). The header must be present AND
	# non-executable -- an RWE stack would be worse than none.
	# Asserted on a real shared object built by the compiler that is
	# about to ship, not on the source patch, because the patch
	# applying is not the same claim as the header appearing.
	#
	printf 'int cix_stack_probe(void){return 7;}\n' > stackprobe.c
	./tcc -B. -shared stackprobe.c -o stackprobe.so || {
		echo "tcc: could not build the PT_GNU_STACK probe library" >&2
		exit 1
	}
	stackline=$(readelf -lW stackprobe.so 2>/dev/null | grep 'GNU_STACK' || true)
	test -n "$stackline" || {
		echo "tcc: shared objects still carry no PT_GNU_STACK -- glibc will refuse to dlopen them (#228)" >&2
		exit 1
	}
	case "$stackline" in
	*E*)
		echo "tcc: PT_GNU_STACK is marked executable -- expected RW: $stackline" >&2
		exit 1
		;;
	esac
	echo "tcc: shared objects carry a non-executable PT_GNU_STACK"

	printf 'int cix_probe_sym(void){return 42;}\n' > gatelib.c
	./tcc -B. -shared gatelib.c -o "/lib/$triplet/libcixgateprobe.so"
	printf 'extern int cix_probe_sym(void);\nint main(void){return cix_probe_sym()==42?0:1;}\n' > gateuse.c
	./tcc -B. gateuse.c -lcixgateprobe -o gateuse || {
		echo "tcc: a library in /lib/$triplet is not on the search path" >&2
		rm -f "/lib/$triplet/libcixgateprobe.so"
		exit 1
	}
	rm -f "/lib/$triplet/libcixgateprobe.so"
	echo "tcc: libraries in /lib/$triplet are found"

	#
	# And ask the compiler itself what it will search, which is the
	# check that would have caught rc-8 immediately. The link test
	# above passes against the build tree; this asserts the setting is
	# really compiled in.
	#
	./tcc -B. -print-search-dirs > searchdirs.txt 2>&1 || true
	grep -q "^ *[^ ]*/lib/$triplet\$" searchdirs.txt || grep -qE "(^| )/lib/$triplet( |\$)" searchdirs.txt || {
		echo "tcc: /lib/$triplet is not in the compiler's own reported search path" >&2
		cat searchdirs.txt >&2
		exit 1
	}

	# #216 regression gate -- the reason this upgrade exists.
	#
	# Two simultaneously-live locals must not share a stack slot when
	# a case label of the enclosing switch sits inside the block that
	# declares them. Checked two ways, because either alone can pass
	# for the wrong reason: the value must survive (which is how perl
	# died), and the addresses must actually differ (which is the
	# defect itself, and is true even where no value happens to be
	# clobbered).
	#
	# Spelled (void *)0 rather than NULL deliberately: these gate
	# sources include no headers, so that they test the compiler and
	# not the header search path.
	cat > slotalias_check.c <<'SLOT_EOF'
void *opaque(int);
int   pick(void);

static int value_survives(int which)
{
	switch (which) {
	case 0:
		return -1;
	{
		void *p;
		void *q;
	case 1:
		if (pick())
			p = opaque(1);
		else
			p = opaque(2);
		q = (void *)0;
		if (p == (void *)0)
			return 1;               /* p was clobbered by q */
		if (q != (void *)0)
			return 2;
		return 0;
	}
	}
	return 3;
}

static int addresses_differ(int which)
{
	switch (which) {
	case 0:
		return -1;
	{
		void *p;
		void *q;
	case 1:
		if (pick()) p = (void *)1; else p = (void *)2;
		q = (void *)0;
		if ((void *)&p == (void *)&q)
			return 1;               /* one slot for two live locals */
		return (p != (void *)0 && q == (void *)0) ? 0 : 2;
	}
	}
	return 3;
}

void *opaque(int n) { static char buf[8]; return buf + (n & 1); }
int   pick(void)    { return 1; }

int main(void)
{
	if (value_survives(1) != 0)
		return 1;
	if (addresses_differ(1) != 0)
		return 2;
	return 0;
}
SLOT_EOF
	./tcc -B. slotalias_check.c -o slotalias_check
	./slotalias_check || {
		echo "tcc: two live locals share a stack slot (issue #216, case $?)" >&2
		exit 1
	}

	#
	# ---- three-stage bootstrap ----
	#
	# Everything above built ONE compiler, using whatever tcc happened
	# to be installed. That is a real gap in an argument about
	# correctness: this package is its own build dependency, so a
	# compiler that miscompiles compilers could produce a working
	# binary that then builds subtly wrong ones. Requiring two
	# independently-produced builds to be byte-identical closes it --
	# and it matters more on this upgrade than on any before it, since
	# the whole point is that 0.9.27's codegen could not be trusted.
	#
	# The snapshot has to be SELF-CONTAINED, because the very next
	# thing that happens to the build tree is `make clean`.
	#
	# 0.9.27-14's version pointed the wrapper's -B at $(pwd), the
	# build directory -- so stage 3 asked stage 2 to find libtcc1.a in
	# a directory whose libtcc1.a had just been deleted, and the build
	# died with "file 'libtcc1.a' not found". Pointing -B at the
	# snapshot instead makes the dependency real rather than
	# incidental.
	#
	# The copies are also unconditional now. They were `2>/dev/null ||
	# true`, which is precisely how a snapshot missing its runtime
	# library gets built successfully and fails later somewhere less
	# obvious.
	stage_snapshot() {
		mkdir -p "$1"
		cp tcc "$1/tcc"
		cp libtcc1.a "$1/libtcc1.a"
		cp -r include "$1/include"
		cp -r lib "$1/lib"
		printf '#!/usr/bin/bash\nexec %s/tcc -B%s "$@"\n' "$1" "$1" > "$1/cc"
		chmod +x "$1/cc"
		# Prove the snapshot can compile on its own before the build
		# tree is cleaned out from under it -- otherwise the first
		# symptom is a confusing failure three steps later.
		printf 'int main(void){return 0;}\n' > "$1/selftest.c"
		"$1/cc" "$1/selftest.c" -o "$1/selftest" || {
			echo "tcc: the stage snapshot in $1 cannot compile a trivial program" >&2
			exit 1
		}
		"$1/selftest" || {
			echo "tcc: the stage snapshot in $1 produced a broken binary" >&2
			exit 1
		}
	}

	# THREE builds, not two -- and this upgrade is what proved the
	# difference matters.
	#
	# The property worth having is the one GCC's own bootstrap has:
	# two compilers built from the SAME source by compilers that were
	# themselves built from that same source must come out identical.
	#
	# 0.9.27-14 compared only two builds: one produced by the
	# already-installed tcc, and one produced by that. Those are
	# compiled by DIFFERENT compilers -- the old release and the new
	# source -- so they are only identical when the two generate
	# identical code. Every previous revision changed codegen barely
	# or not at all, so it held, and the gate looked sound. On this
	# upgrade the seed is a 2017 release and the result is a 2026
	# snapshot: stage 2 came out 475132 bytes and stage 3 440892, and
	# the gate reported that the compiler does not reproduce itself.
	# It does. The comparison was wrong, not the compiler.
	#
	# So: stage 1 is this source built by whatever tcc is installed.
	# Stage 2 is this source built by stage 1. Stage 3 is this source
	# built by stage 2. Stages 2 and 3 are both compiled by compilers
	# built from this source, so they must match byte for byte, and
	# the seed compiler drops out of the result entirely.
	stage_snapshot /run/tcc-stage1

	make clean
	./configure --cc=/run/tcc-stage1/cc --prefix=/usr --libpaths="$tcc_libpaths"
	make -j"$(nproc)"
	stage_snapshot /run/tcc-stage2

	make clean
	./configure --cc=/run/tcc-stage2/cc --prefix=/usr --libpaths="$tcc_libpaths"
	make -j"$(nproc)"

	# sha256sum rather than cmp: cmp lives in diffutils, which this
	# recipe does not declare, and 0.9.27-13 failed for exactly that
	# reason. It failed CLOSED -- unable to compare was treated as
	# "differ" -- which is the right way round for a gate, but it is
	# still a gate failing on its own missing tool rather than on the
	# thing it checks.
	s2sum=$(sha256sum < /run/tcc-stage2/tcc | cut -d' ' -f1)
	s3sum=$(sha256sum < tcc | cut -d' ' -f1)
	echo "bootstrap: stage 2 sha256 $s2sum"
	echo "bootstrap: stage 3 sha256 $s3sum"
	if [ "$s2sum" != "$s3sum" ]; then
		echo "tcc: stage 2 and stage 3 differ -- the compiler does not reproduce itself" >&2
		ls -l /run/tcc-stage2/tcc tcc >&2
		exit 1
	fi
	echo "bootstrap: stage 2 and stage 3 are byte-identical ($(stat -c%s tcc) bytes)"

	# Re-run the codegen gates against stage 3, since stage 3 is what
	# ships. They are identical binaries, so this cannot fail on its
	# own -- it fails only if the comparison above was somehow wrong,
	# which is worth a second to rule out.
	./tcc -B. dowhile_check.c -o dowhile_check3
	./dowhile_check3 || { echo "tcc: stage 3 fails the do-while gate (issue #122)" >&2; exit 1; }
	./tcc -B. initlit_check.c -o initlit_check3
	./initlit_check3 || { echo "tcc: stage 3 fails the initializer gate (issue #211)" >&2; exit 1; }
	./tcc -B. slotalias_check.c -o slotalias_check3
	./slotalias_check3 || { echo "tcc: stage 3 fails the stack-slot gate (issue #216)" >&2; exit 1; }
	./tcc -B. stdatomic_check.c -o stdatomic_check3
	./stdatomic_check3 || { echo "tcc: stage 3 fails the C11 atomics gate (#212)" >&2; exit 1; }
	# #235 gate again, on the compiler that will actually be installed.
	# Stage 3 is the binary that ships, so it answers the same
	# question independently rather than inheriting the first
	# build's answer.
	./tcc -B. c11_check.c -o c11_check3
	./c11_check3 || { echo "tcc: stage 3 fails the C11 reporting gate (#235)" >&2; exit 1; }
	./tcc -B. ctorprio_check.c -o ctorprio_check3
	./ctorprio_check3 || { echo "tcc: stage 3 fails the #219 constructor-priority gate" >&2; exit 1; }

	# #227 again on the compiler that ships: three functions must
	# still produce three distinct FDE start addends.
	./tcc -B. -c fde_check.c -o fde_check3.o
	readelf -rW fde_check3.o > fde_check3.rel 2>/dev/null || true
	fde3_addends=$(awk '/Relocation section .\.rela\.eh_frame/ {f=1; next}
	                    /^Relocation section/ {f=0}
	                    f && $1 ~ /^[0-9a-f]+$/ {print $NF}' fde_check3.rel)
	fde3_n=$(printf '%s\n' "$fde3_addends" | grep -c . || true)
	fde3_d=$(printf '%s\n' "$fde3_addends" | sort -u | grep -c . || true)
	if [ "$fde3_n" -lt 3 ] || [ "$fde3_n" != "$fde3_d" ]; then
		echo "tcc: stage 3 FDEs share start addresses (#227)" >&2
		sed 's/^/  /' fde_check3.rel >&2
		exit 1
	fi
	echo "tcc: stage 3 #227 FDE relocations ok ($fde3_n entries, $fde3_d distinct)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
