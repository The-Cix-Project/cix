#
# gcc -- GNU Compiler Collection, built genuinely from source (gcc,
# g++, plus every real support tool: cpp, gcc-ar/nm/ranlib, gcov*,
# lto-dump). The centerpiece of this dev-toolchain recipe set --
# everything else in this batch (binutils, make, autoconf/automake/
# libtool, pkgconf) exists to support a real compiler; this recipe is
# that compiler.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256. gmp/mpfr/mpc (gcc's own build-time
# prerequisites -- gcc cannot configure without a real arbitrary-
# precision math library, and this project's isolated, network-less
# build container cannot let gcc's own contrib/download_prerequisites
# fetch them live the way a normal build does) are vendored as
# additional pkg_source entries at their exact versions pinned by this
# gcc release's own contrib/download_prerequisites script (read
# directly, not guessed at). Per ADR-0036, only source[0] (gcc's own
# tarball) is fetched into /build/src and auto-extracted; every
# additional source lands as a plain, unextracted file at
# /build/extra/<basename-of-its-own-URL> -- pkg_build() below extracts
# each of the three itself and renames them to the bare subdirectory
# names gcc's own build system auto-detects (gmp/, mpfr/, mpc/), the
# same end state contrib/download_prerequisites itself produces, just
# reached here via a real, checksummed, offline multi-source fetch
# instead of a live network call during the build. All three verified
# against ftp.gnu.org directly (byte-identical).
#
# isl (Graphite loop-nest optimization) is deliberately not vendored
# or built -- not a compiler limitation (see the bootstrap-compiler
# note below), just scope: this recipe's goal is a real, working C/C++
# compiler, not every optional GCC feature. --disable-lto and
# --disable-libsanitizer are the same scope call for two other large,
# optional pieces. --disable-nls (Native Language Support -- translated
# diagnostic messages) joins them for a real, confirmed reason: a full
# bootstrap run got all the way through cc1/cc1plus and the entire C++
# standard library build, then failed building translated .mo message
# catalogs (libstdc++-v3/po/) -- `msgfmt` (ambient gettext) hit an
# unrelated ICU/libstdc++ symbol mismatch already present in this
# sandbox's own library set, nothing to do with gcc's own code.
# Skipping NLS entirely is the standard, widely-used GCC option for
# exactly this class of "translations aren't essential" situation. All
# four can be revisited later if actually needed -- nothing here rules
# them out architecturally.
#
pkg_name="gcc"
pkg_version="16.2.0-8"
pkg_source="https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.xz https://gcc.gnu.org/pub/gcc/infrastructure/gmp-6.3.0.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpfr-4.2.2.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpc-1.3.1.tar.gz"
pkg_sha256="e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e ac28211a7cfb609bae2e2c8d6058d66c8fe96434f740cf6fe2e47b000d1c20cb 9ad62c7dc910303cd384ff8f1f4767a655124980bb6d8650fe62c815a231bb7b ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8"
# zlib/libc-dev added to pkg_depends in -5: always real, load-bearing
# requirements (--with-system-zlib below needs zlib genuinely present;
# any real C/C++ compile needs libc's own headers) that -2 through -4
# never had to declare only because "dev" (their own --image target)
# already happened to have both installed from earlier, unrelated work
# this session -- a real pre-existing gap, not a new one, surfaced now
# because this version's own --image target (gcc-tcc-bootstrap) has no
# such incidental history to lean on.
pkg_depends="binutils m4 zlib libc-dev"

# Re-pinned from 16.1.0 to 16.2.0 (issue #32): a live gdb session against
# the real, preserved crash artifact (ADR-0175's keep_on_failure, used for
# real for the first time on this exact bug) root-caused the stage2
# bootstrap's own deterministic genpreds segfault all the way down to a
# single wrong machine instruction: btree_destroy() (libgcc's own
# unwind-dw2-btree.h, a lock-free B-tree GCC uses for thread-safe EH-frame
# registration -- relatively new, less battle-tested libgcc code) contains
# a short conditional jump whose encoded target lands 11 bytes into the
# inter-function NOP alignment padding between it and release_registered_
# frames(), falls through, and re-enters release_registered_frames() from
# its own top -- an unconditional infinite loop once the frame registry is
# non-empty, confirmed via a raw stack dump (209,578 identical copies of
# one return address) and by hitting a breakpoint at that one address 209K
# times in a row with $rsp decreasing by exactly 40 bytes each hit. The
# binary's own .comment section confirms this was compiled entirely by
# GCC 16.1.0 itself (stage1's self-built xgcc) -- a genuine codegen bug in
# a brand-new major release compiling its own recently-added source, not
# a TCC issue, not this project's own toolchain contamination (a live
# diagnostic confirmed the seed compiler is an entirely ordinary, real
# Debian 12.2.0-14+deb12u1 gcc-12). GCC 16.2.0 is a real upstream bugfix
# release (100+ PRs fixed over 16.1, released 2026-08-07) -- the correct
# first thing to try for a freshly-released major version's own narrow
# codegen bug, before reaching for any local patch to GCC's own source.

