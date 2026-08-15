#
# iputils -- ping/arping/tracepath, real Linux network diagnostics for
# the jump box package set. The already-published "iputils" 20250605
# recipe (meson-based) fails to build against this box's own real
# thinc-builder image -- confirmed directly (build failed exit
# status 1): meson/ninja are not staged anywhere in this project's
# toolchain (no recipe for either exists), so meson's own configure
# step can never run here. Rather than adding meson+ninja+python's own
# whole build-tool chain as a prerequisite just for this, this version
# targets the last iputils release still built by a plain Makefile
# (no configure step, no meson) -- s20180629, confirmed directly by
# fetching the real tarball and inspecting Makefile/meson.build side
# by side (meson.build already existed then, but the Makefile path was
# still the real, working default).
#
# USE_CAP/USE_IDN/USE_NETTLE/USE_GCRYPT/USE_CRYPTO all forced to "no"
# below -- every one of libcap/libidn2/nettle/libgcrypt has no recipe
# in this project (the same real gap mtr.recipe's own comment already
# documents for libcap), and USE_CRYPTO (ping6's IPv6 auth headers,
# an obsolete, unused feature) doesn't need openssl's libcrypto pulled
# in just for it. USE_RESOLV=yes needs only glibc's own -lresolv, no
# extra recipe. ENABLE_RDISC_SERVER stays off (a router-discovery
# server daemon, not what this box's ping/arping/tracepath diagnostics
# need).
#
pkg_name="iputils"
pkg_version="s20180629-1"
pkg_source="https://github.com/iputils/iputils/archive/refs/tags/s20180629.tar.gz"
pkg_sha256="da14105291dd491f28ea91ade854ed10aee8ba019641c80eed233de3908be7c5"
pkg_depends=""

pkg_build() {
	# -fcommon: this 2018-era code relies on tentative definitions
	# across ping.c/ping6_common.c being merged the old K&R "common
	# symbol" way -- GCC >= 10 defaults to -fno-common, which turns
	# that into a real "multiple definition of `device'" link error.
	# Confirmed directly (first build attempt failed exactly this way).
	make CC=tcc CFLAGS="-O3 -g -fno-strict-aliasing -Wstrict-prototypes -Wall -fcommon" \
	     USE_CAP=no USE_IDN=no USE_NETTLE=no USE_GCRYPT=no USE_CRYPTO=no USE_RESOLV=yes \
	     ENABLE_RDISC_SERVER=no ping tracepath arping
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp ping tracepath arping "$PKG_DESTDIR/usr/bin/"
}
