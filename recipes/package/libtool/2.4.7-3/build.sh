#
# libtool -- GNU Libtool: libtool/libtoolize, plus libltdl (the
# portable dlopen()-abstraction runtime library some programs link
# against directly). test/test_image_fixture.c's own extras[] already
# stages the equivalent content into the isolated BUILD sandbox
# (procps.recipe's own autogen.sh chain needed it); this recipe makes
# it a real, installable runtime package for a target image instead.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256.
#
pkg_name="libtool"
pkg_version="2.4.7-3"
pkg_source="https://ftp.gnu.org/gnu/libtool/libtool-2.4.7.tar.xz"
pkg_sha256="4f7f217f057ce655ff22559ad221a0fd8ef84ad1fc5fcb6990cecc333aa1635d"
pkg_depends="bash m4"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils m4"
pkg_changelog="2.4.7-3: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 2.4.7-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

# Plain autotools, confirmed directly.
pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

# Real, load-bearing runtime dependency confirmed via shebang
# inspection: libtool itself is #!/bin/bash (pkg_depends="bash" above
# -- already a real recipe in this project's set); libtoolize is
# #!/usr/bin/env sh, satisfied by any real shell (bash provides
# /bin/sh too). Both also invoke m4 internally the same way bison does
# (pkg_depends="... m4"). usr/share/aclocal/lt*.m4 (the macros
# AC_PROG_LIBTOOL/LT_INIT expand) and usr/share/libtool/build-aux/*
# (the real ltmain.sh/config.guess/config.sub/etc. templates
# libtoolize --install copies into any project using it) are
# load-bearing runtime data, not documentation, kept in full -- the
# same reasoning autoconf.recipe/automake.recipe's own usr/share
# staging already established. libltdl (usr/lib/libltdl.so.7.3.2 +
# its SONAME symlink chain, confirmed via ldd to link against nothing
# but libc) is kept for any program that links -lltdl directly; its
# static archive and headers are dropped, matching every other
# recipe's own static/dev-header stripping.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/info" "$PKG_DESTDIR/usr/share/man" \
	       "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib/libltdl.a" "$PKG_DESTDIR/usr/lib/libltdl.la"
}
