#
# curl -- the real `curl` CLI, built from source specifically for
# PKG_CURL_BIN's own actual use (daemon/src/pkg.c's host-side
# `pkg_source` fetches and `pkg bootstrap --toolchain-url=`): a plain
# HTTPS GET with resume/retry (`-fsSL --retry 8 --retry-all-errors
# --retry-delay 3 -C - -o <path> <URL>`, confirmed via a direct read
# of pkg.c's own argv[] construction). This is deliberately NOT the
# same feature set as this dev sandbox's own Debian-packaged system
# curl (HTTP/2, IDN, RTMP, SSH, LDAP, GSSAPI, PSL, Brotli, Zstd, a
# second GnuTLS-backed TLS stack alongside OpenSSL -- confirmed via a
# real `ldd` on the system binary) -- none of that is real, confirmed
# need for this project's own narrow curl usage, and building it all
# would mean seven more from-source recipes (nghttp2, libidn2,
# librtmp, libssh2, libpsl, a GSSAPI/Kerberos stack, libldap) for
# features nothing here calls. TLS (OpenSSL, this project's own
# openssl.recipe) and gzip response decoding (zlib.recipe, already
# real and present) are the two real, load-bearing dependencies; every
# other protocol/backend is explicitly disabled below.
#
# Source is curl's own canonical curl.se release, checksum verified
# against a second independent source (curl's own GitHub release
# tarball for the same tag) -- byte-identical, same sha256.
#
pkg_name="curl"
pkg_version="8.21.0-4"
pkg_source="https://curl.se/download/curl-8.21.0.tar.gz"
pkg_sha256="d9b327997999045a24cda50f3983e69e51c516bd8be6ef9842fc7f99135e33bb"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/curl-8.21.0.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_depends="openssl zlib"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf openssl zlib"
pkg_changelog="8.21.0-4: install to /usr/lib rather than the multiarch directory (#184 stage 2). The multiarch triplet is a Debian convention for letting several architectures share one filesystem and a Cix image has one architecture. /usr/lib is already on the loader search path, being glibc libdir since 2.44-7. The artifact approval is dropped because those bytes came from the previous revision. The rm of usr/lib becomes selective in the same change: it removed the whole directory after moving libcurl out of it, so leaving libcurl in place would have deleted the library this package exists to ship. 8.21.0-3: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 8.21.0-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

# Real, empirically confirmed via a local build in this sandbox
# (`./configure` summary output + a real `readelf -d` on the resulting
# libcurl.so.4): SSL enabled (OpenSSL), zlib enabled, HTTP2/IDN/RTMP/
# SSH/PSL/GSSAPI/brotli/zstd/LDAP all confirmed off -- the built
# library's own NEEDED list is exactly libssl.so.3/libcrypto.so.3/
# libz.so.1/libc.so.6, nothing else. --disable-manual/--disable-docs
# skip the built-in `curl --manual` text blob and the docs/ tree
# (matching every other recipe's own doc-stripping convention).
pkg_build() {
	CC=tcc ./configure --prefix=/usr --with-openssl --without-libpsl \
	            --disable-ldap --disable-ldaps --without-libidn2 \
	            --without-nghttp2 --without-librtmp --without-libssh2 \
	            --without-brotli --without-zstd --without-gssapi \
	            --disable-manual --disable-docs
	make -j"$(nproc)"
}

# Real files copied from this recipe's own build, not the dev host.
# Static archive/libtool .la/pkgconfig/aclocal files, curl-config, and
# wcurl (a separate real shell-script wrapper, not needed for pkg.c's
# own direct execve() use) are dropped -- nothing in this project
# links libcurl statically or uses pkg-config/autoconf against it.
# No relocation: this configure target's own default LIBDIR is plain
# usr/lib (confirmed via a real local DESTDIR install, unlike openssl's
# Configure target above, which defaults to usr/lib64), and usr/lib is
# now this platform's one library directory (#184). The move into
# lib/x86_64-linux-gnu that used to happen here was following a
# convention rather than a requirement.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	# libcurl stays where the install put it (#184). It used to be moved
	# into the multiarch directory and usr/lib deleted wholesale
	# afterwards -- so the removal has to become selective now, or it
	# would take the library with it.
	rm -f "$PKG_DESTDIR/usr/lib"/libcurl.a "$PKG_DESTDIR/usr/lib"/libcurl.la
	rm -rf "$PKG_DESTDIR/usr/lib/pkgconfig" "$PKG_DESTDIR/usr/share" \
	       "$PKG_DESTDIR/usr/bin/curl-config" "$PKG_DESTDIR/usr/bin/wcurl"
	# The three files this package exists to ship must survive that.
	test -e "$PKG_DESTDIR/usr/lib/libcurl.so.4.8.0"
	test -e "$PKG_DESTDIR/usr/lib/libcurl.so.4"
	test -e "$PKG_DESTDIR/usr/lib/libcurl.so"
}
