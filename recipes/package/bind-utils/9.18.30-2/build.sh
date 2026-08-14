#
# bind-utils -- dig/host/nslookup, the jump box's own real DNS lookup
# tools. ISC BIND9's own official release tarball (real, pre-generated
# ./configure -- BIND is not a raw git-tag archive dependency the way
# psmisc/libuv are).
#
# BIND's own top-level ./configure hard-requires libuv (libuv.recipe,
# published alongside this one purely as this prerequisite) and
# openssl (already a real recipe/dependency of curl/openssh) via
# pkg-config -- confirmed directly in configure.ac (AC_MSG_ERROR if
# either .pc file can't be found). Every OTHER optional dependency
# (json-c, libxml2, lmdb, nghttp2, libmaxminddb, GSSAPI, readline/
# libedit) auto-detects and gracefully disables itself when absent --
# none of those have a recipe in this project, and none are needed for
# dig/host/nslookup's own core lookup functionality anyway.
#
# Deliberately scoped to `make -C lib` (BIND's own internal libisc/
# libdns/libirs/... libraries dig links against) + `make -C bin/dig`
# (the real subdirectory dig/host/nslookup all three build from) --
# never a full top-level `make`, which would also build bin/named (the
# actual DNS server this jump box has no use for) and pull in real
# additional requirements (capability dropping, seccomp, etc.) that
# dig itself never needs.
#
pkg_name="bind-utils"
pkg_version="9.18.30-2"
pkg_source="https://downloads.isc.org/isc/bind9/9.18.30/bind-9.18.30.tar.xz"
pkg_sha256="9f6817640970267317e5aa143ecf70531040f2155636b1a63ea45379aa09034a"
pkg_depends="libuv openssl"

pkg_build() {
	./configure --prefix=/usr --without-python --disable-linux-caps \
	            --without-lmdb --without-libxml2 --without-json-c \
	            --without-maxminddb --without-gssapi --without-readline \
	            --disable-geoip --disable-doh
	make -j"$(nproc)" -C lib
	make -j"$(nproc)" -C bin/dig
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp bin/dig/dig bin/dig/host bin/dig/nslookup "$PKG_DESTDIR/usr/bin/"
}
