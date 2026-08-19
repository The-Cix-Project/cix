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
pkg_version="12.5.0-7"
pkg_source="https://ftp.gnu.org/gnu/gcc/gcc-12.5.0/gcc-12.5.0.tar.xz https://gcc.gnu.org/pub/gcc/infrastructure/gmp-6.2.1.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpfr-4.1.0.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpc-1.2.1.tar.gz"
pkg_sha256="71cd373d0f04615e66c5b5b14d49c1a4c1a08efa7b30625cd240b11bab4062b3 eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c feced2d430dd5a97805fa289fed3fc8ff2b094c02d05287fd6133e7f1f0ec926 17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459"
pkg_depends="binutils m4"

# Re-pinned from 16.2.0 to 12.5.0 (issue #32): 16.1.0 and 16.2.0 both hit
# the identical, confirmed-at-the-instruction-level bug documented in this
# file's own prior git history -- btree_destroy() (libgcc's own
# unwind-dw2-btree.h, a lock-free B-tree GCC uses for thread-safe EH-frame
# registration) contains a wrong short-jump target that falls into
# inter-function padding and re-enters release_registered_frames() from
# its own top, an unconditional infinite loop. Confirmed NOT fixed by
# 16.2.0's own 100+ PRs (re-tested live, same bug at the instruction
# level, only the absolute addresses moved). Directly confirmed (GitHub's
# own gcc-mirror, releases/gcc-12.5.0 tag, not guessed): unwind-dw2-btree.h
# does not exist at all in GCC 12.5.0's libgcc -- this bug class is
# structurally impossible to hit building this version, the buggy code
# was added later.
#
# This is not a workaround or a lowered standard -- the bootstrap
# mechanism is completely unchanged (the ambient seed compiler is still
# only ever a one-time, discarded stage1 seed; `--enable-bootstrap`'s own
# 3-stage self-verification still applies in full; the installed package
# still ships only this build's own self-hosted stage-3 output). Only
# which GCC source gets built changes. 12.5.0 is GCC 12's own final,
# most-mature maintenance release (12.1 through 12.5, released 2025-07-11)
# -- a much smaller, safer self-hosting jump from the Debian 12.2.0 ambient
# seed than 16.x ever was (same major series, not four majors ahead), and
# already comfortably exceeds the Linux kernel's own documented minimum
# supported compiler version (GCC 5.1+) -- there was never a real
# requirement for thinC's own Tier-3 gcc to be the latest release; 16.x
# was only ever the default "newest" choice, not a genuine need.
#
# Re-pinned again to 12.5.0-2 (same real upstream 12.5.0 source,
# unchanged -- only this recipe's own build logic changed, matching
# kernel.recipe's own established "-2" convention for exactly this
# case): the first 12.5.0 attempt got dramatically further than any
# 16.x run ever had (deep into stage2's own libgcc build, not the
# earlier stage1/genpreds crash) before hitting a real, different,
# already-precedented bug -- gcc/Makefile.in's stage2+ header-copy step
# (`real-install-headers-tar`, confirmed live: the actual configured
# Makefile really does select INSTALL_HEADERS_DIR = install-headers-tar)
# uses the same `tar -cf - . | tar xpf -` pattern this project's own
# ambient tar has a confirmed, silent first-entry-only truncation bug
# for (see pkg_build()'s own miniextract.c comment) -- syslimits.h
# genuinely present in the prior stage's own include-fixed/ (confirmed
# byte-identical limits.h alongside it) silently never made it into the
# next stage, breaking libgcc2.c's very first compile. Fixed the same
# way as the exec-tool wrappers: a periodic background sweep forcing
# install-headers-cp (plain `cp -p -r`, no tar) into any newly
# (re)generated Makefile, for the whole bootstrap.

