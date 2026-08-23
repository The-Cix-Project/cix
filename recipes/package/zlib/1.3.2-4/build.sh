#
# zlib -- the DEFLATE compression library. A genuinely missing
# dependency confirmed empirically (ADR-0056): elfutils' libelf.so
# (elfutils.recipe) links against zlib for compressed-ELF-section
# support. Nothing else in this recipe catalog happened to need it as
# a target-image package before -2.
#
# Source is zlib's own canonical zlib.net release.
#
pkg_name="zlib"
pkg_version="1.3.2-4"
pkg_source="https://zlib.net/zlib-1.3.2.tar.gz"
pkg_sha256="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
# Nothing at runtime: libz.so has no dependency of its own beyond libc.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD, declared
# rather than inherited from whatever happened to be in a shared
# sandbox. The build container is composed from exactly these and
# nothing else, so this list is not documentation -- it is the
# environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with CC=tcc below, and
#             /usr/lib/tcc/libtcc1.a which its LDFLAGS names directly
#   make      the build itself
#   libc-dev  headers and libc to compile and link against
#   bash      zlib's configure is a hand-written /bin/sh script
#   coreutils configure and the Makefile use rm/cp/mkdir/nproc/cat
#   sed       configure edits its own Makefile with it
#
# Verified sufficient by building it: an environment holding exactly
# these produced libz.a. Verified NOT over-declared by review, which is
# the honest limit -- sufficiency is enforced by the build, minimality
# is not (see ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed"

# -2's own libz.so.1 has a real, previously-undiscovered gap: the
# kernel 6.18.40-11 hostbuild's own tools/objtool link (real gcc,
# consuming elfutils' libelf.so, which itself links against libz.so.1)
# failed with `/usr/lib/libz.so.1: undefined reference to
# '__va_start'`. Root-caused directly on the real box (probe-gcc-
# headers v7 -- do not extrapolate from a dev sandbox's own local tcc,
# confirmed DIFFERENT further down): `nm -D /usr/lib/libz.so.1` shows
# `__va_start` as a genuine undefined (U) dynamic symbol -- TCC
# implements `va_start`/`va_arg` (zlib's own gzprintf() is a real
# varargs user) via calls to its own runtime helpers
# (`__va_start`/`__va_arg`, both real, confirmed-present symbols in
# the real box's own `/usr/lib/tcc/libtcc1.a`, `va_list.o` member) --
# NOT compiler builtins the way GCC does. Building a SHARED library
# (`-shared`) doesn't auto-link libtcc1.a the way TCC's own EXECUTABLE
# link path does, so these two symbols are left genuinely unresolved
# inside libz.so.1 itself -- harmless as long as every consumer is
# ALSO tcc-linked (tcc's own final executable link auto-adds
# libtcc1.a, transitively resolving them), but fatal the moment a real
# gcc-linked consumer (tools/objtool) tries to use it, since gcc has
# no notion of libtcc1.a at all.
#
# Real, minimal, fix-at-the-source solution (not a kernel-recipe-local
# workaround, so every future gcc-linked consumer of libz.so.1 is safe
# too): pass libtcc1.a as a bare archive path via LDFLAGS at configure
# time -- zlib's own configure captures ambient $LDFLAGS into the
# generated Makefile verbatim, which the shared-lib link rule
# (Makefile.in's own `$(LDSHARED) $(SFLAGS) -o $@ $(PIC_OBJS)
# $(LDSHAREDLIBC) $(LDFLAGS)`) appends at the very end. A static
# archive only pulls in the specific member(s) needed to resolve an
# otherwise-undefined symbol (confirmed: libtcc1.a's own va_list.o is
# the only member providing __va_start/__va_arg, so this can't drag in
# anything unrelated) -- this permanently, statically resolves both
# symbols INTO libz.so.1 itself, the same "supply the missing runtime
# piece directly, bare archive/object path, never -lxxx" technique
# this recipe catalog's own __dso_handle weak-stub fix already
# established for sed/coreutils/grep/bison/gawk/m4.
pkg_build() {
	CC=tcc LDFLAGS="/usr/lib/tcc/libtcc1.a" ./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/lib"/*.a
}
