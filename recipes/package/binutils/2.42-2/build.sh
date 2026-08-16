#
# binutils -- the GNU binary utilities: as (assembler), ld/ld.bfd
# (linker), ar/ranlib (static archives), objcopy/objdump/readelf/nm/
# size/strings/strip/addr2line/c++filt/elfedit/gprof (object-file
# inspection and manipulation). The second dev-toolchain recipe (after
# m4) -- gcc.recipe needs a real as/ld present in the target image to
# actually produce working binaries; without this, a staged gcc could
# compile but not link anything.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256.
#
pkg_name="binutils"
pkg_version="2.42-2"
pkg_source="https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz"
pkg_sha256="f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800"
pkg_depends=""

# Out-of-tree build (binutils' own documented convention, avoids a real,
# known issue building certain subdirs in-tree). --disable-gold (the
# alternative ELF-only linker; ld.bfd already covers every real use
# case this project has, and gold needs a C++ toolchain this recipe set
# doesn't assume yet) and --disable-gprofng (a newer profiler needing
# extra deps this project has no use for) keep the build to the tools
# actually needed. MAKEINFO=true skips texinfo doc generation --
# `makeinfo` isn't part of this toolchain and none of this project's
# own images ship info pages for anything else either (same reasoning
# iputils.recipe's own -DBUILD_MANS=false already used).
#
# Re-pinned to -2: `bfd/bfd.c` (`static TLS bfd_error_type bfd_error;`
# et al) failed under TCC with "';' expected (got \"bfd_error_type\")".
# Two blind guesses (CPPFLAGS="-DTLS=", a single post-configure sed
# targeting a `#define TLS ...` line) both had zero effect -- a
# diagnostic-only build (dumping every generated config.h's own TLS-
# related lines) found the real, exact shape: `bfd/config.h` carries
#
#   /* If the compiler supports a TLS storage class, define it to that here */
#   /* #undef TLS */
#
# -- autoconf's own real, standard "detected as NOT available, left
# commented out" convention. `TLS` is genuinely undefined, not defined
# to `__thread` or anything else -- so the preprocessor leaves the bare
# identifier `TLS` untouched in `static TLS bfd_error_type ...`, which
# TCC's *parser* (not the preprocessor) then correctly rejects as
# invalid syntax: a plain undeclared identifier can't appear between
# `static` and a type name. Real GCC/Clang builds never hit this
# because their own TLS-storage-class autoconf probe succeeds and
# defines `TLS` to `__thread`; something about how the probe exercises
# TCC makes it conclude "no support" without also emitting the
# empty-string fallback binutils' upstream probably expects a genuinely
# TLS-incapable compiler to still receive. Confirmed via ldd against
# this build's own real output was never wrong about what a
# successful, already-built binutils looks like -- what's new is that
# this exact TCC/autoconf-probe interaction was apparently never
# exercised before this session, on this exact bootstrap toolchain.
#
# `bfd/config.h` is generated lazily, per subdirectory, as `make`
# descends into it (binutils' own real, older "Cygnus tree" multi-
# directory build convention) -- not eagerly by the top-level
# `../configure`. Fixed with the same bounded retry loop as before,
# now with the CORRECT sed target (`/* #undef TLS */` -> `#define TLS`,
# matching the exact comment-out convention above, not a `#define TLS
# ...` line that was never actually present) applied to every
# config.h that exists on disk so far after each failed attempt --
# `find`, not a hardcoded path, since other subdirectories (opcodes/,
# etc.) plausibly hit the identical gap once make reaches them. make's
# own dependency tracking is resumable, so each retry picks up exactly
# where the previous one left off.
pkg_build() {
	mkdir -p build
	cd build
	CC=tcc ../configure --prefix=/usr --disable-multilib --disable-gold \
		--disable-gprofng --enable-deterministic-archives
	i=0
	while [ "$i" -lt 10 ]; do
		if make -j"$(nproc)" MAKEINFO=true; then
			break
		fi
		find . -name config.h -exec sed -i 's|/\* *#undef TLS *\*/|#define TLS|' {} +
		i=$((i + 1))
	done
	make -j"$(nproc)" MAKEINFO=true
}

# Confirmed via ldd against every one of the 16 real tools this build
# produces: every single one links against nothing but libc -- binutils
# statically links its own libbfd/libopcodes/libctf/libsframe into each
# tool, so no extra runtime libraries need staging here (unlike most of
# this project's other recipes). Static archives (.a/.la), headers, and
# docs (info/man/locale) are dropped -- nothing else in this project's
# image set links against libbfd directly. The triplet-prefixed
# duplicate copies under usr/x86_64-pc-linux-gnu/{bin,lib} (binutils'
# own cross-compilation convention) are skipped too -- this project
# never cross-compiles, so only the plain usr/bin/* names are staged.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/x86_64-pc-linux-gnu" "$PKG_DESTDIR/usr/share" \
	       "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib"/*.a "$PKG_DESTDIR/usr/lib"/*.la
}