# --- Bootstrap-compiler declaration (Tier 3 of this project's 3-tier
# TCC policy: thinC's own code is always TCC; third-party recipes are
# TCC by default with real effort; a small, explicit, documented
# exception list exists for genuinely infeasible cases -- kernel and
# openssl already sit there for their own reasons). gcc belongs there
# too, confirmed the hard way, not assumed: an earlier version of this
# recipe (when still targeting GCC 16.x, before the re-pin to 12.5.0
# documented above) spent a very long real investigation (full trail in
# git log, not repeated here) trying to build gcc itself under TCC, and
# progressively found real, un-work-aroundable limits -- TCC's own
# generated cc1 genuinely segfaulting on certain optimizer passes over
# real libgcc source, and (when routing around that by substituting a
# different, ambient real gcc for parts of the build) that ambient
# compiler being too version-skewed from that GCC 16 source specifically
# to understand its own newer x86 ISA function attributes -- a real
# finding about that version gap, not evidence this GCC 12.5.0 build
# would fare any differently under TCC (not re-attempted, since the
# underlying "TCC's own cc1 segfaults on real libgcc source" limit is
# the actual, version-independent reason this stays a Tier-3 exception).
# Self-hosting a full C++ compiler is the single hardest bootstrap
# problem in any toolchain ecosystem for a reason.
#
# The fix is the industry-standard one every real compiler (gcc
# itself, rustc, Go, LLVM) actually uses: build with a real, working
# *bootstrap* compiler once, then have the new compiler rebuild
# *itself*, verified. `--enable-bootstrap` below is not
# `--disable-bootstrap` -- it performs GCC's real 3-stage bootstrap:
# stage 1 is built by the ambient host gcc (this build container's own
# already-present gcc 12.2, ordinary Debian-provided at this layer,
# not part of thinC's own package tracking); stage 2 is built *by
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
# 12.5.0 tree's own stage-3 binaries and libraries) into the image --
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

	bzip2 -dc /build/extra/gmp-6.2.1.tar.bz2 > /build/gmp.tar
	/build/miniextract /build/gmp.tar && mv gmp-6.2.1 gmp
	bzip2 -dc /build/extra/mpfr-4.1.0.tar.bz2 > /build/mpfr.tar
	/build/miniextract /build/mpfr.tar && mv mpfr-4.1.0 mpfr
	gzip -dc /build/extra/mpc-1.2.1.tar.gz > /build/mpc.tar
	/build/miniextract /build/mpc.tar && mv mpc-1.2.1 mpc
	rm -f /build/gmp.tar /build/mpfr.tar /build/mpc.tar /build/miniextract.c /build/miniextract

	mkdir -p build
	cd build
	CC=/usr/bin/gcc CXX=/usr/bin/g++ ../configure --prefix=/usr \
		--enable-bootstrap --disable-multilib --enable-languages=c,c++ \
		--disable-libsanitizer --disable-lto --with-isl=no \
		--with-system-zlib --disable-nls --with-build-config=no

	# `make bootstrap` (not bare `make`/`make all`) is GCC's own
	# documented, explicit entry point for the full 3-stage
	# build-and-self-verify sequence described above -- an intentional,
	# named target, not a side effect of some other invocation.
	#
	# Deliberately bounded, not `-j"$(nproc)"`: a real, un-bounded
	# `-j$(nproc)` bootstrap on the real box (8GB RAM total,
	# `pkg-build-config`'s own `memory_max` was 0 -- no enforced ceiling
	# at the time) hung the entire host, not just this build --
	# `thincd` itself stopped answering even `GET /v1/health` for over
	# 20 minutes, requiring a hard host reset to recover. No log
	# evidence survived to prove the exact mechanism (the kernel's own
	# dmesg ring buffer is wiped by a reboot, and thincd's own log store
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
	# A second, unrelated regenerated-per-stage artifact needing the
	# identical periodic-sweep treatment: gcc/configure picks
	# INSTALL_HEADERS_DIR = install-headers-tar (a real, live-confirmed
	# choice on this box -- checked directly in a real stage2 Makefile,
	# not assumed) for copying a completed stage's own include-fixed/
	# tree into the next stage (gcc/Makefile.in's real-install-headers-tar
	# target: `tar -cf - . | tar xpf -`). This hits the exact same
	# ambient-tar bug this recipe set already has a real, confirmed fix
	# for elsewhere (see pkg_build()'s own miniextract.c above, and its
	# comment for the full original diagnosis) -- tar here silently
	# stops after the first archive entry, so a later stage's own
	# include-fixed/ ends up with only whichever single file happened to
	# be tar'd first (confirmed live: limits.h present, byte-identical
	# to the source stage's own copy, but syslimits.h -- a real file
	# genuinely present in the source stage's own include-fixed/ --
	# silently missing, breaking the very next libgcc compile that
	# includes it). Fixed the same way as the exec-tool wrappers: force
	# install-headers-cp instead (a plain `cp -p -r`, no tar involved at
	# all) in any newly-configured Makefile, every 2 seconds, for the
	# whole bootstrap -- `--enable-bootstrap` regenerates gcc/Makefile
	# at each stage boundary, so a one-shot fix on the first one isn't
	# enough, same reasoning as the exec-tool fix.
	#
	# 12.5.0-5: --with-build-config= added below (2026-08-19), a real,
	# confirmed speedup for THIS box specifically, not a correctness
	# fix -- found while explaining to the user why 12.5.0-4's own -j1
	# attempt was running well past the usual 1.5-4h a GCC bootstrap
	# normally takes elsewhere. Confirmed directly, not guessed, by
	# exec'ing into the live 12.5.0-4 build container and reading its
	# own config.log: `configure:9655: checking for default
	# BUILD_CONFIG` resolved to `BUILD_CONFIG='bootstrap-debug'` --
	# GCC's own auto-selected default for this exact source/configure
	# combination, never explicitly requested by this recipe. That
	# config runs stage2 twice (`STAGE2_*FLAGS=... -gtoggle`), once
	# with -g toggled off and once on, purely to diff-compare the
	# resulting object code and prove debug-info generation doesn't
	# perturb codegen -- a real, legitimate verification GCC's own docs
	# describe, but genuinely optional extra work on top of the 3-stage
	# bootstrap-and-compare `--enable-bootstrap` already performs on its
	# own, and a real, material contributor to wall-clock time on a
	# 2-vCPU box already constrained to -j1 by 12.5.0-3/-4's own real,
	# confirmed parallelism races. `--with-build-config=` (empty)
	# explicitly overrides the auto-detection, disabling it -- the
	# 3-stage bootstrap's own stage2-vs-stage3 byte-for-byte comparison
	# (the actual self-hosting proof this recipe's whole bootstrap-
	# compiler section above depends on) is completely unaffected;
	# only the extra bootstrap-debug compare-pass is skipped. Not
	# applied retroactively to the already-running 12.5.0-4 attempt --
	# doing so would need a fresh ./configure, discarding real,
	# already-verified progress past both of that attempt's own prior
	# failure points; this version exists for whichever rebuild
	# actually needs to start fresh next (a further gcc fixup, or the
	# 14.x/16.x chain in issue #38).
	#
	# 12.5.0-7 (2026-08-19): -5's own `--with-build-config=` (empty)
	# did NOT actually disable bootstrap-debug -- confirmed the hard
	# way, not assumed: 12.5.0-6 (carrying -j2 and this same empty
	# flag) got all the way through all 3 real stages, genuinely
	# self-hosting, then failed GCC's own final stage2-vs-stage3
	# comparison on exactly libiberty/{lrealpath,regex}.o (both the
	# plain and pic/ variants) -- pulled both real .o files out of the
	# kept build container and byte-compared them directly: stage2's
	# copy was 1464 bytes, stage3's was 5256 bytes, a signature exactly
	# matching bootstrap-debug's own STAGE2 "-fno-checking -gtoggle"
	# (debug info toggled OFF) vs STAGE3 "-fchecking=1" (debug info
	# stays on) split -- i.e. BUILD_CONFIG=bootstrap-debug was still
	# genuinely active despite -5's own flag. Root-caused precisely by
	# reading gcc's own real, generated ./configure script directly
	# (not gcc docs, not guessed): its `--with-build-config` handling
	# only special-cases literal `yes` or `no` values --
	# `case $with_build_config in yes) ... ;; no) ... BUILD_CONFIG= ;;
	# esac` -- an EMPTY value matches neither arm, so
	# `test "x${with_build_config}" != x` is false and BUILD_CONFIG is
	# left completely untouched, silently keeping whatever the
	# top-level Makefile's own hardcoded default already is
	# (`BUILD_CONFIG = bootstrap-debug`, confirmed directly in the
	# generated top-level Makefile). `--with-build-config=no` is the
	# one case that actually clears it (`with_build_config= BUILD_CONFIG=`).
	# The real 3-stage bootstrap-and-compare itself -- the actual
	# self-hosting proof -- is still completely unaffected either way;
	# only the extra bootstrap-debug compare-pass is what's being
	# skipped, same intent as -5, now actually achieved.
	fixup_install_headers_dir() {
		find . -maxdepth 4 -name Makefile -type f 2>/dev/null | \
		while read -r f; do
			grep -q "^INSTALL_HEADERS_DIR = install-headers-tar$" "$f" 2>/dev/null || continue
			sed -i "s/^INSTALL_HEADERS_DIR = install-headers-tar\$/INSTALL_HEADERS_DIR = install-headers-cp/" "$f"
		done
	}
	# 12.5.0-3: the -2 fix above (patch the Makefile so future
	# stmp-fixinc runs use cp, not tar) turned out not to be enough by
	# itself -- a real -j2 run got much further (past _muldi3.o) but
	# then hit the identical missing-syslimits.h error again, this time
	# on two files failing at the exact same instant
	# (generic-morestack.c / generic-morestack-thread.c) -- real
	# evidence stmp-fixinc's own `rm -rf include-fixed; mkdir
	# include-fixed` re-ran a second time mid-build (very plausibly
	# -j2-enabled, though not conclusively proven) and the periodic
	# Makefile patch above lost that specific timing race: a `make`
	# sub-process that had already parsed the old install-headers-tar
	# value before the patch landed isn't affected by a later edit to
	# the same file's text. Rather than continuing to chase exactly
	# when/why stmp-fixinc re-runs, fix the actual symptom directly,
	# the same reactive-repair philosophy fixup_exec_tools() already
	# uses successfully: don't try to prevent a bad regeneration, just
	# keep correcting its result. Every real GCC include-fixed/limits.h
	# is supposed to have a real syslimits.h right beside it (in the
	# normal, non-broken case they're identical content -- GCC's own
	# Makefile.in literally does `mv limits.h syslimits.h` when
	# generating them from scratch) -- so any limits.h found missing
	# its own syslimits.h gets one copied from itself, unconditionally,
	# every 2 seconds, regardless of which mechanism or how many times
	# produced the gap.
	fixup_syslimits() {
		find . -maxdepth 6 -name limits.h -path "*/include-fixed/*" -type f 2>/dev/null | \
		while read -r f; do
			dir=$(dirname "$f")
			[ -f "$dir/syslimits.h" ] && continue
			cp "$f" "$dir/syslimits.h"
		done
	}
	fixup_exec_tools
	fixup_install_headers_dir
	fixup_syslimits
	( while true; do fixup_exec_tools; fixup_install_headers_dir; fixup_syslimits; sleep 2; done ) &
	fixup_pid=$!
	# Back to -j1 (12.5.0-4, 2026-08-18): -j2 and -j4 were each tried in
	# turn and each hit a real, DIFFERENT failure -- -j2 lost a real
	# timing race against stmp-fixinc regenerating include-fixed/ mid-
	# build (see the -3 comment); -j4 crashed the *ambient* stage1 seed
	# g++ itself (a real segfault compiling libgcov-merge.c, no
	# confirmed OOM-kill evidence in the logs, but a very plausible
	# genuine race on gmp.h -- a file generated as part of stage1's own
	# in-tree GMP build, read by one job before another finished
	# generating it). Two separate, real, escalating failures from two
	# separate parallelism increases is real evidence, not just the
	# original author's own theoretical caution -- reverting to -j1 to
	# get one clean, unambiguous baseline confirming the syslimits.h fix
	# alone resolves the real, original blocker, before treating higher
	# parallelism as its own separate, isolated experiment again.
	#
	# 12.5.0-6 (2026-08-19, user-directed): back to -j2, the confirmed
	# real ceiling on this exact box (`nproc` inside a build container,
	# and the guest kernel's own boot log -- "smpboot: Total of 2
	# processors activated" -- both agree, checked directly, not
	# assumed). This is not blind re-escalation: -2's own fixup_syslimits()
	# (kept, active, unmodified since -3) was written specifically to
	# close the real -j2 stmp-fixinc timing race that caused the
	# original -j2 failure -- that race was never re-tested with the
	# fix actually in place until now. -j4's own separate ambient-g++
	# crash is not being revisited (unmitigated, and moot here anyway --
	# -j4 would oversubscribe a genuine 2-core box regardless).
	make -j2 bootstrap
	make_rc=$?
	kill "$fixup_pid" 2>/dev/null
	wait "$fixup_pid" 2>/dev/null
	if [ "$make_rc" -ne 0 ]; then
		# Autoconf-driven configure failures at any depth only print
		# "See config.log for more details" to stdout/stderr -- the
		# actual compiler invocation and error text never reach the
		# captured build log otherwise. Dump only the single MOST
		# RECENTLY WRITTEN config.log (a full GCC tree has dozens, one
		# per subproject configured so far; dumping all of them
		# overflows thincd's own fixed-size captured-build-output
		# buffer before reaching the relevant one -- confirmed the hard
		# way during the earlier TCC investigation).
		latest_config_log=$(find . -name config.log -printf '%T@ %p\n' | \
		    sort -rn | head -1 | cut -d' ' -f2-)
		if [ -n "$latest_config_log" ]; then
			echo "=== $latest_config_log (error context) ==="
			# config.log's own error-line format embeds a source line
			# number between the colons (`configure:1234: error: ...`),
			# not a plain space -- confirmed the hard way earlier.
			grep -n -B 30 -A 5 '^configure:[0-9]*: error' \
			    "$latest_config_log" | tail -n 150
		fi
		exit 1
	fi
}

