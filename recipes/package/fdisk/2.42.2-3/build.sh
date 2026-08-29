#
# fdisk -- the interactive partition editor, from util-linux.
#
# image/src/mkinstalleriso.c stages it into the installer image for
# cix-install's own interactive partitioning path (the default; the
# --auto-partition flag uses sfdisk instead). It was read straight off
# /usr/sbin/fdisk on whatever machine ran the ISO build -- true on a
# Debian development box, false on every Cix host, and one of the last
# reasons POST /v1/system/iso could not run on a real installed machine.
#
# Same util-linux tarball libuuid, libblkid and getopt already build
# from, at the same version and checksum -- one source, four packages,
# each taking only the part it needs.
#
# NOT "--disable-all-programs --enable-fdisk", which -1 tried and which
# cannot work, for exactly the reason getopt 2.42.2-2 documents:
# UL_BUILD_INIT emits an AS_HELP_STRING for a PROGRAM but registers no
# AC_ARG_ENABLE to back it, so the switch is advertised and then
# ignored. -1 checked for the string "--enable-fdisk" in the generated
# configure and found it -- in the help text, not in a registration --
# which is a weaker test than it looks and produced the identical
# outcome getopt's own -1 did: configure warned "unrecognized options:
# --enable-fdisk", the build succeeded having compiled nothing, and
# install failed with "cp: cannot stat 'fdisk'". The supported route is
# the one configure prints itself -- "Type 'make' or 'make <utilname>'
# to compile".
#
# --disable-shared is deliberate and load-bearing: fdisk links libfdisk,
# libsmartcols, libblkid and libuuid, and a default shared build would
# make the installer's fdisk drag four .so files that mkinstalleriso
# would then also have to stage and keep in step. Building util-linux's
# own libraries static folds them into the one binary, leaving a
# closure of just libc -- checked below rather than assumed, since a
# silently-still-shared link is precisely the failure ADR-0154 and the
# openssh DT_NEEDED episode were both about.
#
pkg_name="fdisk"
pkg_version="2.42.2-3"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
pkg_depends=""

# ADR-0199/0209: composed from exactly these, no fallback (#168). The
# set getopt 2.42.2-2 proved against this identical tarball, plus
# diffutils -- configure probes with cmp ("checking for a working dd"),
# and without it that test silently fails rather than erroring.
pkg_build_depends="bash coreutils make tcc libc-dev sed grep gawk binutils pkgconf findutils diffutils"

pkg_build() {
	# CPPFLAGS, not CFLAGS: this only needs to reach the preprocessor,
	# and CFLAGS would clobber configure's own -g -O2 default.
	#
	# -D__STDC_NO_VLA__=1 is this project's already-documented fix for
	# TCC and glibc's <regex.h> (see CLAUDE.md, and daemon/src/logstore.c
	# for the confirmed repro). glibc declares regexec()'s array
	# parameter with a C99 VLA-in-prototype size expression referencing
	# the next parameter, which TCC's parser rejects outright --
	#   /usr/include/regex.h:682: error: '__nmatch' undeclared
	# reached here through libsmartcols/src/filter-param.c, which fdisk
	# pulls in via libsmartcols. The header already carries an
	# #ifndef __STDC_NO_VLA__ branch written for exactly this case, so
	# the define steers it onto glibc's own supported fallback rather
	# than patching third-party source. It is also simply true: TCC
	# does not support the construct.
	CC=tcc CPPFLAGS="-D__STDC_NO_VLA__=1" ./configure --prefix=/usr \
		--disable-shared --enable-static \
		--without-python --without-systemd --without-udev
	make fdisk
}

pkg_install() {
	# util-linux is non-recursive automake: the binary lands at the top
	# of the build tree. This project stages it at /usr/sbin/fdisk,
	# which is where mkinstalleriso reads it.
	mkdir -p "$PKG_DESTDIR/usr/sbin"
	cp -a fdisk "$PKG_DESTDIR/usr/sbin/fdisk"
	test -x "$PKG_DESTDIR/usr/sbin/fdisk" || {
		echo "fdisk: expected output missing -- configure did not build it" >&2
		exit 1
	}

	# The static-link claim above, verified against the real ELF rather
	# than trusted from the configure flag. A DT_NEEDED on any
	# util-linux library means --disable-shared did not take, and the
	# installer would ship a binary that cannot start.
	needed=$(readelf -d "$PKG_DESTDIR/usr/sbin/fdisk" | grep NEEDED)
	echo "fdisk DT_NEEDED:"
	echo "$needed"
	case "$needed" in
	*libfdisk*|*libsmartcols*|*libblkid*|*libuuid*)
		echo "fdisk: still dynamically linked against util-linux libs" >&2
		exit 1
		;;
	esac
}