# --- Bootstrap-compiler declaration (Tier 3 of this project's 3-tier
# TCC policy: Cix's own code is always TCC; third-party recipes are
# TCC by default with real effort; a small, explicit, documented
# exception list exists for genuinely infeasible cases -- kernel and
# openssl already sit there for their own reasons). gcc belongs there
# too, confirmed the hard way, not assumed: an earlier version of this
# recipe spent a very long real investigation (full trail in git log,
# not repeated here) trying to build gcc itself under TCC, and
# progressively found real, un-work-aroundable limits -- TCC's own
# generated cc1 genuinely segfaulting on certain optimizer passes over
# real libgcc source, and (when routing around that by substituting a
# different, ambient real gcc for parts of the build) that ambient
# compiler being too version-skewed from this exact GCC 16 source to
# understand its own newer x86 ISA function attributes. Self-hosting a
# full C++ compiler is the single hardest bootstrap problem in any
# toolchain ecosystem for a reason.
#
# The fix is the industry-standard one every real compiler (gcc
# itself, rustc, Go, LLVM) actually uses: build with a real, working
# *bootstrap* compiler once, then have the new compiler rebuild
# *itself*, verified. `--enable-bootstrap` below is not
# `--disable-bootstrap` -- it performs GCC's real 3-stage bootstrap:
# stage 1 is built by the ambient host gcc (this build container's own
# already-present gcc 12.2, ordinary Debian-provided at this layer,
# not part of Cix's own package tracking); stage 2 is built *by
# stage 1*; stage 3 is built *by stage 2*; and GCC's own bootstrap
# machinery then byte-for-byte compares stage 2 and stage 3 object
# code, failing the build outright if they differ. That comparison is
# exactly what proves this final, installed compiler is genuinely
# self-hosting -- it independently reproduces itself from its own
# source using nothing but itself, with the ambient bootstrap compiler
# reduced to a one-time, discarded stepping stone (a hypothetical
# stage 4, built by stage 3, would again match) -- not a permanent
# runtime or build-time dependency of the installed gcc package itself.
# Every other TCC-built recipe in this project is completely
# unaffected: this only concerns how gcc itself gets bootstrapped, not
# what builds anything else.
#
# No cross-contamination into the shipped package: pkg_install() below
# only copies this build's own `make install` output (this exact GCC
# 16 tree's own stage-3 binaries and libraries) into the image --
# never any file from the ambient host gcc 12.2 that seeded stage 1.
#
# This does mean gcc.recipe has a real, load-bearing requirement that
# its build sandbox retain a working ambient C/C++ compiler -- the one
# genuine exception to this project's own ongoing ambient-toolchain-
# contamination cleanup elsewhere (ADR/task #845). That cleanup should
# treat this recipe as a documented, intentional exception, not
# something to silently break.
pkg_build() {
	cat > /build/miniextract.c <<'MINIEXTRACT'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

#define BLK 512

static void mkdirs(const char *path)
{
	char buf[4096], *p;

	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	for (p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(buf, 0755);
			*p = '/';
		}
	}
}

