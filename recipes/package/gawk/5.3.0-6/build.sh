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
pkg_version="5.3.0-6"
pkg_source="https://ftp.gnu.org/gnu/gawk/gawk-5.3.0.tar.gz"
pkg_sha256="378f8864ec21cfceaa048f7e1869ac9b4597b449087caf1eb55e440d30273336"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/gawk-5.3.0-2.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
# ncurses, not "" -- gawk's binary links libtinfo.so.6, and until
# ADR-0209 it never had to say so: pkg_seed_image_baseline() staged
# libtinfo into every image, so the library was simply always there.
# Reducing the baseline to the glibc floor (ADR-0209 decision 4, on the
# correct grounds that libtinfo is our own ncurses package) removed it,
# and a composed build environment holding gawk but not ncurses then
# produced a gawk that cannot start at all:
#
#   gawk: error while loading shared libraries: libtinfo.so.6:
#         cannot open shared object file: No such file or directory
#
# Found the moment a real recipe's ./configure invoked awk. The build
# environment composer already recurses into a tool's own pkg_depends
# (buildenv_add_tool), so declaring it here is the whole fix and it
# fixes every recipe that reaches for awk, not just the one that
# happened to fail first.
# Empty, and that is the point of building --without-readline. gawk
# links libtinfo only THROUGH readline; with readline gone it needs
# neither, so there is no runtime dependency left to declare and no
# foreign library to ship. -5 declared ncurses because it still linked
# readline; this revision removes the cause rather than the symptom.
#
# It also has to be empty for a different reason worth recording:
# pkg_hostbuild_start() rejects any recipe with pkg_depends set
# (PKG_ERR_INVALID_RECIPE), because a hostbuild is a one-shot harvest
# whose prerequisites must already be in the build image. That is the
# route this version has to take -- the only gawk that exists ships
# Debian's libreadline, so a fresh composed environment cannot run awk
# at all, while cix-builder's older rootfs still can.
pkg_depends=""
# ADR-0199/0209: composed from exactly these, no fallback (#168).
# gawk appears in its own build environment deliberately -- autoconf's
# configure runs awk, and the copy already installed satisfies it.
# ncurses is here as well as in pkg_depends, and the reason is a real
# bootstrap circle rather than belt-and-braces. Building gawk composes
# an environment containing the ALREADY-INSTALLED gawk (configure runs
# awk), and that older copy still records pkg_depends="" -- so the
# composer has no reason to pull ncurses in, libtinfo is absent, and
# the old awk cannot start, which fails the build of the very version
# that would have fixed the declaration. Naming ncurses as a build
# input breaks the circle: the environment gets libtinfo, the old awk
# runs, and the resulting package finally records the runtime
# dependency for everyone downstream.
pkg_build_depends="bash coreutils make tcc libc-dev sed grep gawk ncurses binutils"

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

	# --without-readline: gawk only uses readline for its own interactive
	# mode, which nothing in this project invokes, and linking it was how
	# a foreign library ended up inside a Cix package (see pkg_install
	# below). configure already probed readline as unusable here in every
	# variant it tried, so this makes the existing outcome explicit
	# instead of incidental.
	CC=tcc ./configure --prefix=/usr --without-readline LIBS="$(pwd)/dso_stub.o"
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
	# The two lines that used to sit here copied the BUILD HOST's
	# /lib/x86_64-linux-gnu/libreadline.so.8 into this package. That is
	# foreign-OS library content shipped inside a Cix package, which the
	# Build Provenance Mandate forbids outright, and it caused a real
	# failure rather than staying theoretical: Debian's libreadline
	# resolves versioned symbols against Debian's libtinfo, so once
	# ADR-0209 reduced the baseline to the glibc floor and libtinfo came
	# from this project's own ncurses instead, every build environment
	# holding gawk died in the dynamic linker --
	#   gawk: /usr/lib/libtinfo.so.6: no version information available
	#         (required by /lib/x86_64-linux-gnu/libreadline.so.8)
	#   Inconsistency detected by ld.so: check_match: Assertion failed
	# -- which took out mtools and xorriso, neither of which has
	# anything to do with readline. Building --without-readline removes
	# the need entirely.
}
