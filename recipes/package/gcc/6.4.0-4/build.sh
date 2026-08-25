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
pkg_name="gcc"
pkg_version="6.4.0-4"
pkg_source="https://ftp.gnu.org/gnu/gcc/gcc-6.4.0/gcc-6.4.0.tar.xz https://gcc.gnu.org/pub/gcc/infrastructure/gmp-4.3.2.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpfr-2.4.2.tar.bz2 https://gcc.gnu.org/pub/gcc/infrastructure/mpc-0.8.1.tar.gz"
pkg_sha256="850bf21eafdfe5cd5f6827148184c08c4a0852a37ccf36ce69855334d2c914d4 936162c0312886c21581002b79932829aa048cfaf9937c6265aeaa14f1cd1775 c7e75a08a8d49d2082e4caee1591a05d11b9d5627514e678f02d66a124bcf2ba e664603757251fd8a352848276497a4c79b7f8b21fd8aedd5cc0598a38fee3e4"
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

	bzip2 -dc /build/extra/gmp-4.3.2.tar.bz2 > /build/gmp.tar
	/build/miniextract /build/gmp.tar && mv gmp-4.3.2 gmp
	bzip2 -dc /build/extra/mpfr-2.4.2.tar.bz2 > /build/mpfr.tar
	/build/miniextract /build/mpfr.tar && mv mpfr-2.4.2 mpfr
	gzip -dc /build/extra/mpc-0.8.1.tar.gz > /build/mpc.tar
	/build/miniextract /build/mpc.tar && mv mpc-0.8.1 mpc
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
	CC=/usr/bin/gcc CXX=/usr/bin/g++ ../configure --prefix=/usr \
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
	make -j6 all > /build/make.log 2>&1
	make_rc=$?
	kill "$fixup_pid" 2>/dev/null
	wait "$fixup_pid" 2>/dev/null
	if [ "$make_rc" -ne 0 ]; then
		echo "=== tail of /build/make.log (real failure point) ==="
		tail -n 100 /build/make.log
		exit 1
	fi
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	# Same real gap gcc/4.7.4's own recipe already found and fixed:
	# gcc's own freestanding compiler-provided headers (stddef.h et al)
	# don't reliably reach DESTDIR via a plain `make install` for this
	# configuration -- defensively copy the build tree's own
	# already-processed ./gcc/include/ directly, same proven fix.
	install_include_dir="$PKG_DESTDIR/usr/lib/gcc/x86_64-unknown-linux-gnu/6.4.0/include"
	mkdir -p "$install_include_dir"
	cp -a ./gcc/include/. "$install_include_dir/"

	# Same real gap gcc/16.2.0's and gcc/4.7.4's own recipes already
	# found and fixed: collect2 looks for `ld` via its own private
	# COMPILER_PATH search, not $PATH.
	mkdir -p "$PKG_DESTDIR/usr/libexec/gcc/x86_64-unknown-linux-gnu/6.4.0"
	ln -sf /usr/bin/ld "$PKG_DESTDIR/usr/libexec/gcc/x86_64-unknown-linux-gnu/6.4.0/ld"
}