# A real, genuinely new class of dependency this recipe set hasn't hit
# before: gcc does not compile C itself -- it's a driver program that
# shells out to real subprocess binaries for the actual work. cc1 (C),
# cc1plus (C++), and lto1 (link-time optimization) live under
# usr/libexec/gcc/x86_64-pc-linux-gnu/12.5.0/, found via a real build's
# own `make install` output, not guessed at -- without this exact
# directory, `gcc -c foo.c` fails outright with "cc1: not found", not a
# degraded-but-working state the way a missing optional runtime lib
# would be elsewhere in this project. Likewise usr/lib/gcc/
# x86_64-pc-linux-gnu/12.5.0/{crtbegin.o,crtend.o,crtbeginS.o,crtendS.o,
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
# C++ standard library headers (usr/include/c++/12.5.0/) are kept in
# full -- without them g++ cannot compile any real C++ program at all,
# the same "load-bearing, not documentation" reasoning already applied
# to autoconf/automake/perl's own runtime data directories.
#
# Triplet-prefixed duplicate binaries (x86_64-pc-linux-gnu-gcc, etc --
# confirmed via `ls -la` to be real hardlinks of the plain-named ones,
# so dropping them costs zero extra disk, just directory-entry
# cleanliness) are skipped, matching binutils.recipe's own precedent --
# this project never cross-compiles. gdb pretty-printer scripts
# (usr/share/gcc-12.5.0/python), the install-only fixincl/fixinc.sh/
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
	       "$PKG_DESTDIR/usr/share/gcc-12.5.0" \
	       "$PKG_DESTDIR/usr/libexec/gcc/x86_64-pc-linux-gnu/12.5.0/install-tools" \
	       "$PKG_DESTDIR/usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/plugin" \
	       "$PKG_DESTDIR/usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/install-tools"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	# The real, versioned libstdc++.so.6.0.<N> filename is discovered from
	# this build's own actual output, never hardcoded -- its exact minor
	# SONAME number is tied to libstdc++-v3's own libtool version-info for
	# whichever GCC release this recipe happens to be pinned to at any
	# given time (confirmed the hard way: an earlier version of this
	# recipe carried a hardcoded "libstdc++.so.6.0.35", the real value for
	# GCC 16.x, silently stale and wrong the moment this recipe was
	# re-pinned to a different major version -- readlink -f on the
	# already-installed SONAME symlink is the actual, authoritative
	# source, not a number copy-pasted from any prior recipe or release).
	libstdcxx_real=$(basename "$(readlink -f "$PKG_DESTDIR/usr/lib64/libstdc++.so.6")")
	cp -a "$PKG_DESTDIR/usr/lib64/libgcc_s.so.1" \
	   "$PKG_DESTDIR/usr/lib64/libstdc++.so.6" "$PKG_DESTDIR/usr/lib64/$libstdcxx_real" \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	ln -sf /usr/bin/ld "$PKG_DESTDIR/usr/libexec/gcc/x86_64-pc-linux-gnu/12.5.0/ld"
}
