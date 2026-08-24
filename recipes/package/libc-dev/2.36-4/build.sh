#
# libc-dev -- glibc's own development files: headers (stdio.h,
# stdlib.h, string.h, etc.) and the real C-runtime startup/teardown
# object files (crt1.o/crti.o/crtn.o and friends) every dynamically-
# linked ELF binary on this platform needs at link time. Found as a
# real, missing gap only by actually trying to compile a real C program
# with the newly-built gcc.recipe inside a real running container --
# every prior recipe in this project only ever produced a *finished*,
# already-linked binary (built inside the isolated sandbox, using the
# sandbox's own real glibc-dev), so nothing before this recipe ever
# needed glibc's own dev files present in a *target* image. gcc.recipe
# supplies its own crtbegin.o/crtend.o and libgcc -- those are GCC's
# own files, real and already staged there; crt1.o/crti.o/crtn.o and
# the libc headers below are glibc's, a genuinely separate package on
# every real distro (libc6-dev on Debian) and equally separate here.
#
# Building glibc itself from source is a real, giant undertaking of
# its own -- a multi-day bootstrap with its own kernel-header/build-
# system intricacies, squarely out of scope for this recipe set, the
# same "too complex to reasonably build here, stage the real
# pre-built host copy instead" call this project already made for the
# Go/Rust toolchains (test/test_image_fixture.c's own extras[],
# staged for the BUILD sandbox's own use). This recipe makes the
# equivalent real files available in a *target* image instead, which
# that toolchain-extras mechanism doesn't reach. pkg_source below is
# the real, genuine glibc 2.36 upstream release -- the exact version
# this build host's own already-installed libc6-dev is (confirmed via
# `dpkg -s libc6-dev`) -- fetched and checksum-verified against two
# independent GNU mirrors for real provenance, even though
# pkg_install() below copies the pre-built files directly from this
# build container's own toolchain-provided glibc-dev (not built from
# this tarball -- see pkg_build()'s own comment).
#
pkg_name="libc-dev"
pkg_version="2.36-4"
pkg_source="https://ftp.gnu.org/gnu/libc/glibc-2.36.tar.xz"
pkg_sha256="1c959fea240906226062cb4b1e7ebce71a9f0e3c0836c09e7e3423d434fcfe75"
pkg_depends=""

# Deliberately not a real build -- see this recipe's own header
# comment for why (glibc's own build is its own giant undertaking, out
# of scope). This step exists only to fail loudly if the fetched
# source's own version doesn't match what pkg_install() is about to
# copy from the toolchain sandbox's real glibc-dev, keeping the two in
# sync rather than silently drifting apart.
pkg_build() {
	grep -Eq '#define[[:space:]]+__GLIBC_MINOR__[[:space:]]+36' /usr/include/features.h || {
		echo "toolchain's own glibc-dev is not 2.36 -- update pkg_version/pkg_source above to match" >&2
		exit 1
	}
}

