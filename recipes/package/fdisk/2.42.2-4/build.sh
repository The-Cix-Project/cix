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
pkg_version="2.42.2-4"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
pkg_depends=""

# ADR-0199/0209: composed from exactly these, no fallback (#168). The
# set getopt 2.42.2-2 proved against this identical tarball, plus
# diffutils -- configure probes with cmp ("checking for a working dd"),
# and without it that test silently fails rather than erroring.
pkg_build_depends="bash coreutils make gcc libc-dev sed grep gawk binutils pkgconf findutils diffutils"

pkg_build() {
	# Tier 3 (gcc), for the same defect libblkid 2.42.2-3 documents at
	# length against this identical util-linux tarball: TCC does not
	# implement __builtin_clz, and rather than failing it emits the
	# builtin as an ordinary undefined external symbol. Here it surfaced
	# at link time --
	#
	#   CCLD     fdisk
	#   tcc: error: undefined symbol '__builtin_clz'
	#
	# -- which is the lucky version. libblkid got the silent one: it
	# linked, installed cleanly, and shipped a broken library.
	#
	# Not worked around with a hand-written __builtin_clz stub (the
	# shape sysklogd.recipe uses for __dso_handle). That would be a
	# second, different answer to a defect this project has already
	# answered once, in the same tarball, for the same reason -- and
	# unlike __dso_handle, which is CRT bookkeeping no code reads, this
	# one is a real arithmetic primitive whose miscompilation would be
	# silent. One answer, reused.
	#
	# -D__STDC_NO_VLA__=1 is gone with TCC: it existed only to steer
	# glibc's <regex.h> off a C99 VLA-in-prototype that TCC cannot
	# parse. gcc parses it fine, and claiming otherwise would be false.
	CC=/usr/bin/gcc ./configure --prefix=/usr \
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
