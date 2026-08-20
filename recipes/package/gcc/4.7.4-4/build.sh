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
pkg_version="4.7.4-4"
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
	CC=tcc ../configure --prefix=/usr \
		--disable-bootstrap --disable-multilib --enable-languages=c \
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
	fixup_exec_tools
	( while true; do fixup_exec_tools; sleep 2; done ) &
	fixup_pid=$!
	make -j"$(nproc)"
	make_rc=$?
	kill "$fixup_pid" 2>/dev/null
	wait "$fixup_pid" 2>/dev/null
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
}
