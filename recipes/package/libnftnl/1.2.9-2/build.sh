#
# libnftnl -- the low-level netlink library for the nftables subsystem
# (libnftnl builds and parses the netlink messages; nftables' own
# higher-level rule syntax lives in libnftables, which this is not).
#
# The remaining half of #207's netlink gap. Two things need it:
#
#   iptables      the nft-backed iptables binaries (iptables-nft etc.)
#   keepalived    restores VRRP's firewall integration, which
#                 keepalived@2.3.4-2 measurably lost -- Use nftables
#                 went from yes to No once the build stopped picking up
#                 the build host's ambient Debian copy of this library
#
# Same story as libmnl and libnl: the old shared build sandbox supplied
# this ambiently from Debian, so anything needing it exists today only
# as an artifact from that era. Under a composed build environment
# (ADR-0199) that supply is correctly gone.
#
# Source is netfilter.org's own release archive -- the first-party
# distribution point for the project. Checksum computed directly from
# the downloaded bytes.
#
pkg_name="libnftnl"
pkg_version="1.2.9-2"
pkg_source="https://www.netfilter.org/projects/libnftnl/files/libnftnl-1.2.9.tar.xz"
pkg_sha256="e8c216255e129f26270639fee7775265665a31b11aa920253c3e5d5d62dfc4b8"
pkg_depends="libmnl"
#
# The autotools baseline, plus:
#   pkgconf    configure.ac does PKG_CHECK_MODULES([LIBMNL], ...) -- a
#              hard requirement, not a probe, so a missing pkg-config
#              fails the configure outright
#   libmnl     that same check; libnftnl is a layer directly on top of it
#   binutils   configure.ac calls AM_PROG_AR, and libtool builds an
#              internal archive on the way to the shared library
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf libmnl"
pkg_changelog="1.2.9-2: the --version-script strip used [^ ]*, which excludes only a literal space -- it ate the TAB and line-continuation backslash after libnftnl.map and left an unparseable Makefile. 1.2.9: first packaging -- unblocks iptables, and restores the nftables/firewall features keepalived lost when it stopped building against an ambient Debian libnftnl (#206, #207)"

pkg_build() {
	#
	# Cix packages do not agree on where a .pc file lands, so a consumer
	# cannot assume its dependency chose the same convention. Setting
	# this explicitly is what ipset and iw both needed; without it a
	# genuinely installed library still reports "not found".
	#
	export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"

	# Fail here with a clear message rather than inside configure's own
	# PKG_CHECK_MODULES, which reports a missing pkg-config as a missing
	# libmnl.
	pkg-config --exists libmnl || {
		echo "libmnl not visible to pkg-config" >&2
		exit 1
	}
	echo "building against libmnl $(pkg-config --modversion libmnl)"

	CC=tcc ./configure --prefix=/usr --libdir=/lib/x86_64-linux-gnu \
	    --disable-static

	#
	# TCC cannot honour --version-script, and src/Makefile.am passes one
	# unconditionally (-Wl,--version-script=$(srcdir)/libnftnl.map).
	# Stripped from the GENERATED makefiles so upstream's tree stays
	# unmodified. See docs/guides/writing-recipes.md.
	#
	# The character class is [^[:space:]], NOT [^ ]. [^ ] excludes only
	# a literal space, so against src/Makefile.am's real line --
	#
	#     libnftnl_la_LDFLAGS = -Wl,--version-script=$(srcdir)/libnftnl.map<TAB>\\
	#
	# it matched straight through the TAB and the line-continuation
	# backslash and deleted both, orphaning the continuation line. make
	# then failed with "recipe commences before first target", four
	# directories deep in a recursive build, naming nothing to do with
	# version scripts. That is what killed the -1 revision.
	#
	# The guard uses find+grep, not `grep -r --include=`: this build
	# image's grep does not accept --include and reads it as a filename,
	# so that spelling makes the guard fail on its own error message
	# (which is exactly how libnl's first revision died).
	#
	# `grep -c` over MULTIPLE files prints "file:count", but over a
	# SINGLE file prints just the count -- so an awk -F: sum silently
	# yields 0 for a one-Makefile project and the guard below would
	# compare 0 against 0 and pass without checking anything. Counting
	# over the concatenated stream avoids depending on that difference.
	before=$(find . -name Makefile -exec cat {} + | grep -c '\\$')

	find . -name Makefile -exec sed -i 's/-Wl,--version-script[=,][^[:space:]]*//g' {} +

	if find . -name Makefile -exec grep -l -- '--version-script' {} + | grep -q .; then
		echo "version-script flag survived the strip" >&2
		exit 1
	fi

	# Every line-continuation that existed before the edit must still
	# exist after it. This is the exact damage the -1 revision did, and
	# it is silent until make trips over it much later.
	after=$(find . -name Makefile -exec cat {} + | grep -c '\\$')
	if [ "$before" != "$after" ]; then
		echo "strip destroyed line continuations: $before -> $after" >&2
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
	# Assert what consumers actually need rather than trusting that
	# configure and make exited 0 -- that combination is exactly how a
	# probe-and-degrade build ships something useless (#113), and how
	# zlib once installed a static-only library cleanly.
	#
	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libnftnl.so.11"
	test -e "$PKG_DESTDIR/usr/include/libnftnl/rule.h"
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libnftnl.pc"

	# It must really be linked against Cix's libmnl, not have quietly
	# resolved the symbols some other way.
	echo "=== libnftnl DT_NEEDED ==="
	readelf -d "$PKG_DESTDIR/lib/x86_64-linux-gnu/libnftnl.so.11" | grep NEEDED
	readelf -d "$PKG_DESTDIR/lib/x86_64-linux-gnu/libnftnl.so.11" \
	    | grep -q 'NEEDED.*libmnl\.so\.0' || {
		echo "libnftnl is not linked against libmnl.so.0" >&2
		exit 1
	}
}
