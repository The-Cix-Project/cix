#
# gawk -- GNU awk. Found as a real, hard runtime dependency while
# writing autoconf.recipe: autoconf's own AC_PROG_AWK check hardcodes
# the bare command name "gawk" (found ahead of mawk/nawk/awk in this
# build host's own PATH, autoconf's standard search order) into the
# generated autoconf script -- a target image with autoconf but no
# gawk would fail the first time anything actually ran, not at install
# time.
#
# Source is GNU's own canonical ftp.gnu.org release, unchanged from
# 5.3.0, checksum verified against two independent mirrors
# (ftp.gnu.org and mirrors.kernel.org) -- byte-identical, same sha256.
#
pkg_name="gawk"
pkg_version="5.3.0-2"
pkg_source="https://ftp.gnu.org/gnu/gawk/gawk-5.3.0.tar.gz"
pkg_sha256="378f8864ec21cfceaa048f7e1869ac9b4597b449087caf1eb55e440d30273336"
pkg_depends=""

# 5.3.0's own bare `CC=tcc ./configure` failed only at the final link
# (every .o compiled clean, no gnulib static-inline "defined twice"
# class of error this time -- gawk doesn't lean on gnulib the way
# m4/sed/coreutils/grep/bison do): `tcc: error: undefined symbol
# '__dso_handle'`, the same real, environment-specific bare-tcc-link
# CRT gap sysklogd/m4/sed/coreutils/grep/bison all already document.
# Fixed the same proven way: a `weak` stub object passed as a bare
# object-file path in LIBS (never `-lxxx`, which m4.recipe's own trail
# already found gets conditionally extracted away for a weak-only
# archive member on this toolchain).
pkg_build() {
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o

	CC=tcc ./configure --prefix=/usr LIBS="$(pwd)/dso_stub.o"
	make -j"$(nproc)" MAKEINFO=true
}

# Confirmed via ldd: gawk links against libreadline.so.8 (interactive
# line editing for gawk's own -- rare but real -- interactive mode) and
# libtinfo.so.6 (readline's own terminal-capability dependency,
# already part of pkg_seed_image_baseline()'s global runtime set, not
# copied again here), plus libm/libc. libreadline is staged the same
# two-file SONAME-symlink-plus-real-target pattern bird.recipe's own
# readline staging already established. gawk's own loadable extension
# modules (usr/lib/gawk/*.so -- filefuncs, fnmatch, fork, time, etc.,
# real optional @load-able gawk features, not build artifacts) and its
# grcat/pwcat helper binaries are kept; usr/include (the C extension
# API header, nothing in this project's own image set writes gawk
# extensions) and usr/etc/profile.d (a shell-login-profile convention
# this project's images don't use anywhere else) are dropped.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/etc"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a /lib/x86_64-linux-gnu/libreadline.so.8 /lib/x86_64-linux-gnu/libreadline.so.8.2 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
