#
# v2.57.106: LDAP over TLS (#414), and the startup race underneath it
#
# v2.57.105 shipped LDAPS and could not be used. Applying ldap-2 1.4.0
# crash-looped it: the recipe gated glauth on the delivered key with a
# waitkey oneshot copied from jump, and the ldap image is glibc plus
# glauth with no shell in it at all -- execve(/usr/bin/bash) failed,
# errno 2, measured on 192.168.15.95 on 2026-09-12.
#
# The gate existed because the certificate was delivered from the
# post-pidfd block, so a container's services started while
# pki_cert_create() was still running three openssl subprocesses. jump
# could paper over that with a shell loop; a service image has no shell
# to paper with, and asking every future image to carry one so it can
# wait for the daemon is the wrong shape.
#
# The race did not need to exist. pki_cert_deliver() stopped using a pid
# at #269, when it moved to writing through the container's host-side
# tree -- from then on the parameter was vestigial, its own comment said
# "pid does not say", and nothing about delivery required a running
# container. The certificate is now STAGED before clone3(), like every
# other file the daemon writes for a container, and is simply already
# there when the first process execve()s. A container that asked for a
# TLS identity and could not be given one fails to create rather than
# starting without it.
#
# v2.57.105: LDAP over TLS (#414)
#
# ldap-1/ldap-2 shipped with "[ldaps] enabled = false" in every recipe
# version, so every bind on this platform crossed the wire in cleartext
# -- including jump's AuthorizedKeysCommand, which binds with the
# svc-nslcd service-account password on every SSH login.
#
# Two blockers, both in Cix's own code, and neither visible from the
# recipes:
#
#   - pki_cert_create() emitted "subjectAltName=DNS:%s" unconditionally.
#     A TLS client checks the name it DIALLED against the matching SAN
#     TYPE, and an address dialled as an address is never matched
#     against a DNS: SAN however identical the string looks. So a cert
#     issued to ldap-1 carried DNS:ldap-1 and no address, while the
#     client dialled ldaps://192.168.150.103 -- verification fails
#     against a certificate that looks entirely correct.
#   - ldap_effective_client_uri() hardcoded "ldap://" and the plaintext
#     port, so a client had no way to be told about TLS at all.
#
# pki_issue now issues for the container's name plus every address it is
# reachable at, read from spec.nets[] rather than hand-typed into a
# separate POST /v1/pki/certs call -- the address is declared once, in
# the container's own body, and a second copy is what drifts the first
# time a container moves. IP: vs DNS: is decided by inet_pton(), not a
# digits-and-dots heuristic, which would wrongly promote
# "10.0.0.1.local".
#
# The plaintext listener on 3893 stays up, deliberately. cixd is itself
# an LDAP client -- hostauth_login() dials ldapclient.c, a hand-rolled
# BER-over-raw-socket implementation with no TLS anywhere in it -- so
# client_tls neither affects nor protects the daemon's own bind.
# Measured on 192.168.15.95 before any of this was written:
# ldap_enabled false, ldap_servers [], so that bind is not in use there
# and turning LDAPS on could not lock anyone out of the control plane.
# #416 tracks giving ldapclient.c TLS and then removing plaintext.
#
pkg_name="cix"
pkg_version="v2.57.106"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.106.tar.gz"
pkg_sha256="af49f80c6461484a2886566c50ea154be9b76242a3a85e1e28c34d826822ffa7"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils minisign"
pkg_build_caps="CAP_SYS_ADMIN"
# ADR-0208: cix-builder's one job is building Cix. This said
# "toolchain" -- an image no recipe in this repo describes, which
# existed only on one host and died with it. cix-builder is what the
# guide documents and what the taxonomy names (#305).
pkg_build_image="cix-builder"
pkg_depends=""

