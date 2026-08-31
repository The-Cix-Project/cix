#
# iptables -- the legacy xtables packet filter/NAT CLI (iptables,
# ip6tables, iptables-save/restore, and the full set of match/target
# extension modules -- MASQUERADE/DNAT/SNAT/conntrack/etc., the actual
# reason a router needs this). Same recipe contract as bash.recipe --
# see that file's own header comment for the metadata-scanner-vs-
# sourced-shell-script split.
#
# Source is Debian's own ".orig.tar.xz" for their iptables package --
# the exact, unmodified upstream release tarball (Debian repackages
# nothing into "orig" tarballs, by their own packaging policy), used
# here rather than netfilter.org's own download page directly because
# that 403s any non-browser User-Agent, the same workaround bird.recipe's
# own header comment already documents for a different upstream.
# Checksum verified two ways: against Debian's own published .dsc
# Checksums-Sha256 field for this exact file, and independently by
# downloading it and computing sha256 directly -- both matched.
#
#
# WHAT CHANGED IN -2:
#
# 1. nftables support is ON. 1.8.13 passed --disable-nftables for a
#    real, correctly-diagnosed reason: configure.ac requires
#    `libnftnl >= 1.2.6` and the ambient Debian libnftnl on the old
#    shared build host was 1.2.4. That constraint is gone -- libnftnl
#    is a Cix package now, at 1.2.9 -- so the reason to disable it has
#    genuinely expired rather than merely become inconvenient.
#
# 2. Headers and pkg-config files are SHIPPED, where 1.8.13 deleted
#    them. iptables provides three real shared libraries (libxtables,
#    libip4tc, libip6tc) and other packages need to build against them:
#    keepalived's iptables support is off today for exactly this
#    reason. The guide's rule is that a .pc must be a TRUE claim about
#    what the package ships -- so headers and .pc go together, or
#    neither goes. Here the libraries are real, so both ship.
#
# 3. libmnl and libnftnl are declared rather than assumed ambient.
#
pkg_name="iptables"
pkg_version="1.8.13-4"
pkg_source="https://deb.debian.org/debian/pool/main/i/iptables/iptables_1.8.13.orig.tar.xz"
pkg_sha256="1afcd33da9e8f913ace6a2126788162e207e26f5d5e29c6573c0e581ffc58b99"
pkg_depends="libmnl libnftnl"
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf libmnl libnftnl"
pkg_changelog="1.8.13-4: corrects the -3 assertion, which looked for libxtables.pc -- the pkg-config module is named xtables, not after the library file. 1.8.13-3: overrides AM_DEPFLAGS, which passes -Wp,-MMD,... that TCC cannot parse. 1.8.13-2: enables the nftables backend, now that libnftnl>=1.2.6 exists as a Cix package (1.2.9); ships libxtables/libip4tc/libip6tc headers and pkg-config files so other packages can build against them; declares libmnl/libnftnl (#206, #207)"

pkg_build() {
	export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"

	echo "=== netlink libraries visible to pkg-config ==="
	for m in libmnl libnftnl; do
		if pkg-config --exists "$m" 2>/dev/null; then
			echo "  present  $m $(pkg-config --modversion "$m")"
		else
			echo "  ABSENT   $m"
		fi
	done

	# configure treats libnftnl as a PROBE (nftables=1/nftables=0), not a
	# hard requirement -- so a missing or too-old libnftnl silently
	# produces a legacy-only build that installs perfectly cleanly. That
	# is the #113 shape exactly, so require it here rather than
	# discovering the backend went missing later.
	pkg-config --atleast-version=1.2.6 libnftnl || {
		echo "libnftnl >= 1.2.6 required for the nftables backend" >&2
		exit 1
	}

	CC=tcc ./configure --prefix=/usr --enable-shared

	#
	# extensions/GNUmakefile.in sets, unconditionally:
	#
	#     AM_DEPFLAGS = -Wp,-MMD,$(@D)/.$(@F).d,-MT,$@
	#
	# GCC splits a -Wp,a,b,c list on commas and forwards each piece to
	# the preprocessor. TCC strips the -Wp, prefix but does NOT split,
	# so it sees one nonsense option and stops:
	#
	#     tcc: error: invalid option -- '-MMD,./.libxt_AUDIT.oo.d,-MT,...'
	#
	# Overridden on the make command line rather than patched out of the
	# tree: a command-line variable assignment beats the makefile's own,
	# propagates to sub-makes through MAKEFLAGS, and leaves upstream's
	# source untouched. AM_DEPFLAGS only generates .d files for
	# incremental rebuilds, which a one-shot package build from a clean
	# tree has no use for -- nothing in the built output depends on it.
	#
	# This is the same class as the -pthread gap chrony hit (CLAUDE.md):
	# a GCC driver-flag convention TCC does not implement.
	#
	make AM_DEPFLAGS= -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	# Man pages and the xslt doc helper are real `make install` output but
	# not runtime-needed -- this is a container image, not a dev box. The
	# headers and .pc files that 1.8.13 also deleted are kept now; see the
	# header comment.
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/xtables"
	rm -f "$PKG_DESTDIR"/usr/lib/*.la

	echo "=== installed binaries ==="
	ls "$PKG_DESTDIR/usr/sbin" 2>/dev/null || true

	# The extension modules are the actual reason a router needs this
	# package (MASQUERADE, DNAT/SNAT, conntrack, ...), and they are what
	# the AM_DEPFLAGS failure stopped from building at all. Loaded on
	# demand by rule type, so nothing else would notice them missing.
	n=$(ls "$PKG_DESTDIR/usr/lib/xtables"/*.so 2>/dev/null | wc -l)
	echo "xtables extension modules: $n"
	[ "$n" -gt 20 ] || {
		echo "too few xtables extension modules ($n)" >&2
		exit 1
	}

	# The nftables backend must actually be present. Its absence is the
	# silent outcome this revision exists to prevent.
	test -e "$PKG_DESTDIR/usr/sbin/xtables-nft-multi"

	# The three shared libraries...
	for lib in libxtables libip4tc libip6tc; do
		ls "$PKG_DESTDIR"/usr/lib/$lib.so.* >/dev/null 2>&1 || {
			echo "$lib shared library missing" >&2
			exit 1
		}
	done

	# ...and the dev files that make them usable by another package's
	# build. Note the pkg-config MODULE names are not the library file
	# names: libxtables.so is described by `xtables.pc`, not
	# `libxtables.pc`. -3 asserted the latter and failed on its own
	# wrong expectation after an otherwise complete, correct build.
	for pc in xtables libip4tc libip6tc libiptc; do
		test -e "$PKG_DESTDIR/usr/lib/pkgconfig/$pc.pc" || {
			echo "missing pkgconfig/$pc.pc" >&2
			exit 1
		}
	done
	for hdr in xtables.h xtables-version.h libiptc/libiptc.h libiptc/libip6tc.h; do
		test -e "$PKG_DESTDIR/usr/include/$hdr" || {
			echo "missing header $hdr" >&2
			exit 1
		}
	done

	echo "=== xtables-nft-multi DT_NEEDED ==="
	readelf -d "$PKG_DESTDIR/usr/sbin/xtables-nft-multi" | grep NEEDED
}
