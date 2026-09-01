#
# keepalived -- VRRP (a shared "floating" IP a router pair fails over
# between them) plus LVS/IPVS load-balancing health checking. Same
# recipe contract as bash.recipe -- see that file's own header comment
# for the metadata-scanner-vs-sourced-shell-script split.
#
# Source is Debian's own ".orig.tar.gz" for their keepalived package
# -- the exact, unmodified upstream release tarball (Debian repackages
# nothing into "orig" tarballs, by their own packaging policy), used
# here rather than github.com/acassen/keepalived's own release page
# directly for consistency with the rest of this recipe set (several
# of whose real upstreams 403 non-browser User-Agents; Debian's own
# mirror sidesteps needing to check that project by project). Checksum
# verified two ways: against Debian's own published .dsc
# Checksums-Sha256 field for this exact file, and independently by
# downloading it and computing sha256 directly -- both matched.
#
#
# WHAT CHANGED IN -2:
#
# 2.3.4 copied EIGHT of the build host's Debian libraries straight into
# the package with `cp -a` -- libssl, libcrypto, libnl-3, libnl-genl-3,
# libip4tc, libip6tc, libnftnl and libmnl. That ships foreign binaries
# inside a Cix package, which the Build Provenance Mandate forbids, and
# it was invisible: the checksum verified and the binary ran.
#
# openssl, libnl and libnftnl are now real Cix packages, so those are
# declared rather than copied. This revision deliberately disables no
# feature -- the build log records the pkg-config visibility table and
# every DT_NEEDED the binary produces, so what keepalived actually gets
# is measured rather than assumed.
#
# -2 measured a real capability loss against 2.3.4: iptables, nftables
# and ipset support all went to No, because those were only ever on by
# accident of Debian's libraries sitting on the old shared build host.
# libnftnl closes the nftables half of that. iptables and ipset support
# stay off until libip4tc/libip6tc and libipset are packaged too -- the
# ipset package currently ships only the binary, not its library.
#
# libmnl is deliberately NOT declared: -2's DT_NEEDED measurement showed
# keepalived never links it directly. It arrives under libnftnl, which
# does.
#
pkg_name="keepalived"
pkg_version="2.3.4-3"
# Source moves from Debian's mirror of the orig tarball to keepalived's
# own first-party release. The checksum is unchanged because the bytes
# are unchanged -- verified by downloading both and comparing sha256,
# not assumed. A checksum is never carried across a source change
# without that check.
pkg_source="https://www.keepalived.org/software/keepalived-2.3.4.tar.gz"
pkg_sha256="6afd95ddb7d3e0d3b8b8e5b3a489144131b61a01b06d29e883d0c44acc8a36bf"
pkg_depends="openssl libnl libnftnl"
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf openssl libnl libnftnl"
pkg_changelog="2.3.4-3: restores nftables support, which -2 measurably lost -- libnftnl@1.2.9-2 is now a real Cix package. Drops libmnl from pkg_depends: -2's own DT_NEEDED measurement showed it was never linked directly. 2.3.4-2: builds against Cix's own openssl/libnl/libmnl and declares them, instead of copying eight of the build host's Debian libraries into the package (#206, #207)"

pkg_build() {
	export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"

	echo "=== libraries visible to pkg-config ==="
	for m in libnl-3.0 libnl-genl-3.0 libmnl libnftnl openssl libipset; do
		if pkg-config --exists "$m" 2>/dev/null; then
			echo "  present  $m $(pkg-config --modversion "$m")"
		else
			echo "  ABSENT   $m"
		fi
	done

	CC=tcc ./configure --prefix=/usr

	echo "=== configure result ==="
	grep -E "Use (iptables|nftables|ipset)|Use VRRP|OpenSSL" config.log 2>/dev/null | head -10 || true

	make -j"$(nproc)"
}

pkg_install() {
	dir="$PKG_DESTDIR/usr/sbin"
	mkdir -p "$dir"
	cp keepalived/keepalived "$dir/"

	# Record every shared library this binary actually needs. This is the
	# measurement the revision exists to take -- anything listed here that
	# is not a Cix package is a remaining gap, not something to paper over
	# by copying the host's copy back in.
	echo "=== keepalived DT_NEEDED ==="
	readelf -d "$dir/keepalived" | grep NEEDED

	readelf -d "$dir/keepalived" | grep -q 'NEEDED.*libnl-3\.so\.200' || {
		echo "keepalived is not linked against libnl-3.so.200" >&2
		exit 1
	}
}
