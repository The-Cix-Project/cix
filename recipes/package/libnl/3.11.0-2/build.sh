#
# libnl -- the netlink protocol library suite (libnl-3, libnl-genl-3,
# libnl-route-3 and friends).
#
# The larger half of #207. Three recipes need it and could not be
# rebuilt from source without it:
#
#   iw          libnl-3 + libnl-genl-3   (nl80211 wireless configuration)
#   hostapd     libnl-3 + libnl-genl-3
#   keepalived  libnl-3 + libnl-genl-3
#
# Like libmnl, this is a library the OLD shared build sandbox supplied
# ambiently from Debian, so those three exist today only as artifacts
# from that era and cannot be reproduced. Under a composed build
# environment (ADR-0199) the ambient supply is correctly gone.
#
# Upstream is the project's own GitHub release tarball. Note this is
# libnl3 -- the "libnl-3.11.0" naming is the 3.x series, not a
# different project from the "libnl" a consumer's configure asks for.
#
pkg_name="libnl"
pkg_version="3.11.0-2"
pkg_source="https://github.com/thom311/libnl/releases/download/libnl3_11_0/libnl-3.11.0.tar.gz"
pkg_sha256="2a56e1edefa3e68a7c00879496736fdbf62fc94ed3232c0baba127ecfa76874d"
pkg_depends=""
#
# The autotools baseline (docs/guides/writing-recipes.md), plus flex and
# bison: libnl's route library carries a real generated parser
# (lib/route/pktloc_syntax.y and pktloc_grammar.l). A release tarball
# normally ships the pre-generated output, so these may prove
# unnecessary -- but this is the one package in this batch that has a
# grammar at all, and a missing generator surfaces as a confusing
# mid-build error rather than a clean refusal.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils flex bison"
pkg_changelog="3.11.0-2: the version-script guard used grep --include, which this build image's grep does not accept and read as a filename -- the guard failed on its own error before make ran. 3.11.0: first packaging -- unblocks iw, hostapd and keepalived, none of which could be rebuilt without netlink development files (#207)"

pkg_build() {
	CC=tcc ./configure --prefix=/usr --libdir=/lib/x86_64-linux-gnu \
	    --disable-static --disable-cli

	#
	# TCC cannot honour --version-script; libmnl and ipset hit exactly
	# this, and it is documented in docs/guides/writing-recipes.md.
	# Stripped from the GENERATED makefiles so upstream's own tree stays
	# unmodified, then asserted -- a sed that silently matched nothing
	# would leave the original link failure to be rediscovered later.
	#
	find . -name Makefile -exec sed -i 's/-Wl,--version-script[=,][^ ]*//g' {} +
	#
	# The guard uses find+grep rather than `grep -r --include=`: the
	# build image's grep does not accept --include, and parsed it as a
	# FILENAME, so the guard failed on its own error message and the
	# build died before make ever ran. A guard that cannot fail for its
	# own reasons is the whole point of having one.
	#
	if find . -name Makefile -exec grep -l -- '--version-script' {} + | grep -q .; then
		echo "version-script flag survived the strip" >&2
		exit 1
	fi

	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	mkdir -p "$PKG_DESTDIR/usr/lib/pkgconfig"
	if [ -d "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig" ]; then
		mv "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"/*.pc \
		   "$PKG_DESTDIR/usr/lib/pkgconfig/"
		rmdir "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"
	fi

	rm -f "$PKG_DESTDIR/lib/x86_64-linux-gnu"/*.la

	#
	# Assert what the three consumers actually need, rather than
	# trusting that configure and make exited 0 (#113). iw, hostapd and
	# keepalived all link libnl-3 AND libnl-genl-3, so a build that
	# produced only the core library would satisfy `make` and still be
	# useless to every one of them.
	#
	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libnl-3.so.200"
	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libnl-genl-3.so.200"
	test -e "$PKG_DESTDIR/usr/include/libnl3/netlink/netlink.h"
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libnl-3.0.pc"
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libnl-genl-3.0.pc"
}
