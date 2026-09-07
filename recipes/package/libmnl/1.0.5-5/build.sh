#
# libmnl -- the minimal netlink library.
#
# Written because six recipes could not be rebuilt from source without
# it (#207): ipset, iproute2, iptables, iw, hostapd and keepalived all
# need netlink development files, and this project packaged none of
# them. Those six are the networking tools, and they are exactly the
# ones the OLD shared build sandbox could build -- because Debian's
# libmnl and libnl happened to be sitting there ambiently. Under a
# composed build environment (ADR-0199) that ambient supply is
# correctly gone, and nothing replaced it. Their current artifacts date
# from that era and cannot be reproduced.
#
# libmnl is the smaller half of the problem and unblocks the most per
# line of work: ipset and iproute2 need only this, while iw, hostapd
# and keepalived want the larger libnl.
#
# Upstream is netfilter.org's own release tarball, not a distribution
# repack.
#
pkg_name="libmnl"
pkg_version="1.0.5-5"
pkg_source="https://www.netfilter.org/projects/libmnl/files/libmnl-1.0.5.tar.bz2"
pkg_sha256="274b9b919ef3152bfb3da3a13c950dd60d6e2bcd54230ffeca298d03b40d0525"
pkg_artifact_sha256="8510081b45084c637d40ce1b6da7f812ade3ce17c747843bc354f85e656b75ad"
pkg_depends=""
#
# The autotools baseline (docs/guides/writing-recipes.md). libmnl is a
# plain configure/make library with no generator step, no scripting
# language and no other library behind it -- its only real input is the
# kernel's own netlink uapi headers, which is what linux-headers is
# doing here.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils"
pkg_changelog="1.0.5-5: install to /usr/lib rather than the multiarch directory (#184 stage 2). The multiarch triplet is a Debian convention for letting several architectures share one filesystem and a Cix image has one architecture, so this platform is collapsing onto a single library directory. /usr/lib is already on the loader search path -- it is glibc slibdir plus libdir, and libdir has been /usr/lib since glibc 2.44-7 -- so a consumer finds this library exactly as before. The pkgconfig relocation below goes away with it: it existed only because libdir was not where pkgconf looks. The artifact approval is dropped because those bytes came from -4. 1.0.5-4: rebuild against tcc 0.9.28rc-10, which emits PT_GNU_STACK (#228). The artifact approval is dropped because those bytes came from rc-9."

pkg_build() {
	# One library directory (#184). This used to say /lib/x86_64-linux-gnu
	# "to match the convention the other libraries here use" -- a
	# convention inherited from Debian's multiarch layout, which exists so
	# several architectures can share one filesystem. A Cix image has one
	# architecture, and since #179 architecture lives in package identity
	# rather than in a directory name.
	#
	# Nothing about consumers changes: glibc's compiled-in search path is
	# exactly slibdir + libdir, and libdir has been /usr/lib since 2.44-7
	# (#196), so a linker and a loader both find this library here today.
	CC=tcc ./configure --prefix=/usr --libdir=/usr/lib \
	    --disable-static

	#
	# TCC does not implement --version-script, and the first build died
	# on exactly that:
	#
	#   CCLD     libmnl.la
	#   tcc: error: unsupported linker option '--version-script=./libmnl.map'
	#
	# Dropping it costs symbol versioning and nothing else: the library
	# still exports the same symbols with the same soname, and nothing
	# in this project links against a specific symbol version. The same
	# trade is already made by nss-pam-ldapd and by zlib 1.3.2-6, which
	# is where issue #113 started -- with the important difference that
	# libmnl's make FAILS here rather than silently producing a
	# downgraded library. A loud failure is the good case; zlib's
	# configure merely probed, printed "No shared library support", and
	# shipped a static library that broke the next thing to link it.
	#
	# Edited in the generated Makefile rather than in upstream's source,
	# so the third-party tree stays unmodified.
	#
	sed -i 's/-Wl,--version-script[=,][^ ]*//g' src/Makefile
	grep -q -- '--version-script' src/Makefile && exit 1

	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	# No pkgconfig relocation any more. It existed because libdir was
	# not where pkgconf looks, so every .pc file had to be carried from
	# $libdir/pkgconfig to /usr/lib/pkgconfig by hand. With libdir at
	# /usr/lib the install writes them straight there -- the workaround
	# and the problem it worked around were the same thing (#184).

	# libtool's .la files describe a static-link world this project does
	# not use, and carry absolute build-time paths that are wrong the
	# moment the package moves.
	rm -f "$PKG_DESTDIR/usr/lib"/*.la

	#
	# Assert what was built rather than trusting that configure and make
	# exited 0. This is issue #113's lesson written into the recipe: a
	# configure that PROBES rather than requires does not fail when the
	# probe fails -- zlib quietly produced a static library instead of a
	# shared one, installed cleanly, and the next thing to link against
	# it died. The whole point of this package is that a consumer can
	# link it and pkg-config can find it, so both are checked here.
	#
	test -e "$PKG_DESTDIR/usr/lib/libmnl.so.0"
	test -e "$PKG_DESTDIR/usr/include/libmnl/libmnl.h"
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libmnl.pc"
}
