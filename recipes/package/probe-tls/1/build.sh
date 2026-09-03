#
# probe-tls -- does a BUILD container have TLS, and does it have the
# internet at all?
#
# Two separate questions that look like one, and getting them the wrong
# way round costs a whole build cycle. Rebuilding glauth properly needs
# `go mod vendor`, which fetches modules over HTTPS from inside a build
# container. Before that can be designed, two facts have to be measured
# rather than assumed:
#
#   1. Can a build container reach the internet at all? Build containers
#      are created with CLONE_NEWNET and no bridge is attached, which
#      says no -- but that is a reading of the source, not a
#      measurement, and this project's rule is that a claim about this
#      environment needs the command that produced it.
#
#   2. If it can, does it now trust anything? ca-certificates was just
#      packaged; nothing has yet confirmed a real handshake succeeds
#      using it from inside a container.
#
# The probe reports both and always exits 0. It is a measurement, not a
# gate: a NO here is a fact to design around, not a build failure.
#
# Source is the CA bundle itself -- a real, already-checksummed input
# this probe genuinely uses (it hands curl an explicit --cacert as one
# of the three cases), rather than an unrelated tarball fetched only to
# satisfy the field.
#
pkg_name="probe-tls"
pkg_version="1"
pkg_source="https://curl.se/ca/cacert.pem"
pkg_sha256="f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9"
pkg_depends=""
pkg_build_depends="bash coreutils curl ca-certificates"
pkg_changelog="1: first probe. Measures whether a build container has egress and whether a TLS handshake succeeds inside one now that ca-certificates exists, before designing the glauth source-preparation path around either answer."

pkg_build() {
	echo "=== CA bundle visible in the build environment ==="
	for f in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem \
	         /etc/pki/tls/certs/ca-bundle.crt; do
		if [ -f "$f" ]; then
			echo "  present: $f ($(wc -c < "$f") bytes, $(grep -c 'BEGIN CERTIFICATE' "$f") certs)"
		else
			echo "  MISSING: $f"
		fi
	done

	echo "=== raw egress, no TLS, no DNS ==="
	rc=0
	curl -s --max-time 10 -o /dev/null http://192.168.15.31:8080/api/v1/artifacts || rc=$?
	echo "  LAN plain HTTP to the artifact cache: curl exit $rc"

	rc=0
	curl -s --max-time 15 -o /dev/null http://93.184.215.14/ || rc=$?
	echo "  internet plain HTTP to a literal IP: curl exit $rc"

	echo "=== DNS ==="
	rc=0
	curl -s --max-time 15 -o /dev/null http://proxy.golang.org/ || rc=$?
	echo "  hostname resolution (plain HTTP): curl exit $rc"

	echo "=== TLS ==="
	rc=0
	curl -s --max-time 20 -o /dev/null https://proxy.golang.org/ || rc=$?
	echo "  HTTPS with the system trust store: curl exit $rc"

	rc=0
	curl -s --max-time 20 --cacert cacert.pem -o /dev/null https://proxy.golang.org/ || rc=$?
	echo "  HTTPS with an explicit --cacert: curl exit $rc"

	echo "=== verdict inputs above; curl exit 6 is DNS, 7 is unreachable, 60 is trust ==="
}

pkg_install() {
	#
	# A probe installs nothing. The answer is the build log.
	#
	mkdir -p "$PKG_DESTDIR/usr/share/doc/probe-tls"
	echo "probe only -- see the build log" > "$PKG_DESTDIR/usr/share/doc/probe-tls/README"
}
