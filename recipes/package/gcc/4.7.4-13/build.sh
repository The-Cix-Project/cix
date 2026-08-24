#
# gcc 4.7.4 -- a deliberate, user-directed pivot in this project's own
# self-hosted toolchain bootstrap effort (issues #36-#40, milestone 16,
# "no ambient/external seed compiler, ever"). The prior approach --
# bootstrapping gcc/16.2.0 with CC=/usr/bin/gcc pointed at this
# project's own earlier self-built gcc/12.5.0 -- hit a real, deeply
# root-caused GCC codegen bug (a corrupted jump-instruction encoding in
# libgcc's own unwind-dw2-btree.h, confirmed via gdb against real
# crashed binaries) that, after three separate multi-hour bootstrap
# attempts, kept resurfacing in different, unrelated places (a second
# function in the same header, then an unrelated dlopen-self hang in
# libstdc++'s own configure) -- evidence of something broader and more
# systemic than one fixable header, not something to keep whack-a-mole
# patching indefinitely.
#
# This recipe instead follows the real, well-established Live-Bootstrap/
# GNU Guix "bootstrappable builds" methodology's own intermediate
# stages: this project already has ONE trusted, self-hosted, no-
# ambient-seed compiler -- TCC (tcc.recipe) -- so the correct next step
# is having TCC build a real, historical GCC release directly, rather
# than reaching for any ambient/ombient-descended compiler again.
# gcc-4.7.4 (2013) is the target: old enough that its own compiler
# internals are still plain C (confirmed directly, not assumed: grep
# for GCC's own pre-C++-conversion `VEC()` container macro throughout
# gcc/c-typeck.c finds real, live use -- the switch to a real C++-
# implemented GCC didn't land until 4.8), so a C-only compiler is
# architecturally capable of building it, unlike gcc/16.2.0's own
# already-documented TCC-cc1-segfaults-on-modern-libgcc wall.
#
# --enable-languages=c only for this first attempt (not c,c++) --
# proving TCC can build a working C compiler here at all before
# expanding scope to C++, matching this project's own "one micro-step
# at a time" discipline. --disable-bootstrap (a single-pass build, not
# GCC's own 3-stage self-verification) for the same reason -- establish
# real, basic viability first; a real bootstrap (self-hosting
# verification) can follow once this is proven to work.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# directly (sha256sum against the fetched tarball). gmp-4.3.2/
# mpfr-2.4.2/mpc-0.8.1 are the exact prerequisite versions gcc-4.7.4's
# own contrib/download_prerequisites pins (read directly from the
# extracted tarball, not guessed), vendored the same offline,
# checksummed way gcc/16.2.0's own recipe already established (this
# project's isolated build container has no live network access for
# gcc's own contrib script to fetch them itself).
#
pkg_name="gcc"
pkg_version="4.7.4-13"
pkg_source="https://ftp.gnu.org/gnu/gcc/gcc-4.7.4/gcc-4.7.4.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/gmp-4.3.2.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpfr-2.4.2.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpc-0.8.1.tar.gz"
pkg_sha256="92e61c6dc3a0a449e62d72a38185fda550168a86702dea07125ebd3ec3996282 936162c0312886c21581002b79932829aa048cfaf9937c6265aeaa14f1cd1775 c7e75a08a8d49d2082e4caee1591a05d11b9d5627514e678f02d66a124bcf2ba e664603757251fd8a352848276497a4c79b7f8b21fd8aedd5cc0598a38fee3e4"
pkg_depends="binutils m4"