# Real files copied directly from this build container's own
# toolchain-provided glibc-dev (confirmed present via the wholesale
# /usr/include, /usr/lib/x86_64-linux-gnu toolchain copy every other
# recipe's own build already relies on) -- not built from the fetched
# tarball above. crt1.o (the real _start/__libc_start_main entry
# point), crti.o/crtn.o (.init/.fini section prologue/epilogue,
# bracketing gcc's own crtbegin.o/crtend.o), Scrt1.o (the PIE variant,
# what gcc actually links by default -- confirmed via gcc.recipe's own
# `gcc -v` output using non-PIE crt1.o only because this recipe was
# tested with default flags; Scrt1.o staged too since PIE is this
# platform's own real default for anything not passing -no-pie), and
# gcrt1.o (profiling variant, real and small, kept for completeness)
# are all genuinely required at link time -- confirmed the hard way,
# `collect2: fatal error: cannot find 'ld'` then (after symlinking ld
# into gcc's own private prefix directory, a separate real finding --
# see gcc.recipe) `cannot find crt1.o: No such file or directory` on a
# real compile+link attempt inside a real running container. libc.so
# (a small real GNU ld script, not a symlink -- confirmed via `file`,
# `GROUP ( /lib/x86_64-linux-gnu/libc.so.6 ... )`-style, what `-lc`
# actually resolves against at link time) and libc.a (static libc,
# real and legitimate for anything statically linking) are kept too.
# The full real /usr/include tree (88MB, glibc's own headers plus
# every other dev package already installed on this build host --
# openssl, zlib, ncurses, sqlite3, etc.) is staged wholesale rather
# than hand-picking "just glibc's" headers out of it -- the same
# "too fragile to hand-curate perfectly, stage the real complete tree"
# reasoning already applied to gcc's own runtime library staging.
# Destination for the object files/libc.so/libc.a is /lib/x86_64-linux-gnu
# (this project's own real runtime-library path, confirmed via `ls -la`
# that /lib is a genuine directory here, not a /usr/lib symlink the way
# a real usrmerge'd host has it) -- NOT /usr/lib/x86_64-linux-gnu, which
# is where the build host itself keeps them but which gcc's own
# LIBRARY_PATH (confirmed via `gcc -v`) never actually searches on this
# platform; only /lib/x86_64-linux-gnu/ is in that list.
#
# ADR-0057: the crt objects (crt1.o/crti.o/crtn.o/Scrt1.o/gcrt1.o/
# Mcrt1.o) are ALSO staged a second time at /usr/lib/x86_64-linux-gnu --
# confirmed the hard way via a real thinc.recipe hostbuild failing
# `tcc: error: file 'crt1.o' not found` even though the file genuinely
# existed at /lib/x86_64-linux-gnu, then root-caused against tinycc's own
# upstream source (tcc.h's CONFIG_TCC_CRTPREFIX macro, confirmed vanilla,
# not a Debian patch): TCC maintains a separate, single-path "crt:"
# search list independent of its broader "libraries:" list used for
# libc.a/libc.so/-l lookups, and that single path defaults to
# /usr/lib/x86_64-linux-gnu specifically -- a real, structural
# difference from gcc's own LIBRARY_PATH convention, not an oversight in
# either tool. libc.a/libc_nonshared.a/libpthread*.a are NOT duplicated
# there since TCC's broader "libraries:" list (confirmed via `tcc -vv`)
# already includes /lib/x86_64-linux-gnu -- only the crt objects need
# the second copy. libc.so is itself a real GNU ld script, not a symlink (confirmed via `cat`):
# `GROUP ( /lib/x86_64-linux-gnu/libc.so.6 /usr/lib/x86_64-linux-gnu/
# libc_nonshared.a AS_NEEDED (...) )` -- its first path already matches
# this project's own convention, but its second (libc_nonshared.a, a
# small real static archive with the handful of libc symbols that are
# never in the shared lib) points at /usr/lib/x86_64-linux-gnu, which
# doesn't exist on this project's own images. Rather than introduce a
# second, otherwise-unused directory convention just for one file, the
# script is rewritten here to reference /lib/x86_64-linux-gnu for both
# paths, matching where libc_nonshared.a is actually staged below.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/lib/x86_64-linux-gnu" \
	   "$PKG_DESTDIR/usr/lib/x86_64-linux-gnu"
	cp -a /usr/include/. "$PKG_DESTDIR/usr/include/"
	cp -a /usr/lib/x86_64-linux-gnu/crt1.o /usr/lib/x86_64-linux-gnu/crti.o \
	   /usr/lib/x86_64-linux-gnu/crtn.o /usr/lib/x86_64-linux-gnu/Scrt1.o \
	   /usr/lib/x86_64-linux-gnu/gcrt1.o /usr/lib/x86_64-linux-gnu/Mcrt1.o \
	   /usr/lib/x86_64-linux-gnu/libc.a /usr/lib/x86_64-linux-gnu/libc_nonshared.a \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	cp -a /usr/lib/x86_64-linux-gnu/crt1.o /usr/lib/x86_64-linux-gnu/crti.o \
	   /usr/lib/x86_64-linux-gnu/crtn.o /usr/lib/x86_64-linux-gnu/Scrt1.o \
	   /usr/lib/x86_64-linux-gnu/gcrt1.o /usr/lib/x86_64-linux-gnu/Mcrt1.o \
	   "$PKG_DESTDIR/usr/lib/x86_64-linux-gnu/"
	sed 's|/usr/lib/x86_64-linux-gnu/|/lib/x86_64-linux-gnu/|' /usr/lib/x86_64-linux-gnu/libc.so \
	   > "$PKG_DESTDIR/lib/x86_64-linux-gnu/libc.so"
	# -lpthread compat archives: glibc >= 2.34 folded libpthread's
	# functions into libc.so.6 itself and ships no libpthread.so at
	# all anymore (confirmed on this exact host -- only the .a/
	# _nonshared.a static compat archives remain), but real-world
	# build systems (the Linux kernel's own scripts/Makefile.host
	# among them) still pass -lpthread explicitly. Confirmed missing
	# the hard way (ADR-0056): a real kernel hostbuild failed on
	# `cannot find -lpthread` the moment it built its first HOSTCC
	# tool linked with -lpthread (scripts/sorttable).
	cp -a /usr/lib/x86_64-linux-gnu/libpthread.a \
	   /usr/lib/x86_64-linux-gnu/libpthread_nonshared.a \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"

	# -ldl: the identical story one library over, and missed until a
	# build environment held only what it declared. glibc >= 2.34
	# folded libdl into libc.so.6 the same way it folded libpthread,
	# leaving an 8-byte placeholder archive and no libdl.so at all --
	# but real build systems still pass -ldl unconditionally. tcc's own
	# Makefile does (`tcc -o tcc tcc.o libtcc.a -lm -ldl`), so tcc could
	# not link itself: `tcc: error: library 'dl' not found`. Staged
	# exactly like libpthread.a above, for exactly the same reason.
	cp -a /usr/lib/x86_64-linux-gnu/libdl.a "$PKG_DESTDIR/lib/x86_64-linux-gnu/"

	# libm.so: the same real, missing-until-actually-tried gap
	# libpthread.a/libc.so above already document, found the same way --
	# a real g++ link of a genuine C++ program (gcc/4.7.4-6's own
	# validation) failed with `cannot find -lm`, even though the real
	# runtime library it needs (libm.so.6) is already present via
	# pkg_seed_image_baseline()'s own global runtime set. `-lm` (and
	# libstdc++'s own internal use of libm) needs the bare, unversioned
	# *dev* symlink specifically, which nothing before this recipe's own
	# libc.so ever staged. glibc's own real
	# `/usr/lib/x86_64-linux-gnu/libm.so` here is a GNU ld script
	# (confirmed via `file`+`cat`, not a plain symlink):
	# `GROUP ( /lib/.../libm.so.6 AS_NEEDED ( /lib/.../libmvec.so.1 ) )`.
	#
	# -2's own straight path-rewrite of that real script hit a second,
	# real gap immediately: this project's own gcc-built ld (confirmed
	# with gcc/4.7.4's own bundled binutils-era ld, an older release)
	# does NOT treat AS_NEEDED inside a GROUP as genuinely optional the
	# way glibc's own upstream comment intends -- it errors outright,
	# `cannot find libmvec.so.1`, for a library this project never
	# stages (SIMD-vectorized math variants, a real but niche glibc
	# optimization, not architecturally required for `-lm` to work at
	# all). Written out as a plain, minimal GROUP referencing only the
	# real, always-present libm.so.6 -- not a blind rewrite of the
	# upstream script's own text -- since the AS_NEEDED clause has no
	# safe, portable way to express "genuinely optional" across every ld
	# this project's own toolchains might use.
	printf 'GROUP ( /lib/x86_64-linux-gnu/libm.so.6 )\n' \
	   > "$PKG_DESTDIR/lib/x86_64-linux-gnu/libm.so"
}