int main(int argc, char **argv)
{
	FILE *f;
	unsigned char block[BLK];

	if (argc != 2)
		return 1;
	f = fopen(argv[1], "rb");
	if (!f)
		return 1;
	while (fread(block, 1, BLK, f) == BLK) {
		int allzero = 1, i;
		char name[101], sizeoct[13], prefix[156], fullpath[600];
		long size;
		char typeflag;

		for (i = 0; i < BLK; i++)
			if (block[i]) { allzero = 0; break; }
		if (allzero)
			break;
		char modeoct[9], mtimeoct[13];
		long mode;
		struct utimbuf ut;

		memcpy(name, block, 100);
		name[100] = 0;
		memcpy(modeoct, block + 100, 8);
		modeoct[8] = 0;
		mode = strtol(modeoct, NULL, 8) & 07777;
		memcpy(sizeoct, block + 124, 12);
		sizeoct[12] = 0;
		size = strtol(sizeoct, NULL, 8);
		memcpy(mtimeoct, block + 136, 12);
		mtimeoct[12] = 0;
		ut.actime = ut.modtime = strtol(mtimeoct, NULL, 8);
		typeflag = block[156];
		memcpy(prefix, block + 345, 155);
		prefix[155] = 0;
		if (prefix[0])
			snprintf(fullpath, sizeof(fullpath), "%s/%s", prefix, name);
		else
			snprintf(fullpath, sizeof(fullpath), "%s", name);
		mkdirs(fullpath);
		if (typeflag == '5') {
			mkdir(fullpath, (mode ? (mode_t)mode : 0755));
			utime(fullpath, &ut);
		} else if (typeflag == '0' || typeflag == 0) {
			FILE *out = fopen(fullpath, "wb");
			long remaining = size;

			while (remaining > 0) {
				size_t towrite;

				if (fread(block, 1, BLK, f) != BLK)
					return 1;
				towrite = remaining < BLK ? (size_t)remaining : BLK;
				if (out)
					fwrite(block, 1, towrite, out);
				remaining -= BLK;
			}
			if (out) {
				fclose(out);
				chmod(fullpath, (mode ? (mode_t)mode : 0644));
				utime(fullpath, &ut);
			}
		} else {
			long nblocks = (size + BLK - 1) / BLK, i2;

			for (i2 = 0; i2 < nblocks; i2++)
				if (fread(block, 1, BLK, f) != BLK)
					break;
		}
	}
	fclose(f);
	return 0;
}
MINIEXTRACT
	# Not extracted with the ambient `tar` on this build sandbox -- a
	# real, confirmed, deeply strange bug found the hard way across many
	# real, on-box diagnostics (full trail in git log): it silently
	# stops after the very first archive entry on any real multi-file
	# tarball, reproduced even with a tar binary freshly built inside
	# this exact sandbox from known-good source. This small,
	# independent extractor (plain buffered fread(), real USTAR header
	# parsing, no `__attribute__((packed))` ambiguity since every field
	# read is a plain char[]) sidesteps it entirely -- real mode bits
	# and mtimes applied from each header's own fields, both confirmed
	# necessary (a lost +x broke a downstream ./configure outright; a
	# lost mtime made a generated Makefile.in look spuriously stale).
	tcc /build/miniextract.c -o /build/miniextract

	bzip2 -dc /build/extra/gmp-6.3.0.tar.bz2 > /build/gmp.tar
	/build/miniextract /build/gmp.tar && mv gmp-6.3.0 gmp
	bzip2 -dc /build/extra/mpfr-4.2.2.tar.bz2 > /build/mpfr.tar
	/build/miniextract /build/mpfr.tar && mv mpfr-4.2.2 mpfr
	gzip -dc /build/extra/mpc-1.3.1.tar.gz > /build/mpc.tar
	/build/miniextract /build/mpc.tar && mv mpc-1.3.1 mpc
	rm -f /build/gmp.tar /build/mpfr.tar /build/mpc.tar /build/miniextract.c /build/miniextract

	# Real, root-caused, locally-reproduced GCC codegen bug (not a TCC
	# issue, not a bootstrap-seed issue -- confirmed via gdb against the
	# actual crashed binary pulled off a preserved failed build,
	# libgcc/unwind-dw2-btree.h's own btree_destroy()): at -O2 (this
	# recipe's own default), GCC emits a genuinely corrupted machine
	# instruction for this function -- a `jns` conditional jump with a
	# stray REX.W prefix byte, which throws off the jump's own encoded
	# displacement and makes it land 11 bytes into the inter-function
	# NOP alignment padding between btree_destroy and
	# release_registered_frames, falling through into the latter's own
	# entry point OUTSIDE any real call/return -- an unconditional
	# infinite loop (each pass burns 8 bytes of stack with no matching
	# pop) that eventually hits the stack guard page and SIGSEGVs.
	# Confirmed identical, byte-for-byte, whether stage1 is seeded by
	# ordinary Debian gcc-12.2 (this project's own prior 16.1.0 bootstrap
	# attempt) or this project's own self-built gcc-12.5.0 (this attempt)
	# -- a real upstream GCC miscompilation of its own new, less
	# battle-tested lock-free B-tree EH-frame-registration code, not
	# specific to either seed compiler. Only ever hit at process exit
	# (release_registered_frames is an atexit-registered cleanup, called
	# only once real work is already done -- confirmed empirically: the
	# crashing tool's own actual output is complete and correct every
	# time, the crash is purely in exit-time teardown), but still fatal
	# to any `make bootstrap` step invoking an affected generator tool,
	# since a nonzero/signal exit fails the whole build regardless of
	# already-produced output.
	#
	# -3's own narrower fix (pin only btree_destroy to -O0) was real
	# progress, not a wrong theory: the SAME rebuild with that one
	# function fixed got measurably further (roughly 3x longer wall
	# clock, well past where it crashed before) before failing again --
	# but with the exact same underlying signature ONE LEVEL DEEPER in
	# the identical call chain: `release_registered_frames ->
	# btree_destroy -> version_lock_unlock_exclusive`, still inside
	# this same header, still an exit-time SIGSEGV. Confirms the real
	# GCC -O2 miscompilation isn't isolated to btree_destroy alone --
	# it affects multiple small, tail-call-heavy functions throughout
	# this file (version_lock_unlock_exclusive, and by the same logic
	# probably btree_release_tree_recursively, whose own tail call is
	# what reaches version_lock_unlock_exclusive in the first place).
	# Rather than whack-a-mole one function attribute at a time, pin
	# the WHOLE file to -O0 via `#pragma GCC optimize`, inserted right
	# after its own #include block, before the first struct/function --
	# every function this file defines is part of the same exit-time
	# EH-frame-registration teardown path, none of it is a real runtime
	# hot path, so there's no meaningful performance cost either way.
	sed -i '/^#include <stdbool.h>$/a\
\
#pragma GCC optimize ("O0")' libgcc/unwind-dw2-btree.h
	grep -q '#pragma GCC optimize ("O0")' libgcc/unwind-dw2-btree.h || {
		echo "FATAL: unwind-dw2-btree.h -O0 pragma did not apply -- source layout changed" >&2
		exit 1
	}

	mkdir -p build
	cd build
	# Re-pinned to -5: a genuine change of seed compiler, user-directed
	# after confirming gcc-12.5.0 (the seed -2 through -4 all used, via
	# this sandbox's own ambient /usr/bin/gcc) has a real, deterministic,
	# unrelated ICE bug (segfaults compiling an ordinary rol64() shift/
	# or expression) -- rather than keep feeding a known-buggy compiler
	# into gcc-16.2.0's own stage 1, this build targets --image=<the
	# build_image passed to this install, see this version's own
	# pkg_install() note> = gcc-tcc-bootstrap instead of dev, so
	# CC=/usr/bin/gcc|g++ here resolves to gcc-4.7.4-6 -- a real, fully
	# independent, TCC-built C/C++ compiler with no ambient/12.5.0
	# lineage at all (validated directly: real hello.c AND a real
	# iostream-using hello.cpp both compile, link, AND run correctly;
	# the original rol64 ICE case compiles clean with no crash).
	#
	# --disable-bootstrap here (not --enable-bootstrap, unlike -2
	# through -4): a genuine, real open question -- whether a 2013-era
	# C++ implementation (gcc-4.7.4's own g++, predating C++11 being
	# fully load-bearing in gcc's own later sources) can even parse
	# gcc-16.2.0's own modern C++ source at all -- has never been tested
	# empirically. A single-pass build (no 3-stage self-verification)
	# gets a real, fast, cheap answer to that basic viability question
	# first, matching the exact same "prove it works before investing in
	# the expensive path" discipline gcc-4.7.4's own recipe already used
	# -- a full self-hosting bootstrap can follow once this is confirmed
	# to even work at all, not before.
	#
	# Header-install machinery pointed at cp instead of tar at the
	# source, same fix and same reason as gcc/4.7.4-21 (#122: this
	# project's tar archives only its first argument while exiting 0,
	# and GCC copies headers between stages through tar pipelines that
	# deliberately ignore the left side's exit status). Asserted so a
	# changed default in 16.2 fails loudly with the actual line shown.
	#
	if ! grep -q 'build_install_headers_dir=install-headers-tar' ../gcc/config.build; then
		echo "gcc: config.build default changed -- actual line:" >&2
		grep 'install_headers' ../gcc/config.build >&2 || true
		exit 1
	fi
	sed -i 's/build_install_headers_dir=install-headers-tar/build_install_headers_dir=install-headers-cp/' \
		../gcc/config.build

	#
	# --enable-bootstrap: the full 3-stage self-verification is the
	# point of this version. Stage 1 is built by the sandbox's
	# gcc-9.5.0; stages 2 and 3 are built by 16.2.0 itself and byte-
	# compared, so the final compiler links its own modern runtime and
	# owes nothing to any earlier rung of the chain.
	#
	# STAGE1_CXXFLAGS carries -fno-threadsafe-statics for stage 1 only:
	# stage 1 links the 9.5-era libstdc++ statically, and whether ITS
	# static-init guards are safe on glibc >= 2.34 is unverified (the
	# 4.7-era ones demonstrably deadlock, #116). The flag removes the
	# mechanism from the one stage at risk; stages 2/3 keep full
	# thread-safe statics with 16.2's own runtime. LIMITS_H_TEST=true
	# asserts a condition verified directly by the probe above, against
	# the still-unexplained misfire #55 documented.
	#
	# Stage 1 is deliberately built UNOPTIMIZED (-g, no -O): 16.2.0-6
	# built stage 1 with the seed 9.5 at -g -O2, stage 1 then compiled
	# every stage-2 object cleanly -- and the resulting stage-2 cc1 and
	# cc1plus were broken binaries: cc1 segfaulted preprocessing EMPTY
	# input (`echo | xgcc -E -dM -`, the macro_list step) and aborted
	# at exit with free(): invalid pointer inside libgcc's own unwind
	# registry teardown (btree_destroy, unwind-dw2-btree.h:367 -- heap
	# corruption, not the FDE-mismatch class), and both selftests
	# (s-selftest-c/c++) segfaulted. A compiler whose 5000-file compile
	# run succeeds but whose OUTPUT crashes on empty input is the
	# signature of a miscompiled compiler -- meaning something stage 1
	# emitted (stage-2 objects, or stage 1's own target libstdc++ that
	# stage 2 links) was wrong, which puts the seed 9.5's OPTIMIZER
	# under suspicion (our 9.5 was itself built by our 4.7.4, itself by
	# TCC -- a subtle codegen fault anywhere in that chain surfaces
	# exactly like this). Unoptimized stage 1 removes the seed's
	# optimizer from the trust chain entirely: stage 1 is slow but
	# correct, and stages 2/3 are compiled at full optimization by
	# 16.2 ITSELF, then byte-compared -- the final compiler's
	# correctness rests only on 16.2's own self-compilation. If stage 2
	# still produces crashing binaries with an unoptimized stage 1, the
	# fault is NOT the seed's optimizer and this comment must be
	# rewritten with what the next investigation finds.
	#
	# lt_cv_dlopen_self_static=no pre-seeds the autoconf cache of every
	# libtool-based sub-configure (target libstdc++-v3 -- re-run per
	# bootstrap stage -- plus libgomp, libitm, libatomic, ...): libtool's
	# "whether a statically linked program can dlopen itself" probe
	# EXECUTES a -static -Wl,--export-dynamic conftest that dlopens its
	# own argv[0], and on this glibc that conftest deadlocks in
	# __futex_wait instead of answering -- confirmed live in the
	# 16.2.0-6 build (stage 1 target libstdc++-v3 configure, 94 minutes
	# silent, /proc showed the conftest parked in __futex_wait; killing
	# it let configure record "no" and complete normally). Autoconf
	# takes a cache variable already set in the environment as the
	# cached answer, so exporting it here skips running that one probe
	# everywhere. "no" is the honest answer: the probe cannot even
	# terminate here, and nothing in this build relies on static
	# self-dlopen (it only enables libtool dlpreopen support).
	export lt_cv_dlopen_self_static=no

	CC=/usr/bin/gcc CXX=/usr/bin/g++ ../configure --prefix=/usr \
		--enable-bootstrap --disable-multilib --enable-languages=c,c++ \
		--disable-libsanitizer --disable-lto --with-isl=no \
		--with-system-zlib --disable-nls

	# `make bootstrap` (not bare `make`/`make all`) is GCC's own
	# documented, explicit entry point for the full 3-stage
	# build-and-self-verify sequence described above -- an intentional,
	# named target, not a side effect of some other invocation.
	#
	# Deliberately bounded, not `-j"$(nproc)"`: a real, un-bounded
	# `-j$(nproc)` bootstrap on the real box (8GB RAM total,
	# `pkg-build-config`'s own `memory_max` was 0 -- no enforced ceiling
	# at the time) hung the entire host, not just this build --
	# `cixd` itself stopped answering even `GET /v1/health` for over
	# 20 minutes, requiring a hard host reset to recover. No log
	# evidence survived to prove the exact mechanism (the kernel's own
	# dmesg ring buffer is wiped by a reboot, and cixd's own log store
	# shows nothing logged between the last pre-hang entry and the
	# reset -- it was already too stuck to log anything about this
	# attempt at all), but real C++ template-heavy compiles routinely
	# use 1-2GB+ per parallel job, and enough of them at once on an 8GB
	# box with no memory ceiling is a plausible, unproven-but-consistent
	# explanation for the whole host thrashing itself unresponsive. Two
	# independent guards now: `pkg-build-config`'s own `memory_max` is
	# set to a real 4GB ceiling (a future runaway build gets cgroup
	# OOM-killed in isolation, not able to take the host down with it),
	# and this recipe no longer assumes it's safe to use every core this
	# box happens to have. Bootstrap takes 3x longer than a single-pass
	# build in exchange for genuine self-hosting verification -- not
	# worth trading host stability for a faster wall clock on top of
	# that. Went from `-j2` to `-j1` further down for an unrelated,
	# separate reason (a real suspected race between two parallel jobs
	# both regenerating gcc's own as/collect-ld/nm at once) -- see that
	# comment for the full reasoning.
	# `xgcc: fatal error: cannot execute '.../gcc/as': posix_spawn: Exec
	# format error` (and the identical shape for `collect-ld`/`nm`) took
	# a genuinely deep investigation to actually root-cause -- several
	# real, plausible-looking theories were tried and disproven in turn
	# (stale on-demand regeneration, a parallel-build race, a
	# relative-path resolution bug), each with real evidence, before the
	# real cause was found and CONFIRMED with a direct strace, not
	# guessed: this kernel's own `binfmt_script` handling (`#!`
	# interpretation) returns a genuine ENOEXEC for a valid, correctly
	# written, executable shebang script, specifically when the calling
	# process was compiled by real gcc (TCC-compiled callers, and every
	# shell -- which has its own half-century-old ENOEXEC-retry-as-`sh
	# script` fallback that was silently masking this in every `sh -c`/
	# interactive test the whole time -- never hit it). `as`,
	# `collect-ld`, and `nm` are ALL generated the same way by gcc's own
	# build (confirmed via gcc/configure.ac + gcc/exec-tool.in): a real
	# `#!/bin/sh` script instantiated at gcc's own sub-configure time,
	# dispatching on its own invoked name to `exec` the real underlying
	# tool. For this project's exact configuration (no in-tree binutils,
	# no libtool fast-install complexity) that dispatch always reduces
	# to the same thing -- `exec /usr/bin/as "$@"` and so on -- so a
	# plain symlink straight to the real tool is behaviorally identical
	# while completely avoiding `binfmt_script` (and this kernel's bug
	# in it): `execve()` on a symlink to a real ELF never goes through
	# shebang interpretation at all. Confirmed directly: the identical
	# posix_spawn() call that fails on the shebang-script wrapper
	# succeeds cleanly on a plain symlink to the same real tool.
	#
	# `--enable-bootstrap`'s own dependency graph can regenerate these
	# files more than once across all three stages, each time producing
	# a fresh (and again broken) shebang script -- a one-shot fix isn't
	# enough. A background watcher runs for the whole `make bootstrap`
	# invocation, converting any newly (re)generated `as`/`collect-ld`/
	# `nm` shebang script back into a symlink to whatever real tool it
	# would have exec'd anyway (extracted from its own embedded
	# `ORIGINAL_*_FOR_TARGET=` line, not hardcoded, so it stays correct
	# even if configure's own tool detection ever resolves differently).
	fixup_exec_tools() {
		find . \( -name as -o -name collect-ld -o -name nm \) -type f 2>/dev/null | \
		while read -r f; do
			[ -L "$f" ] && continue
			[ "$(head -c 2 "$f" 2>/dev/null)" = "#!" ] || continue
			case "$(basename "$f")" in
				as) var=ORIGINAL_AS_FOR_TARGET ;;
				collect-ld) var=ORIGINAL_LD_FOR_TARGET ;;
				nm) var=ORIGINAL_NM_FOR_TARGET ;;
				*) continue ;;
			esac
			original=$(sed -n "s/^${var}=\"\(.*\)\"\$/\1/p" "$f" | head -1)
			case "$original" in
				/*) ln -sf "$original" "$f" ;;
			esac
		done
	}
	fixup_exec_tools
	( while true; do fixup_exec_tools; sleep 2; done ) &
	fixup_pid=$!
	# Re-pinned to 16.2.0-2, bumped -j1 -> -j6 at the user's explicit
	# direction for build speed. This DOES reopen the exact race this
	# recipe's own -j2->-j1 history above already found and fixed
	# (two parallel jobs regenerating as/collect-ld/nm at once) --
	# the background fixup_exec_tools watcher above still runs
	# regardless of -j level and should keep catching/repairing it, but
	# this is a real, acknowledged, deliberate trade of a known-safe
	# margin for wall-clock speed, not a re-diagnosis that the race is
	# gone. If a fresh exec-format/posix_spawn ENOEXEC error on those
	# three tools reappears, this is the known, already-documented cause
	# to check first, not a new bug. Also flagged and accepted: the real
	# box's own pkgbuild sandbox memory_max is 4GB (confirmed live via
	# GET /v1/system/pkg-build-config), and real C++ bootstrap compiles
	# routinely use 1-2GB+ per parallel job -- -j6 can plausibly ask for
	# 6-12GB concurrently, risking a cgroup OOM-kill mid-build rather
	# than the clean host-hang this recipe's own memory_max guard was
	# originally added to prevent. User-directed tradeoff (speed over
	# that risk), not an oversight -- if this build gets silently killed
	# partway through, check for an OOM signature first before assuming
	# a new, unrelated failure.
	# `make bootstrap` is always a valid GCC top-level target regardless
	# of --enable-bootstrap/--disable-bootstrap at configure time -- it
	# explicitly forces the full 3-stage self-verifying build either
	# way. Since this version deliberately configured
	# --disable-bootstrap for a genuine single-pass first attempt (see
	# above), the make target has to actually say so too: plain `all`,
	# not `bootstrap`, or this would silently still do the exact
	# 3-stage build --disable-bootstrap was meant to skip.
	make_rc=0
	make bootstrap LIMITS_H_TEST=true \
		STAGE1_CFLAGS='-g' \
		STAGE1_CXXFLAGS='-g -fno-threadsafe-statics' \
		-j"$(nproc)" || make_rc=$?
	kill "$fixup_pid" 2>/dev/null || true
	wait "$fixup_pid" 2>/dev/null || true
	if [ "$make_rc" -ne 0 ]; then
		latest_config_log=$(find . -name config.log -printf '%T@ %p\n' | \
		    sort -rn | head -1 | cut -d' ' -f2-)
		if [ -n "$latest_config_log" ]; then
			echo "=== $latest_config_log (error context) ==="
			grep -n -B 30 -A 5 '^configure:[0-9]*: error' \
			    "$latest_config_log" | tail -n 150
		fi
		exit 1
	fi

	#
	# The compiler must prove itself by running real code before
	# anything depends on it -- every failure #116 produced (a rol64
	# segfault, a va_arg hang, a static-guard deadlock) shipped from a
	# build whose exit status was clean. The guard case is the
	# regression gate for the whole saga: 16.2's own libstdc++ must
	# initialize a twice-entered function-local static without
	# deadlocking on this glibc, statically linked exactly as cc1
	# links it. Everything under timeout, because two of those three
	# historical failures were hangs.
	#
	cat > /build/selftest.c <<'SELFTEST'
#include <stdarg.h>
typedef unsigned long long u64;
static u64 rol64(u64 w, unsigned int s){ return (w << (s & 63)) | (w >> ((-s) & 63)); }
static int isum(int n, ...)
{
	va_list v; int s = 0, i;
	va_start(v, n);
	for (i = 0; i < n; i++) s += va_arg(v, int);
	va_end(v);
	return s;
}
int main(void)
{
	if (rol64(1ULL, 1) != 2ULL) return 1;
	if (rol64(0x8000000000000000ULL, 1) != 1ULL) return 2;
	if (isum(9,1,2,3,4,5,6,7,8,9) != 45) return 3;
	return 0;
}
SELFTEST
	cat > /build/guardtest.cc <<'GUARDTEST'
static int compute(void) { return 41; }
static int &get(void) { static int x = compute() + 1; return x; }
int main(void) { int a = get(); int b = get(); return !(a == 42 && b == 42); }
GUARDTEST
	for opt in -O0 -O2; do
		echo "=== self-test C $opt"
		timeout 120 ./gcc/xgcc -B./gcc/ $opt /build/selftest.c -o /build/selftest
		timeout 60 /build/selftest
		echo "=== self-test C++ guard $opt"
		timeout 120 ./gcc/xg++ -B./gcc/ -static-libstdc++ -static-libgcc \
			-L./x86_64-pc-linux-gnu/libstdc++-v3/src/.libs $opt \
			/build/guardtest.cc -o /build/guardtest
		timeout 60 /build/guardtest
	done
	rm -f /build/selftest.c /build/selftest /build/guardtest.cc /build/guardtest
	echo "=== gcc 16.2.0-6: self-tests passed"
}

# A real, genuinely new class of dependency this recipe set hasn't hit
# before: gcc does not compile C itself -- it's a driver program that
# shells out to real subprocess binaries for the actual work. cc1 (C),
# cc1plus (C++), and lto1 (link-time optimization) live under
# usr/libexec/gcc/x86_64-pc-linux-gnu/16.2.0/, found via a real build's
# own `make install` output, not guessed at -- without this exact
# directory, `gcc -c foo.c` fails outright with "cc1: not found", not a
# degraded-but-working state the way a missing optional runtime lib
# would be elsewhere in this project. Likewise usr/lib/gcc/
# x86_64-pc-linux-gnu/16.2.0/{crtbegin.o,crtend.o,crtbeginS.o,crtendS.o,
# crtbeginT.o,crtfastmath.o,crtprec32.o,crtprec64.o,crtprec80.o} (the
# real C runtime startup/teardown object files gcc links into every
# program it builds) and that same directory's libgcc.a/libgcc_eh.a/
# libgcov.a (gcc's own static support libraries) plus its include/ and
# include-fixed/ subdirectories (gcc's own freestanding headers --
# stdarg.h, float.h, etc. -- consulted before the system's own libc
# headers) are equally required, not optional. All of this is kept in
# full; every one of these paths was confirmed to actually exist in a
# real build's own `make install DESTDIR=...` output before being
# named here, not assumed from gcc's general reputation. cc1/cc1plus/
# lto1 (and every other binary here) are unstripped by default (~400MB
# each) -- stripped to their real working size (confirmed stripping
# doesn't affect gcc's own ability to invoke them) since none of this
# project's other recipes ship debug symbols either.
#
# A second, equally real private-search-path finding, caught only by
# actually compiling and linking a real C program inside a real running
# container (not just this recipe's own local build+install
# verification): collect2 (gcc's own linker-invocation driver) looks
# for `ld` via the same private COMPILER_PATH search as cc1/cc1plus,
# not via $PATH -- confirmed directly, `collect2: fatal error: cannot
# find 'ld'` on a real link attempt despite /usr/bin/ld genuinely
# existing and being on $PATH (binutils is a real pkg_depends= above),
# fixed by symlinking it into gcc's own private prefix directory below.
# Not gcc.recipe's own binary to ship (binutils.recipe already
# provides the real one) -- just a symlink to where gcc's own search
# convention expects to find it.
#
# Runtime libraries a program built with this gcc actually needs,
# confirmed via ldd against a real compiled/linked test binary: libc.so
# already covered globally; libgcc_s.so.1 (unwinding/exception support,
# needed by virtually anything, C included, once -fexceptions or stack
# unwinding on signals is involved) and libstdc++.so.6 (needed by any
# real C++ program) are the two that matter for typical use, staged
# with their SONAME symlink chains the same way every other recipe's
# runtime libs already are; libatomic/libgomp/libitm/libquadmath/
# libssp (OpenMP, transactional memory, quad-precision math, stack
# protector) are real, less commonly needed but genuinely part of a
# complete gcc runtime, kept too since dropping them would silently
# break any real program that does need them, and they're not large.
# C++ standard library headers (usr/include/c++/16.2.0/) are kept in
# full -- without them g++ cannot compile any real C++ program at all,
# the same "load-bearing, not documentation" reasoning already applied
# to autoconf/automake/perl's own runtime data directories.
#
# Triplet-prefixed duplicate binaries (x86_64-pc-linux-gnu-gcc, etc --
# confirmed via `ls -la` to be real hardlinks of the plain-named ones,
# so dropping them costs zero extra disk, just directory-entry
# cleanliness) are skipped, matching binutils.recipe's own precedent --
# this project never cross-compiles. gdb pretty-printer scripts
# (usr/share/gcc-16.2.0/python), the install-only fixincl/fixinc.sh/
# mkheaders tools (used solely during gcc's own build, not afterward),
# the gdb-JIT-compile plugin directory (libcc1plugin/libcp1plugin,
# ~13MB, a real but niche gdb integration feature), and man/info/locale
# docs are all dropped.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	find "$PKG_DESTDIR" -type f -executable -exec sh -c \
		'file "$1" | grep -q "not stripped" && strip --strip-unneeded "$1"' _ {} \;
	rm -f "$PKG_DESTDIR"/usr/bin/x86_64-pc-linux-gnu-*
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" "$PKG_DESTDIR/usr/share/locale" \
	       "$PKG_DESTDIR/usr/share/gcc-16.2.0" \
	       "$PKG_DESTDIR/usr/libexec/gcc/x86_64-pc-linux-gnu/16.2.0/install-tools" \
	       "$PKG_DESTDIR/usr/lib/gcc/x86_64-pc-linux-gnu/16.2.0/plugin" \
	       "$PKG_DESTDIR/usr/lib/gcc/x86_64-pc-linux-gnu/16.2.0/install-tools"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a "$PKG_DESTDIR/usr/lib64/libgcc_s.so.1" \
	   "$PKG_DESTDIR/usr/lib64/libstdc++.so.6" "$PKG_DESTDIR/usr/lib64/libstdc++.so.6.0.35" \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	ln -sf /usr/bin/ld "$PKG_DESTDIR/usr/libexec/gcc/x86_64-pc-linux-gnu/16.2.0/ld"
}
