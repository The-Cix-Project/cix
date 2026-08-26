#
# gcc 6.4.0 -- the next real Live-Bootstrap-style stepping stone
# (user-directed) between gcc-4.7.4 (2013, this project's own
# TCC-built, no-ambient-seed C/C++ compiler, gcc/4.7.4-6) and
# gcc-16.2.0. Confirmed directly, not guessed: gcc-16.2.0's own
# configure requires a bootstrap compiler with real, complete C++14
# language support (checked directly against gcc-4.7.4-6: its own g++
# fails gcc-16.2.0's own C++14 feature probe -- auto return-type
# deduction, decltype, variable templates -- outright, `configure:
# error: *** A compiler with support for C++14 language features is
# required`), and gcc-4.7.4 (2013, pre-dates C++14's very existence)
# has none. gcc-6.4.0 (2017) is chosen as the bridge: GCC's own C++14
# support was completed in the 5.x series and is mature/complete by
# 6.x, while GCC's own documented minimum-bootstrap-compiler
# requirements stayed deliberately conservative (roughly C++03/early
# C++11) through this era specifically to preserve exactly this kind
# of bootstrap continuity across GCC's own release history -- gcc-4.7.4
# is a real, plausible seed for it. Not yet empirically confirmed --
# this recipe's own real build is that confirmation.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256. gmp-4.3.2/mpfr-2.4.2/mpc-0.8.1 are
# the exact prerequisite versions gcc-6.4.0's own
# contrib/download_prerequisites pins (read directly, not guessed --
# identical to gcc-4.7.4's own pins, already vendored and checksummed
# for that recipe, reused here unchanged). isl (Graphite loop-nest
# optimization) is deliberately not vendored or built, matching
# gcc/16.2.0's own scope call -- --with-isl=no below.
#
#
# gcc 9.5.0 -- the bridge between this project's own TCC-built
# gcc-4.7.4 and current GCC, replacing the 4.7.4 -> 6.4.0 -> 12.5.0
# chain with a single hop.
#
# The version is not a guess. GCC's own installation prerequisites say
# exactly which compiler can build which:
#
#   "Versions of GCC prior to 15 allow bootstrapping with an ISO C++11
#    compiler, versions prior to 10.5 allow bootstrapping with an ISO
#    C++98 compiler, and versions prior to 4.8 allow bootstrapping with
#    an ISO C89 compiler."
#
#   "If you need to build an intermediate version of GCC in order to
#    bootstrap current GCC, consider GCC 9.5"
#
# So: gcc-4.7.4 is what TCC can build (C89-buildable, being < 4.8), and
# it provides a C++98 compiler. Anything below 10.5 can be bootstrapped
# with C++98, which puts 9.5.0 in range -- and 9.5 is the version GCC
# itself names as the intermediate to use. It provides C++17, well past
# the C++14 that GCC >= 15 requires, so it can build gcc-16.2.0
# directly. Two hops instead of three, and both endpoints chosen by the
# upstream documentation rather than by trial.
#
# It also retires gcc-6.4.0 (2017), which never worked here: every
# compiler it produced deadlocked in __futex_wait -- cc1 expanding
# va_arg on a three-line file, and cc1plus on a libstdc++ conftest even
# when built -O0 (issue #116). The futex word sat on the heap holding a
# value no real lock takes, which is heap corruption rather than
# contention. Whether that was 4.7.4 miscompiling 6.4.0 or 6.4.0 being
# too old for glibc 2.36 was never settled, and with this bridge it no
# longer needs to be: 9.5.0 (2022) is contemporary with this glibc,
# where 6.4.0 predated it by five years.
#
pkg_name="gcc"
pkg_version="9.5.0-6"
pkg_source="https://ftp.gnu.org/gnu/gcc/gcc-9.5.0/gcc-9.5.0.tar.xz https://gcc.gnu.org/pub/gcc/infrastructure/gmp-6.1.0.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpfr-3.1.4.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpc-1.0.3.tar.gz"
pkg_sha256="27769f64ef1d4cd5e2be8682c0c93f9887983e6cfd1a927ce5a0a2915a95cf8f 498449a994efeba527885c10405993427995d3f86b8768d8cdf8d9dd7c6b73e8 d3103a80cdad2407ed581f3618c4bed04e0c92d1cf771a65ead662cc397f7775 617decc6ea09889fb08ede330917a00b16809b8db88c29c31bfbb49cbf88ecc3"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/gcc-9.5.0-6.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="3fd84ac5d0ad6308e6e916f0f92dc14b09c638091977ff3050a79ac708b7e486"
# zlib/libc-dev: real, load-bearing (gcc-16.2.0-5's own recipe already
# found and documents this same gap) -- always required for any real
# C/C++ build in a target image with no incidental prior-install
# history to lean on.
pkg_depends="binutils m4 zlib libc-dev"