pkg_build() {
	make CIX_VERSION="$pkg_version" \
	    build/cix-init build/cixd build/cixctl build/mkbootroot build/cix-install \
	    build/cix-recover build/mkinstalleriso build/cix-boot.efi build/cix-xorriso
	#
	# The contract guards: the generated REST surface and its route
	# count, the documentation indexes, the ADR-0224 gcc-exception
	# count, the ELF install gate, treecopy's device-node handling.
	# Failing here fails the build, which is the point.
	#
	make CIX_VERSION="$pkg_version" selftest
	#
	# #296: prove the new console-input scenario actually catches the
	# bug it was written for.
	#
	# The selftest above ran test_console_exec, which now includes an
	# INPUT scenario: it writes a keystroke as a binary websocket frame
	# and requires a real exec'd process to echo it back. A pass proves
	# nothing on its own -- the whole reason #294 shipped is that this
	# file was green while the console ignored every keystroke.
	#
	# v2.53.60 tried to prove it by DELETING the memset, and the test
	# still passed, so that build correctly failed. The reason is worth
	# keeping: not zeroing a malloc() only reproduces #294 when the
	# memory happens to be non-zero, and fresh kernel pages are zeroed,
	# so pty_out_len came up 0 and the code worked. That is also why
	# #294 was environment-dependent rather than constant.
	#
	# So the struct is POISONED instead of merely left unzeroed, which
	# is what uninitialised memory actually looked like on the host that
	# hit this: every field the code below assigns is still assigned,
	# and every field it forgot -- pty_out_len, the buffer -- is
	# garbage. That is exactly #294.
	#
	echo "=== #296: poisoning the console session struct, the input test must now FAIL ==="
	sed -i 's@^\tmemset(sess, 0, sizeof(\*sess));$@\tmemset(sess, 0xff, sizeof(*sess)); /* #296 proof */@' daemon/src/main.c
	grep -q "#296 proof" daemon/src/main.c || {
		echo "could not inject the #294 condition -- the proof is not being run" >&2
		exit 1
	}
	make CIX_VERSION="$pkg_version" build/cixd
	rc=0
	./build/test_console_exec >/tmp/c296.log 2>&1 || rc=$?
	tail -30 /tmp/c296.log | sed 's/^/  /'
	if [ "$rc" = "0" ]; then
		echo "=== the console-input test PASSED against a build carrying the #294 bug" >&2
		echo "=== it does not detect what it was written for; failing this build" >&2
		exit 1
	fi
	echo "=== the input test detected the injected bug (rc=$rc), so its pass above is real ==="
	sed -i 's@^\tmemset(sess, 0xff, sizeof(\*sess)); /\* #296 proof \*/$@\tmemset(sess, 0, sizeof(*sess));@' daemon/src/main.c
	grep -q "memset(sess, 0, sizeof(\*sess));" daemon/src/main.c || {
		echo "could not restore the #294 fix -- refusing to ship" >&2
		exit 1
	}
	make CIX_VERSION="$pkg_version" build/cixd
}

pkg_install() {
	cp build/cix-init build/cixd build/cixctl build/mkbootroot build/cix-install \
	   build/cix-recover build/mkinstalleriso build/cix-boot.efi build/cix-xorriso \
	   "$PKG_DESTDIR/"
	# The dashboard mkbootroot copies into the assembled root.
	cp -r web "$PKG_DESTDIR/web"
	for f in index.html app.js style.css api.js; do
		if [ ! -f "$PKG_DESTDIR/web/$f" ]; then
			echo "web/$f missing from the artifact -- the assembled control plane would serve a blank dashboard" >&2
			exit 1
		fi
	done
	# Asserted against the real bytes: this has to be a PE32+ image or
	# the firmware will not load it, and a wrong format would surface
	# only as a machine that does not boot after an install. MZ is the
	# DOS header every PE file begins with.
	magic=$(dd if="$PKG_DESTDIR/cix-boot.efi" bs=1 count=2 2>/dev/null)
	case "$magic" in
	MZ) ;;
	*)
		echo "cix-boot.efi is not a PE image (magic: $magic)" >&2
		exit 1
		;;
	esac
}
