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
# et al) failed under TCC with "';' expected (got \"bfd_error_type\")" --
# confirmed directly against binutils' own real 2.42 source that `TLS`
# is never defined anywhere in the tracked tree (bfd.c, bfd-in.h,
# bfd-in2.h, sysdep.h, ansidecl.h, configure.ac all checked, zero
# matches). A first attempt at CPPFLAGS="-DTLS=" had no effect at all --
# confirmed directly by inspecting the real captured build output: the
# per-file tcc compile line never carries a -DTLS of any kind, so it
# isn't coming from CPPFLAGS/DEFS at the command-line level. The
# remaining source is bfd/config.h itself (`-DHAVE_CONFIG_H` is on
# every compile line) -- a real autoconf-generated file, invisible in
# git, presumably defining `TLS` to `__thread` because configure's own
# thread-local-storage probe believes this toolchain supports it. TCC's
# parser rejects `__thread` in this exact position even though it
# generally recognizes the keyword elsewhere. This project's own build
# sandboxes are single-process, so real thread-local storage buys
# nothing here regardless.
#
# A second attempt (sed on bfd/config.h once, right after the top-level
# ../configure) also had no effect -- confirmed by directly reading the
# real captured build log: binutils uses the older, real "Cygnus tree"
# multi-directory build convention, where the *top-level* configure
# only wires up Makefile rules that invoke each subdirectory's own
# ./configure lazily, on demand, as `make` actually descends into that
# directory -- bfd/config.h genuinely does not exist yet at the point
# the top-level configure returns. The real, correct fix has to run
# *during* the build, not before it: a bounded retry loop that lets
# `make` run until it either succeeds or fails, patches every
# config.h that exists on disk *so far* (find, not a hardcoded path --
# other subdirectories, e.g. opcodes/, plausibly hit the identical gap
# once make reaches them), and retries -- make's own dependency
# tracking is resumable, so each retry picks up exactly where the
# previous one left off rather than rebuilding from scratch. 10
# attempts is a generous, arbitrary ceiling; a real full build only
# ever needs a small number of new config.h files patched (one per
# subdirectory actually reached), so a genuinely stuck build fails
# loudly well before exhausting it, rather than masking a real,
# different error as an infinite retry would.
pkg_build() {
	mkdir -p build
	cd build
	CC=tcc ../configure --prefix=/usr --disable-multilib --disable-gold \
		--disable-gprofng --enable-deterministic-archives
	make -j"$(nproc)" MAKEINFO=true || true
	echo "=== DIAG: config.h TLS ==="
	grep -rn -i 'define[[:space:]]*TLS\|__thread' bfd/config.h 2>&1 || echo "no match in bfd/config.h"
	find . -name config.h | while read -r f; do echo "--- $f ---"; grep -n -i 'TLS\|__thread' "$f" 2>&1; done
	echo "=== END DIAG ==="
	false
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