pkg_build() {
	# Same real, confirmed, environment-specific tar bug gcc/16.2.0's
	# and gcc/4.7.4's own recipes already document (a real multi-file
	# tarball silently stops after the very first archive entry in this
	# exact pkgbuild sandbox) -- same minimal, dependency-free USTAR
	# extractor sidesteps it.
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

	bzip2 -dc /build/extra/gmp-6.1.0.tar.bz2 > /build/gmp.tar
	/build/miniextract /build/gmp.tar && mv gmp-6.1.0 gmp
	bzip2 -dc /build/extra/mpfr-3.1.4.tar.bz2 > /build/mpfr.tar
	/build/miniextract /build/mpfr.tar && mv mpfr-3.1.4 mpfr
	gzip -dc /build/extra/mpc-1.0.3.tar.gz > /build/mpc.tar
	/build/miniextract /build/mpc.tar && mv mpc-1.0.3 mpc
	rm -f /build/gmp.tar /build/mpfr.tar /build/mpc.tar /build/miniextract.c /build/miniextract

	# Same real "ancient GCC vs. modern glibc" incompatibility
	# gcc/4.7.4's own recipe already root-caused and fixed (confirmed
	# this file has the identical `struct ucontext` usage there too,
	# read directly before writing this recipe, not assumed) --
	# `struct ucontext` is a genuinely incomplete type on this sandbox's
	# modern glibc (only `ucontext_t`, a typedef, exists), regardless of
	# GCC version or which compiler processes it.
	sed -i 's/struct ucontext\b/ucontext_t/g' libgcc/config/i386/linux-unwind.h
	grep -q 'ucontext_t \*uc_ = context->cfa' libgcc/config/i386/linux-unwind.h || {
		echo "FATAL: linux-unwind.h ucontext_t patch did not apply -- source layout changed" >&2
		exit 1
	}

	# Re-pinned to -2: the plain -1 build above got real, substantial
	# confirmation gcc-4.7.4-6 (this seed's own compiler) works fine on
	# its own terms -- it hit a real failure only once gcc-6.4.0's OWN
	# build started, configuring gmp: `configure: error: C preprocessor
	# "/lib/cpp" fails sanity check`, real underlying error
	# `/usr/include/limits.h:124:26: error: no include path in which to
	# search for limits.h`. Root-caused directly (probe-gcc-headers
	# v11, not guessed): gcc-4.7.4-6's own .../4.7.4/include-fixed/
	# directory exists but is completely EMPTY -- no compiler-private
	# limits.h at all, so gcc-4.7.4-6's own search finds nothing and
	# falls straight through to glibc's real /usr/include/limits.h,
	# which -- unconditionally, whenever __GNUC__ is defined -- demands
	# a SECOND, compiler-provided limits.h reachable further down the
	# search path via its own #include_next; with nothing there, that
	# #include_next has nowhere left to search.
	#
	# Re-pinned again to -4: -2's own fix (a bare `_GCC_LIMITS_H_`-
	# defining passthrough, straight to #include_next) solved that
	# specific error, but is the exact same INCOMPLETE fix this
	# project's own elfutils 0.192-4 investigation already found and
	# corrected earlier this session for the identical underlying
	# class of bug -- a bare passthrough discards every ISO C limit
	# macro (CHAR_BIT, SCHAR_MIN/MAX, INT_MIN/MAX, LONG_MIN/MAX, etc.)
	# a real compiler-provided limits.h is supposed to supply, since
	# glibc's own header only provides those when __GNUC__ is NOT set
	# (it defers to the compiler otherwise) -- confirmed directly here
	# too, not assumed: gcc-6.4.0's own real compile of
	# libiberty/fibheap.c failed with `'LONG_MIN' undeclared`, an ISO
	# limit macro that only ever comes from the compiler's own private
	# limits.h, never glibc's. Real, complete, correct fix this time
	# (the actual glimits.h/syslimits.h pair GCC's own build normally
	# generates, not a shortcut): a real limits.h with every ISO limit
	# macro derived from real compiler builtins (__SCHAR_MAX__,
	# __INT_MAX__, __LONG_MAX__, __LONG_LONG_MAX__ -- all genuinely
	# provided by any real GCC, confirmed present in gcc-4.7.4-6 the
	# same way they are in every GCC release), PLUS the real
	# `#include "syslimits.h"` chain-in mechanism and matching
	# syslimits.h wrapper, so glibc's own real header (MB_LEN_MAX,
	# PATH_MAX via bits/posix1_lim.h, etc.) is still correctly reached
	# afterward -- the complete mechanism, not a partial one.
	mkdir -p /usr/lib/gcc/x86_64-unknown-linux-gnu/4.7.4/include-fixed
	cat > /usr/lib/gcc/x86_64-unknown-linux-gnu/4.7.4/include-fixed/limits.h <<'EOF'
#ifndef _GCC_LIMITS_H_
#define _GCC_LIMITS_H_

#ifndef _LIBC_LIMITS_H_
#include "syslimits.h"
#endif

#ifndef _LIMITS_H___
#define _LIMITS_H___

#undef CHAR_BIT
#define CHAR_BIT __CHAR_BIT__

#ifndef MB_LEN_MAX
#define MB_LEN_MAX 1
#endif

#undef SCHAR_MIN
#define SCHAR_MIN (-SCHAR_MAX - 1)
#undef SCHAR_MAX
#define SCHAR_MAX __SCHAR_MAX__

#undef UCHAR_MAX
#if __SCHAR_MAX__ == __INT_MAX__
# define UCHAR_MAX (SCHAR_MAX * 2U + 1U)
#else
# define UCHAR_MAX (SCHAR_MAX * 2 + 1)
#endif

#ifdef __CHAR_UNSIGNED__
# undef CHAR_MIN
# if __SCHAR_MAX__ == __INT_MAX__
#  define CHAR_MIN 0U
# else
#  define CHAR_MIN 0
# endif
# undef CHAR_MAX
# define CHAR_MAX UCHAR_MAX
#else
# undef CHAR_MIN
# define CHAR_MIN SCHAR_MIN
# undef CHAR_MAX
# define CHAR_MAX SCHAR_MAX
#endif

#undef SHRT_MIN
#define SHRT_MIN (-SHRT_MAX - 1)
#undef SHRT_MAX
#define SHRT_MAX __SHRT_MAX__

#undef USHRT_MAX
#if __SHRT_MAX__ == __INT_MAX__
# define USHRT_MAX (SHRT_MAX * 2U + 1U)
#else
# define USHRT_MAX (SHRT_MAX * 2 + 1)
#endif

#undef INT_MIN
#define INT_MIN (-INT_MAX - 1)
#undef INT_MAX
#define INT_MAX __INT_MAX__

#undef UINT_MAX
#define UINT_MAX (INT_MAX * 2U + 1U)

#undef LONG_MIN
#define LONG_MIN (-LONG_MAX - 1L)
#undef LONG_MAX
#define LONG_MAX __LONG_MAX__

#undef ULONG_MAX
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
# undef LLONG_MIN
# define LLONG_MIN (-LLONG_MAX - 1LL)
# undef LLONG_MAX
# define LLONG_MAX __LONG_LONG_MAX__
# undef ULLONG_MAX
# define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#endif

#endif /* _LIMITS_H___ */

#else /* not _GCC_LIMITS_H_ */

#ifdef _GCC_NEXT_LIMITS_H
#include_next <limits.h>
#endif

#endif /* not _GCC_LIMITS_H_ */
EOF
	cat > /usr/lib/gcc/x86_64-unknown-linux-gnu/4.7.4/include-fixed/syslimits.h <<'EOF'
#ifndef _GCC_NEXT_LIMITS_H
#define _GCC_NEXT_LIMITS_H
#include_next <limits.h>
#undef _GCC_NEXT_LIMITS_H
#endif
EOF

	mkdir -p build
	cd build
	# CC/CXX point at gcc-4.7.4-6 (this recipe's own --image target is
	# gcc-tcc-bootstrap, where that is the only gcc installed) -- NOT
	# tcc (gcc-6.4.0's own C++ internals need a real C++ frontend to
	# process, unlike gcc-4.7.4's own still-plain-C implementation) and
	# NOT /usr/bin/gcc's other possible meanings elsewhere in this
	# sandbox (this project's own ambient-descended, confirmed-buggy
	# gcc-12.5.0, installed into "dev" specifically, never this image).
	# Single-pass first (--disable-bootstrap + `make all`, not
	# `make bootstrap`): same "prove basic viability before investing in
	# a full 3-stage self-hosting verification" discipline gcc-4.7.4's
	# own recipe and gcc/16.2.0-5 already established -- whether
	# gcc-4.7.4 can really bootstrap gcc-6.4.0 at all hasn't been
	# empirically confirmed yet.
	#
	# Repair the seed compiler before using it (issue #55).
	#
	# The gcc installed in this sandbox ships no include-fixed/ at all,
	# so glibc's own limits.h -- which reaches the compiler's copy with
	# `#include_next <limits.h>` -- resolves to nothing. Every `#include
	# <limits.h>` then fails, autoconf decides the preprocessor is
	# broken, and falls through to /lib/cpp, which does not exist here
	# either. It reports the fallback and never the cause.
	#
	# gcc/4.7.4-11 fixes the installed package, but that repair lives in
	# a build container's upperdir and does not reach the image, and
	# installing the fixed gcc into this sandbox fails at the image
	# merge for an unrelated reason. So the repair is done here too,
	# from GCC's own install-tools templates rather than hand-written
	# headers, and proved with a probe before an hour is spent finding
	# out otherwise.
	#
	seed_fixed=$(/usr/bin/gcc -print-file-name=include-fixed 2>/dev/null)
	case "$seed_fixed" in
	/*) ;;
	*)  seed_fixed="/usr/lib/gcc/$(/usr/bin/gcc -dumpmachine)/$(/usr/bin/gcc -dumpversion)/include-fixed" ;;
	esac
	seed_tools="$(dirname "$seed_fixed")/install-tools"
	mkdir -p "$seed_fixed"
	if [ ! -f "$seed_fixed/limits.h" ] && [ -f "$seed_tools/include/limits.h" ]; then
		echo "gcc 9.5.0: 8: seeding $seed_fixed/limits.h from the compiler's own template"
		cp "$seed_tools/include/limits.h" "$seed_fixed/limits.h"
	fi
	if [ ! -f "$seed_fixed/syslimits.h" ] && [ -f "$seed_tools/gsyslimits.h" ]; then
		echo "gcc 9.5.0: 8: seeding $seed_fixed/syslimits.h from the compiler's own template"
		cp "$seed_tools/gsyslimits.h" "$seed_fixed/syslimits.h"
	fi
	printf '#include <limits.h>\nint probe[INT_MAX > 0 ? 1 : -1];\n' > /build/limits_probe.c
	if ! /usr/bin/gcc -E /build/limits_probe.c > /dev/null; then
		echo "gcc: seed repair failed -- C preprocessor still cannot resolve limits.h" >&2
		exit 1
	fi
	if [ -x /usr/bin/g++ ] && ! /usr/bin/g++ -E /build/limits_probe.c > /dev/null; then
		echo "gcc: seed repair failed -- C++ preprocessor still cannot resolve limits.h" >&2
		exit 1
	fi
	rm -f /build/limits_probe.c

	#
	# -O0 for the compiler being built, deliberately, as an experiment
	# (issue #116).
	#
	# What is established: gcc-4.7.4 (this seed) emits CORRECT varargs
	# code -- a program exercising va_arg over ints, doubles, mixed
	# pairs, and enough arguments to spill past the register save area
	# runs correctly when compiled by it, and by TCC too. So this is not
	# "varargs codegen is broken" in any simple sense.
	#
	# What is broken is the gcc-6.4.0 that 4.7.4 produces: its cc1 hangs
	# forever expanding va_arg, on a three-line file. It burns no CPU
	# while stuck -- blocked in __futex_wait, single-threaded, which is
	# a lock it can never win rather than a loop. That is the signature
	# of memory corruption in cc1, which is what a miscompile looks
	# like from the outside.
	#
	# If the corruption comes from 4.7.4's optimiser, building this
	# unoptimised makes it go away, and that is worth knowing twice
	# over: it says where the fault is, and it yields a usable (if
	# slow) gcc-6.4.0 to carry the chain forward to 12.5.0.
	#
	# If it hangs anyway, the fault is not optimisation-dependent and
	# the next suspect is the 4.7.4 -> 6.4.0 pairing itself rather than
	# a single bad pass.
	#
	#
	#
	# CXXFLAGS carries -fno-threadsafe-statics, and the mechanism it
	# defeats is now fully established (issue #116, final diagnosis).
	#
	# GCC 4.7's libstdc++ guard machinery (libsupc++/guard.cc)
	# deadlocks on function-local static initialization under glibc >=
	# 2.34 -- REGARDLESS of which compiler built it. A 3-stage
	# bootstrapped, byte-compared 4.7.4's own libstdc++ hangs a
	# five-line twice-entered-static test at -O0 and -O2 exactly like
	# the TCC-seeded one did, while rol64, varargs, the limits chain
	# and iostream/locale all pass. glibc 2.34 folded pthreads into
	# libc.so.6, so that era's weak-symbol __gthread_active_p answers
	# "threaded" in every process, taking a futex guard path that
	# misbehaves single-threaded; the observed futex value 0x10100 is
	# guard.cc's own PENDING|WAITING.
	#
	# This compiler's cc1/cc1plus are linked by the host g++ against
	# that 4.7-era libstdc++.a, so any twice-entered guarded static in
	# THEIR code deadlocks -- which is precisely what hung 6.4.0's and
	# 9.5.0's builds. -fno-threadsafe-statics removes guard emission
	# from the host tools entirely: no inline checks, no __cxa_guard_*
	# calls. Semantically exact for single-threaded compilers, and
	# documented by GCC for the purpose. libstdc++'s own internal
	# statics test clean (iostream/locale battery above); 9.5.0's
	# target libstdc++ is built by 9.5.0 itself; 16.2.0's bootstrap
	# links its own modern runtime from stage 2 on.
	#
	# Worth recording: three earlier guard reproducers passed by
	# accident -- they linked libstdc++ DYNAMICALLY, resolving to the
	# build sandbox's untracked modern copy instead of ours. The
	# five-second reproducer only fires with -static-libstdc++, which
	# is exactly how cc1 links. A probe that tests the wrong library is
	# worse than none.
	#
	#
	# Header installs through cp, not tar, fixed at the source (#122):
	# make install copies include-fixed to DESTDIR through a
	# tar -cf - | tar -xf - pipeline, and this project's tar archives
	# only its first argument while exiting 0. That is why 9.5.0-5
	# shipped ZERO include-fixed entries and the next build's configure
	# died on glibc's include_next finding nothing -- the same #55
	# signature, one rung up. Same one-line source fix as 4.7.4-21.
	#
	if ! grep -q 'build_install_headers_dir=install-headers-tar' ../gcc/config.build; then
		echo "gcc: config.build default changed -- actual line:" >&2
		grep 'install_headers' ../gcc/config.build >&2 || true
		exit 1
	fi
	sed -i 's/build_install_headers_dir=install-headers-tar/build_install_headers_dir=install-headers-cp/' \
		../gcc/config.build

	CC=/usr/bin/gcc CXX=/usr/bin/g++ \
		CXXFLAGS="-g -O2 -fno-threadsafe-statics" \
		../configure --prefix=/usr \
		--disable-bootstrap --disable-multilib --enable-languages=c,c++ \
		--disable-libsanitizer --disable-lto --with-isl=no \
		--with-system-zlib --disable-nls

	# Same real, already-documented shebang-script-vs-binfmt_script gap
	# gcc/16.2.0's and gcc/4.7.4's own recipes already fixed -- ported
	# verbatim, not re-diagnosed.
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
	# Re-pinned to -3: -2's own real failure (past the limits.h fix,
	# gmp's own configure genuinely succeeded this time) never actually
	# showed its real cause -- cixd's own 3800-byte tail-capture
	# window was entirely swamped by gmp's own extremely verbose
	# configure output (hundreds of "config.status: linking ..." lines),
	# pushing the actual failing command/error out of the captured
	# window before this recipe's own config.log-only error dump even
	# ran (which also only catches configure-time errors, not a real
	# compile failure -- gmp's own configure had already succeeded, so
	# there was nothing there to find either). Redirecting all of `make`
	# own output to a file instead of the live captured stream, then
	# dumping only its own real tail on failure, guarantees the
	# diagnostic actually shows the failure point instead of whatever
	# happened to be active when the fixed-size window filled up.
	#
	# Re-pinned to -5: -4's file-only redirect fixed the diagnostic but
	# made the LIVE stream (`cixctl pkg build-log`) completely silent
	# for the entire make phase -- 60-120 real minutes for a full gcc
	# build -- which is exactly what got -4's own healthy, progressing
	# 71-minute run misread as hung and cancelled by hand (confirmed
	# from the box's own audit trail: DELETE /v1/containers/__pkgbuild-1
	# and the container's SIGKILL exit share one timestamp, no OOM, no
	# build error -- the kill was external, not a failure). A periodic
	# heartbeat on the live stream (log line count + the last line, so
	# a genuinely-wedged make is still distinguishable by its counter
	# freezing) restores live observability without giving back the
	# swamped-capture-window problem the file redirect solved.
	#
	# A progress reporter that reports progress it has not made is
	# worse than no reporter at all.
	#
	# 6.4.0-5 printed "[make progress] N log lines; last: ..." every 30
	# seconds unconditionally. This build then wedged -- cc1 deadlocked
	# in __futex_wait compiling one libgcc conftest.c -- and sat there
	# for an hour looking healthy: N stayed frozen at 6887 and the last
	# line never changed, but the heartbeat kept writing, so the
	# daemon's own stall detection (last_output_seconds_ago, issue #58)
	# saw fresh output every 30 seconds and had nothing to report. The
	# loop already computed the line count, so it always knew nothing
	# had happened; it just said so in a way that read as life.
	#
	# Now it speaks only when something changed, and after 20 minutes
	# of genuine silence it declares the build stalled, dumps what
	# every build process is actually blocked on, and kills it. A hang
	# becomes a diagnosed failure instead of an infinite wait.
	#
	# -j2, one job per core, and this is a deliberate experiment as much
	# as a correction.
	#
	# -6 hung three times in the same place: 6887 log lines in, cc1
	# blocked in __futex_wait compiling a libgcc conftest, every process
	# above it in do_wait. Two causes fit that. It could be a race, in
	# which case -j6 on a 2-core box -- 3x oversubscription, which this
	# recipe's own history already records as having hung the entire
	# host once at -j4 -- is the obvious suspect. Or the compiler stage1
	# just produced is itself broken, in which case parallelism is
	# irrelevant and gcc-4.7.4 cannot correctly build gcc-6.4.0 at all.
	#
	# Running one job per core separates them. If it completes, it was
	# concurrency. If it stalls at the same point, the seed compiler is
	# producing broken output -- which would also explain gcc-12.5.0's
	# own deterministic segfault in fold_binary_loc (#116), since that
	# was built by the same 4.7.4.
	#
	# -j2 is the right setting regardless of which way that lands: the
	# box has exactly 2 CPUs, and 3x oversubscription buys nothing on a
	# build that is CPU-bound.
	#
	# Foreground make, no background orchestration -- and the killer of
	# 9.5.0-2 and -3, finally identified, was two lines of our own
	# cleanup. The old plumbing ended:
	#
	#     set -e
	#     kill "$fixup_pid" 2>/dev/null
	#     wait "$fixup_pid" 2>/dev/null
	#
	# The fixup pair sat OUTSIDE the set +e region: `wait` on the
	# just-TERMed helper returns 143, set -e exits the recipe with 143,
	# and the container reports what is indistinguishable from being
	# killed by SIGTERM -- because 143 is 128+15 either way. The decode
	# said "killed by signal 15" and was almost right: it was a
	# PROPAGATED wait status, not a delivered signal, which is why no
	# external actor could ever be found. Deterministic, silenced by
	# its own 2>/dev/null, and firing at exactly make-completion. The
	# third appearance of this same set-e-vs-cleanup bug (4.7.4-10 and
	# the 6.4.0 lineage before it) -- and this time the plumbing that
	# hosts it is simply removed. The daemon's own stall reporter
	# (v1.99.6) covers the job the background heartbeat existed for;
	# make output goes straight to the captured build log.
	#
	make_rc=0
	# LIMITS_H_TEST=true: the 9.5 lineage never carried the #55 fix, so
	# its build tree held a glimits-only limits.h (INT_MAX works, no
	# include_next, POSIX limits unreachable). Same verified-fact
	# override as 4.7.4's bootstrap line.
	make -j"$(nproc)" all LIMITS_H_TEST=true || make_rc=$?
	kill "$fixup_pid" 2>/dev/null || true
	wait "$fixup_pid" 2>/dev/null || true
	if [ "$make_rc" -ne 0 ]; then
		echo "=== make failed with rc=$make_rc ==="
		exit 1
	fi

	#
	# Prove the compiler that was just built actually compiles, and
	# that its output is correct.
	#
	# Issue #116 is the whole reason this exists. `make` returning 0
	# has twice now produced a compiler that could not compile: one
	# that segfaulted folding a 64-bit rotate, and one that hung
	# forever expanding va_arg. Both shipped, because nothing checked.
	#
	# The two cases below are exactly those two failures. rol64() is
	# Linux's own rotate from include/linux/bitops.h, which every
	# kernel build reaches immediately. The varargs case walks int,
	# double and mixed pairs, with enough arguments to spill past the
	# x86-64 register save area -- the part a broken va_arg expander
	# gets wrong. Both are RUN, not merely compiled: a compiler that
	# emits wrong code is no better than one that crashes.
	#
	# `timeout` matters as much as the exit status. The va_arg failure
	# was a hang, not an error, so a self-test without one would itself
	# hang and inherit the exact problem it is meant to catch.
	#
	#
	# The echoes below exist because 9.5.0-2 died here SILENTLY: make
	# completed (make.log ends with make[1] leaving the build dir,
	# libstdc++ and libitm fully built by the NEW compiler -- the guard
	# fix demonstrably working), pkg-dest was empty, the self-test
	# files were never created, and the container exited 143 with no
	# captured output after the last heartbeat. Something delivered
	# SIGTERM in this cleanup-to-self-test window; no audit-logged API
	# call, no OOM, no stall, no daemon restart. If it recurs, these
	# markers pin the exact line. The self-test RUN also gains the
	# timeout it should always have had -- a hang there previously had
	# nothing bounding it.
	#
	echo "=== gcc 9.5.0-6: make complete, entering self-test"
	cat > /build/selftest.c <<'SELFTEST'
#include <stdarg.h>

typedef unsigned long long u64;

static u64 rol64(u64 word, unsigned int shift)
{
	return (word << (shift & 63)) | (word >> ((-shift) & 63));
}

static int isum(int n, ...)
{
	va_list v;
	int s = 0, i;

	va_start(v, n);
	for (i = 0; i < n; i++)
		s += va_arg(v, int);
	va_end(v);
	return s;
}

static double dsum(int n, ...)
{
	va_list v;
	double s = 0;
	int i;

	va_start(v, n);
	for (i = 0; i < n; i++)
		s += va_arg(v, double);
	va_end(v);
	return s;
}

int main(void)
{
	if (rol64(1ULL, 1) != 2ULL)
		return 1;
	if (rol64(0x8000000000000000ULL, 1) != 1ULL)
		return 2;
	if (isum(9, 1, 2, 3, 4, 5, 6, 7, 8, 9) != 45)
		return 3;
	if (dsum(9, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0) != 9.0)
		return 4;
	return 0;
}
SELFTEST
	for opt in -O0 -O2; do
		if ! timeout 120 ./gcc/xgcc -B./gcc/ $opt /build/selftest.c -o /build/selftest; then
			echo "gcc: the compiler just built cannot compile the self-test at $opt" >&2
			echo "gcc: (a timeout here means it HUNG -- see issue #116)" >&2
			exit 1
		fi
		echo "=== self-test compiled at $opt, running"
		if ! timeout 60 /build/selftest; then
			echo "gcc: the compiler just built miscompiles the self-test at $opt (exit $?)" >&2
			exit 1
		fi
	done
	rm -f /build/selftest.c /build/selftest
	echo "gcc 9.5.0: self-test passed at -O0 and -O2"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	# include-fixed must survive installation, composed. Belt and
	# braces over the config.build fix above, with an assert so a
	# regression is a one-line diagnosis instead of a downstream
	# configure death: copy the build tree's include-fixed if install
	# missed it, compose limits.h from the source tree's own three
	# files if the chain is absent, and refuse to ship without it.
	triplet_fx=$(ls "$PKG_DESTDIR/usr/libexec/gcc" | head -1)
	fx="$PKG_DESTDIR/usr/lib/gcc/$triplet_fx/9.5.0/include-fixed"
	mkdir -p "$fx"
	if [ -d ./gcc/include-fixed ]; then
		cp -a ./gcc/include-fixed/. "$fx/"
	fi
	if [ ! -f "$fx/limits.h" ] || ! grep -q include_next "$fx/limits.h"; then
		cat ../gcc/limitx.h ../gcc/glimits.h ../gcc/limity.h > "$fx/limits.h"
	fi
	[ -f "$fx/syslimits.h" ] || cp ../gcc/gsyslimits.h "$fx/syslimits.h"
	grep -q include_next "$fx/limits.h" || {
		echo "gcc: installed include-fixed/limits.h has no include_next chain" >&2
		exit 1
	}

	# Same real gap gcc/4.7.4's own recipe already found and fixed:
	# gcc's own freestanding compiler-provided headers (stddef.h et al)
	# don't reliably reach DESTDIR via a plain `make install` for this
	# configuration -- defensively copy the build tree's own
	# already-processed ./gcc/include/ directly, same proven fix.
	# Derive the triplet from what make install actually produced,
	# instead of hardcoding it. -4 failed exactly here: the paths were
	# inherited from the 4.7.4 lineage (x86_64-unknown-linux-gnu, and
	# one leftover 6.4.0), but 9.5.0 configures and installs as
	# x86_64-pc-linux-gnu, so the ln target directory did not exist.
	# The compiler, self-test and make install had all already
	# succeeded -- only these two post-install steps used the wrong
	# name. Reading the tree removes the class of error.
	triplet=$(ls "$PKG_DESTDIR/usr/libexec/gcc" | head -1)
	[ -n "$triplet" ] || { echo "gcc: no triplet dir under libexec/gcc after install" >&2; exit 1; }
	install_include_dir="$PKG_DESTDIR/usr/lib/gcc/$triplet/9.5.0/include"
	mkdir -p "$install_include_dir"
	cp -a ./gcc/include/. "$install_include_dir/"

	# Same real gap gcc/16.2.0's and gcc/4.7.4's own recipes already
	# found and fixed: collect2 looks for `ld` via its own private
	# COMPILER_PATH search, not $PATH.
	mkdir -p "$PKG_DESTDIR/usr/libexec/gcc/$triplet/9.5.0"
	ln -sf /usr/bin/ld "$PKG_DESTDIR/usr/libexec/gcc/$triplet/9.5.0/ld"
}
