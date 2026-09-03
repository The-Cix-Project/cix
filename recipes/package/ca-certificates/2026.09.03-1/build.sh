#
# ca-certificates -- the Mozilla CA root store, so a container can
# verify TLS at all.
#
# Found missing while trying to rebuild glauth (#262): preparing its
# source needs `go mod vendor`, which needs HTTPS to proxy.golang.org,
# and NOTHING in this platform's containers can verify a TLS
# certificate. There is no CA bundle in any image. The host has one --
# mkbootroot stages the build host's -- but a container gets its rootfs
# from an image, and no image carries roots.
#
# So this is not a glauth dependency. It is a capability the platform
# was missing: every container that talks to anything outside the LAN
# over TLS needs this, and until now none could.
#
# The source is the curl project's own published extraction of
# Mozilla's root store -- the same bundle nearly every distribution
# repackages. Its checksum was verified against the publisher's OWN
# published .sha256 rather than merely "it downloaded":
#
#   computed:  f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9
#   published: f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9
#
# That matters more here than for most packages. A CA bundle is the
# thing that decides what everything else will trust, so "the bytes I
# got" and "the bytes the publisher meant" being the same is the whole
# security property.
#
# Versioned by the bundle's own date rather than a software version,
# because that is what it is: a dated snapshot of a trust store, which
# will be re-cut whenever roots change.
#
pkg_name="ca-certificates"
pkg_version="2026.09.03-1"
pkg_source="https://curl.se/ca/cacert.pem"
pkg_sha256="f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9"
pkg_depends=""
pkg_build_depends="bash coreutils grep"
pkg_changelog="2026.09.03-1: first packaging. Containers on this platform could not verify a TLS certificate at all -- no image carried a CA bundle, so every HTTPS client inside a container failed regardless of what it was talking to. Found while preparing glauth's source (#262), which needs go mod vendor over HTTPS, but the gap is general rather than glauth's. Installed at the paths OpenSSL, curl and Go actually look in, since each has its own idea of where roots live and a bundle nothing finds is not installed."

pkg_build() {
	#
	# Nothing to compile -- this is data. What there IS to do is check
	# that the data is what it claims to be, because a truncated or
	# HTML-error-page download would install silently and then fail
	# every TLS handshake with an error naming the remote host rather
	# than the empty trust store.
	#
	n=$(grep -c 'BEGIN CERTIFICATE' cacert.pem || echo 0)
	if [ "$n" -lt 100 ]; then
		echo "ca-certificates: only $n certificates in the bundle -- expected a full root store" >&2
		head -5 cacert.pem >&2
		exit 1
	fi
	if ! grep -q '^## Bundle of CA Root Certificates' cacert.pem; then
		echo "ca-certificates: the file does not carry the bundle's own header" >&2
		exit 1
	fi
	echo "  verified: $n CA certificates"
}

pkg_install() {
	#
	# Three paths, because three consumers disagree about where roots
	# live and each only looks in its own places:
	#
	#   /etc/ssl/certs/ca-certificates.crt  Debian-style; what Go's
	#                                       crypto/x509 checks first
	#                                       and what most tools default to
	#   /etc/ssl/cert.pem                   OpenSSL's own default on
	#                                       several platforms
	#   /etc/pki/tls/certs/ca-bundle.crt    Red Hat-style; also on Go's
	#                                       search list
	#
	# Copies rather than symlinks: these are read by processes inside a
	# container whose rootfs may be assembled from several packages, and
	# a dangling symlink fails in exactly the silent way this package
	# exists to prevent. The file is 185 KB.
	#
	mkdir -p "$PKG_DESTDIR/etc/ssl/certs" "$PKG_DESTDIR/etc/pki/tls/certs"
	cp cacert.pem "$PKG_DESTDIR/etc/ssl/certs/ca-certificates.crt"
	cp cacert.pem "$PKG_DESTDIR/etc/ssl/cert.pem"
	cp cacert.pem "$PKG_DESTDIR/etc/pki/tls/certs/ca-bundle.crt"
}