pkg_build() {
	# Same real, confirmed, environment-specific tar bug gcc/16.2.0's
	# own recipe already documents (a real multi-file tarball silently
	# stops after the very first archive entry in this exact pkgbuild
	# sandbox) -- same minimal, dependency-free USTAR extractor
	# sidesteps it, real mode bits and mtimes applied from each header.
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
	tcc /build/miniextract.c -o /build/miniextract

	bzip2 -dc /build/extra/gmp-4.3.2.tar.bz2 > /build/gmp.tar
	/build/miniextract /build/gmp.tar && mv gmp-4.3.2 gmp
	bzip2 -dc /build/extra/mpfr-2.4.2.tar.bz2 > /build/mpfr.tar
	/build/miniextract /build/mpfr.tar && mv mpfr-2.4.2 mpfr
	gzip -dc /build/extra/mpc-0.8.1.tar.gz > /build/mpc.tar
	/build/miniextract /build/mpc.tar && mv mpc-0.8.1 mpc
	rm -f /build/gmp.tar /build/mpfr.tar /build/mpc.tar /build/miniextract.c /build/miniextract

	# Real, well-known "ancient GCC vs. modern glibc" incompatibility,
	# not a TCC bug -- confirmed directly, not guessed: libgcc's own
	# config/i386/linux-unwind.h (this recipe's own real build got past
	# the exec-tools gap and into genuinely compiling libgcc.a before
	# hitting this) uses the legacy, non-POSIX `struct ucontext` type
	# name (`struct ucontext *uc_ = context->cfa;`), which glibc has
	# since stopped exposing as a distinct struct tag -- confirmed via
	# a direct grep of this sandbox's own /usr/include/.../sys/
	# ucontext.h: only `ucontext_t` (a typedef, not a `struct ucontext`
	# tag) exists now. `struct ucontext` is therefore a genuinely
	# incomplete type to any modern glibc, regardless of which compiler
	# processes it -- this would hit real ambient gcc just as hard.
	# `ucontext_t` is the real, complete, already-available replacement
	# type -- this is the well-established fix the wider "build an old
	# GCC on a modern system" bootstrapping community (Live-Bootstrap
	# included) already uses for this exact, recurring issue.
	sed -i 's/struct ucontext\b/ucontext_t/g' libgcc/config/i386/linux-unwind.h
	grep -q 'ucontext_t \*uc_ = context->cfa' libgcc/config/i386/linux-unwind.h || {
		echo "FATAL: linux-unwind.h ucontext_t patch did not apply -- source layout changed" >&2
		exit 1
	}

	mkdir -p build
	cd build
	# gmp/mpfr/mpc are NOT pointed at via --with-gmp=/--with-mpfr=/
	# --with-mpc= (those expect an already-built, already-installed
	# prefix, not a bare source tree) -- placing their source directly
	# as gcc/mpc/gmp/mpfr sibling subdirectories inside the top-level
	# gcc source tree (already done above) is gcc's own documented,
	# standard in-tree convention: its build system auto-detects and
	# builds them itself as part of the gcc build, the exact same
	# mechanism gcc/16.2.0's own recipe already relies on.
	# Re-pinned to -6: -5 established a real, fully working C-only
	# gcc-4.7.4 (validated: real compile+link of hello.c, and the
	# original rol64 ICE case that started this whole investigation
	# compiles clean here with no crash). User-directed next step: use
	# THIS gcc to seed gcc/16.2.0's own bootstrap instead of the
	# ambient-descended, confirmed-buggy gcc-12.5.0. gcc-16.2.0's own
	# internals are real C++, so this build needs a working g++ first --
	# adding `c,c++` to --enable-languages. This does NOT need a C++
	# compiler to build gcc-4.7.4 ITSELF (already confirmed via the
	# VEC() macro grep in this recipe's own top comment: gcc-4.7.4's own
	# cc1/cc1plus source is still plain C, pre-dating GCC's own switch
	# to a C++-implemented compiler at 4.8) -- TCC compiles cc1plus (a
	# C-source program that happens to implement a C++ frontend for its
	# TARGET language) exactly like it already compiled cc1, and THAT
	# freshly-built cc1plus then compiles libstdc++-v3's own real C++
	# source, the same standard "compiler builds its own successor's
	# runtime" bootstrap step every from-scratch GCC build relies on --
	# no separate CXX= needed, gcc's own build system uses its own
	# just-built g++ for this automatically.
	#
	# Repair the SEED compiler before using it (issue #55).
	#
	# -7 fixed the installed output and could never prove it, because
	# the fix only lands after a successful build and the build cannot
	# succeed: the compiler already installed in this sandbox is the
	# broken one, and configure dies on it before anything is compiled.
	#
	#   /usr/bin/gcc -E conftest.c
	#   /usr/include/limits.h:124:26: error: no include path in which
	#                                        to search for limits.h
	#   configure: error: C preprocessor "/lib/cpp" fails sanity check
	#
	# glibc's own limits.h reaches the compiler's copy with
	# `#include_next <limits.h>`. The installed gcc ships no
	# include-fixed/ at all -- confirmed from its own file list, 0
	# entries -- so that resolves to nothing, every `#include
	# <limits.h>` fails, and autoconf concludes the preprocessor is
	# broken and falls through to /lib/cpp, which does not exist on
	# these images either. It reports the fallback, never the cause,
	# which is why this reads as a missing /lib/cpp and is not.
	#
	# It is also why -7 stopped at a *C++* preprocessor check: the
	# sandbox now has this same gcc's g++, and gmp's configure probes
	# it. One missing pair of headers, surfacing as two different
	# errors in two different sub-configures.
	#
	# The pair is reconstructed from GCC's own installed templates, not
	# hand-written: install-tools/include/limits.h is the fixed limits.h
	# ready to install, and install-tools/gsyslimits.h is the
	# include_next wrapper that mkheaders installs as syslimits.h. Doing
	# it from those files means the seed gets exactly what its own build
	# would have produced.
	#
	seed_fixed=$(/usr/bin/gcc -print-file-name=include-fixed 2>/dev/null)
	case "$seed_fixed" in
	/*) ;;
	*)  seed_fixed="/usr/lib/gcc/$(/usr/bin/gcc -dumpmachine)/$(/usr/bin/gcc -dumpversion)/include-fixed" ;;
	esac
	seed_tools="$(dirname "$seed_fixed")/install-tools"
	mkdir -p "$seed_fixed"
	if [ ! -f "$seed_fixed/limits.h" ] && [ -f "$seed_tools/include/limits.h" ]; then
		echo "gcc 4.7.4-10: seeding $seed_fixed/limits.h from the compiler's own template"
		cp "$seed_tools/include/limits.h" "$seed_fixed/limits.h"
	fi
	if [ ! -f "$seed_fixed/syslimits.h" ] && [ -f "$seed_tools/gsyslimits.h" ]; then
		echo "gcc 4.7.4-10: seeding $seed_fixed/syslimits.h from the compiler's own template"
		cp "$seed_tools/gsyslimits.h" "$seed_fixed/syslimits.h"
	fi

	# Prove the repair worked before spending an hour finding out it
	# did not. Both compilers, because both get probed: gmp's configure
	# asks g++, everything else asks gcc.
	printf '#include <limits.h>\nint probe[INT_MAX > 0 ? 1 : -1];\n' > /build/limits_probe.c
	if ! /usr/bin/gcc -E /build/limits_probe.c > /dev/null; then
		echo "gcc: seed repair failed -- the C preprocessor still cannot resolve limits.h" >&2
		exit 1
	fi
	if [ -x /usr/bin/g++ ] && ! /usr/bin/g++ -E /build/limits_probe.c > /dev/null; then
		echo "gcc: seed repair failed -- the C++ preprocessor still cannot resolve limits.h" >&2
		exit 1
	fi
	rm -f /build/limits_probe.c

	#
	# --enable-bootstrap, and this time it can actually run (#116).
	#
	# 4.7.4-8 tried this and never reached a compile: the in-tree gmp's
	# configure probes for a C++ preprocessor, and the ambient g++ could
	# not resolve limits.h because this compiler shipped no
	# include-fixed/ at all. That was #55, fixed in -10/-11 -- and the
	# seed repair above now runs before configure, so the probe passes.
	#
	# Why it matters now: every compiler this 4.7.4 builds is broken the
	# same way. gcc-6.4.0 and gcc-9.5.0 -- five years apart, different
	# prerequisites, different sources -- both produce a cc1 that
	# deadlocks in the same place (libgcc's configure conftest) with the
	# same futex word and the same impossible expected value, 0x10100.
	# Identical across both is not random corruption; it is one
	# miscompiled code path reproducing exactly. The only thing those
	# two builds share is the compiler that built them.
	#
	# A 3-stage bootstrap is the direct test. TCC builds stage1, stage1
	# builds stage2, stage2 builds stage3, and stage2 and stage3 are
	# compared byte for byte. If TCC's stage1 is subtly wrong, a
	# miscompile that is not perfectly self-reproducing shows up as that
	# comparison failing -- and if it passes, the installed compiler is
	# one that compiled itself twice, with TCC demoted to a discarded
	# seed rather than the thing every later compiler inherits from.
	#
	CC=tcc ../configure --prefix=/usr \
		--enable-bootstrap --disable-multilib --enable-languages=c,c++ \
		--disable-libsanitizer --disable-lto --disable-nls

	# Re-pinned to -2: the plain, unpatched build above got real,
	# substantial confirmation that TCC CAN build a working gcc-4.7.4
	# stage1 (xgcc) -- it got all the way through cc1's own build,
	# fixincludes, and into configuring libgcc -- before hitting the
	# exact same already-root-caused, already-fixed gap gcc/16.2.0's
	# own recipe documents in detail: gcc's own build system generates
	# `as`/`collect-ld`/`nm` as real `#!/bin/sh` dispatch scripts at
	# sub-configure time, and this kernel's own binfmt_script handling
	# returns a genuine ENOEXEC for a valid, correctly-written,
	# executable shebang script when the calling process was compiled
	# by TCC (confirmed directly here, via
	# `xgcc: error trying to exec '.../as': execv: Exec format error`,
	# and via reading `.../gcc/as` directly -- it is a real `#!/bin/sh`
	# script, not a binary) -- the fix already established for
	# gcc/16.2.0 applies unchanged: a plain symlink straight to the
	# real underlying tool is behaviorally identical (that dispatch
	# always reduces to `exec /usr/bin/as "$@"` etc. for this exact
	# project configuration) while completely avoiding binfmt_script,
	# since execve() on a symlink to a real ELF never goes through
	# shebang interpretation at all. Simply forgotten when this recipe
	# was first written, not a new investigation -- ported over
	# verbatim from gcc/16.2.0's own build.sh.
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
	#
	# Seed include-fixed into the BUILD TREE's stage compilers, not just
	# the installed one (issue #55, one layer deeper; issue #116).
	#
	# -12's bootstrap died at configure-stage2-target-libgcc with the
	# same signature #55 had everywhere else: the stage2 in-tree xgcc
	# could not resolve <limits.h> (glibc's own limits.h does
	# #include_next expecting the compiler's copy first), so autoconf
	# declared the preprocessor broken and fell through to /lib/cpp,
	# which does not exist here. The seed repair above fixes the
	# INSTALLED compiler this build starts from; each freshly built
	# stage inside ./build/gcc has the same hole, because whatever
	# leaves include-fixed unpopulated on this box (the still-unsolved
	# root of #55) applies to every stage equally.
	#
	# Seeded from the same install-tools templates as the installed
	# repair -- GCC's own ready-to-install limits.h (the composed
	# limitx+glimits+limity form, correct here since /usr/include/
	# limits.h exists) and gsyslimits.h -- so every stage gets exactly
	# what its own mkheaders would have produced. Identical content in
	# every stage also means the stage2/stage3 comparison is unaffected:
	# same headers in, same objects out. If the build's own
	# stmp-int-hdrs later regenerates them, its rm -f simply replaces
	# ours with the same thing.
	#
	fixup_stage_headers() {
		for d in gcc prev-gcc stage1-gcc stage2-gcc stage3-gcc; do
			[ -d "/build/src/build/$d" ] || continue
			fixed="/build/src/build/$d/include-fixed"
			mkdir -p "$fixed"
			if [ ! -f "$fixed/limits.h" ] && [ -f "$seed_tools/include/limits.h" ]; then
				cp "$seed_tools/include/limits.h" "$fixed/limits.h"
			fi
			if [ ! -f "$fixed/syslimits.h" ] && [ -f "$seed_tools/gsyslimits.h" ]; then
				cp "$seed_tools/gsyslimits.h" "$fixed/syslimits.h"
			fi
		done
	}
	fixup_exec_tools
	fixup_stage_headers
	( while true; do fixup_exec_tools; fixup_stage_headers; sleep 2; done ) &
	fixup_pid=$!
	make bootstrap -j"$(nproc)"
	make_rc=$?
	#
	# `|| true` on both, and it is load-bearing (issue #55, -11).
	#
	# Recipes now run under `set -e`, so every command's exit status is
	# checked -- which is the point, and it caught a real package that
	# shipped half-installed. But `kill` on a job that has already
	# exited returns 1, and `wait` on a job that was just SIGTERM'd
	# returns 143. Both are the expected outcome of deliberately
	# stopping a helper, and under set -e both end the build.
	#
	# -10 hit exactly that: the whole compiler built, `make` returned 0,
	# and the recipe then died on the `wait` for a background loop it
	# had just killed on purpose. The daemon reported exit 143 as
	# "overlay mount or exec failed: No such process", because 143 is
	# both 128+SIGTERM and 140+ESRCH in its own encoding -- so a
	# successful build was reported as a container that never started.
	#
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
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	# Real, confirmed gap in -3's own plain `make install`: this
	# project's own real validation (compiling a trivial hello.c
	# against the actually-installed image) found
	# .../lib/gcc/x86_64-unknown-linux-gnu/4.7.4/include/stddef.h (gcc's
	# own freestanding compiler-provided header -- not glibc's) missing
	# from the installed output entirely, even though the BUILD tree's
	# own `./gcc/include/` already has it (and every other freestanding
	# header) correctly generated and fully processed -- confirmed via
	# the build's own log, which shows the real stdint.h wrap/fixinclude
	# logic already ran successfully during `make all`. The exact reason
	# `make install`'s own header-install sub-target doesn't reach
	# DESTDIR for this configuration (--disable-bootstrap,
	# --enable-languages=c only) isn't fully root-caused, but the fix
	# doesn't need it to be: copy the build tree's own already-correct,
	# already-processed `./gcc/include/` directly into DESTDIR's
	# expected install location. Safe regardless of whether `make
	# install` already got this right (a harmless identical re-copy) or
	# not (the real, working fix) -- uses the exact same files gcc's own
	# build already produced, not a naive re-copy of raw, unprocessed
	# gcc/ginclude/ source (which would miss the stdint.h wrapping logic
	# `make all` already applied).
	install_include_dir="$PKG_DESTDIR/usr/lib/gcc/x86_64-unknown-linux-gnu/4.7.4/include"
	mkdir -p "$install_include_dir"
	cp -a ./gcc/include/. "$install_include_dir/"

	#
	# -7 (issue #55): the same gap, one directory over. `include/` was
	# being copied and `include-fixed/` was not -- and limits.h lives
	# in include-fixed/, not include/. The installed compiler therefore
	# had NO limits.h anywhere, in either directory.
	#
	# That is not a cosmetic omission. glibc's own /usr/include/limits.h
	# does `#include_next <limits.h>` to reach the compiler's copy, so
	# with none present every `#include <limits.h>` fails outright with
	# "no include path in which to search for limits.h" and INT_MAX,
	# UINT_MAX and PATH_MAX are all undeclared. Confirmed directly on
	# the real build sandbox with a `gcc -H` header trace before
	# changing anything, not inferred from the error text: `gcc
	# -print-file-name=include-fixed` pointed at a real but EMPTY
	# directory, and include/limits.h did not exist either.
	#
	# The consequence was not local to gcc. This compiler is what the
	# cumulative pkgbuild sandbox uses, so perl -- and therefore
	# openssl, and therefore anything needing libcrypto -- could not be
	# built into a fresh image at all. Found while packaging a DHCP
	# client (issue #106), which needed openssl and could not have it.
	#
	# Fixed the same way, and for the same reason, as the include/ copy
	# above: take the build tree's own already-generated, already-
	# processed output rather than reconstructing anything. If that
	# directory is somehow empty too, fall back to generating the pair
	# exactly as GCC's own mkheaders does -- glimits.h becomes
	# limits.h, and syslimits.h is the standard include_next wrapper --
	# so the installed compiler always has a working limits.h whichever
	# way the build went. Which path was taken is printed, because a
	# fallback that fires silently is a fallback nobody knows they are
	# relying on.
	#
	install_fixed_dir="$PKG_DESTDIR/usr/lib/gcc/x86_64-unknown-linux-gnu/4.7.4/include-fixed"
	mkdir -p "$install_fixed_dir"
	if [ -d ./gcc/include-fixed ]; then
		cp -a ./gcc/include-fixed/. "$install_fixed_dir/"
	fi
	if [ ! -f "$install_fixed_dir/limits.h" ]; then
		echo "gcc 4.7.4-10: build tree had no include-fixed/limits.h -- generating it"
		cp ./gcc/glimits.h "$install_fixed_dir/limits.h"
	else
		echo "gcc 4.7.4-10: took include-fixed/limits.h from the build tree"
	fi
	if [ ! -f "$install_fixed_dir/syslimits.h" ]; then
		echo "gcc 4.7.4-10: generating include-fixed/syslimits.h"
		cat > "$install_fixed_dir/syslimits.h" <<'SYSLIMITS'
/* syslimits.h stands for the system's own limits.h.
   This is the standard wrapper GCC's own mkheaders installs. */
#ifndef _GCC_NEXT_LIMITS_H
#define _GCC_NEXT_LIMITS_H
#include_next <limits.h>
#undef _GCC_NEXT_LIMITS_H
#endif
SYSLIMITS
	fi

	# Same real, already-documented gap gcc/16.2.0's own recipe already
	# found and fixed: collect2 (gcc's own linker-invocation driver)
	# looks for `ld` via its own private COMPILER_PATH search, not
	# $PATH -- confirmed directly here too (`collect2: fatal error:
	# cannot find 'ld'` on a real link attempt, despite binutils'
	# real /usr/bin/ld genuinely present and on $PATH, binutils being a
	# real pkg_depends= above). Not this recipe's own binary to ship
	# (binutils.recipe already provides the real one) -- just a symlink
	# to where gcc's own search convention expects to find it, for this
	# version's own private prefix directory.
	mkdir -p "$PKG_DESTDIR/usr/libexec/gcc/x86_64-unknown-linux-gnu/4.7.4"
	ln -sf /usr/bin/ld "$PKG_DESTDIR/usr/libexec/gcc/x86_64-unknown-linux-gnu/4.7.4/ld"
}
