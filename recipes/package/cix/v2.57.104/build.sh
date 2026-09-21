#
# v2.57.104: a container can be given a certificate it does not own
#            (#397, ADR-0280)
#
# v2.57.103 deployed, and pki_cert delivered nothing. Found by really
# running it: jump started, its waitkey gate timed out after 30 seconds
# reporting the key "was never delivered by the PKI", and the daemon's
# own reason reached only stderr -- which this platform does not mirror
# into the log store, so the container could say what it had not
# received and nothing could say why.
#
# pki_cert_deliver() took one `name` parameter and used it for two
# different things: finding the certificate (cert_find(), the
# g_certs_dir paths) and locating the container's tree
# (registry_find(), container_file_host_path()). Every caller before
# ADR-0280 named the cert after the container it issued for, so the two
# were always equal and the conflation could not be observed.
# Delivering "jump-ssh" into "jump" computed the host path of a
# container named "jump-ssh", which does not exist.
#
# The signature now takes both names, and the delivery failure is
# written to the log store as well as stderr. test_pki's cert and
# container names deliberately differ and would have caught this;
# test_pki is not in SELFTESTS (#224), so it did not run.
#
# v2.57.102 built and failed the selftest on one document: ADR-0280
# carried "# ADR-0280: Title" and a Status/Date/Issue bullet list, where
# the corpus requires "# NNNN -- Title" (em-dash, number matching the
# filename) and a "## Status" heading. test_docindex exists to catch
# exactly that drift, and v2.57.100's own header records it catching the
# same thing in ADR-0274/0275. Worth stating what that build did prove:
# 85 of 86 tests passed, so the C change is -Wall -Werror clean and
# functionally sound -- only the document format failed.
#
# v2.57.101 is the same change with one gap, and was never built: the
# pki_cert lookup did not apply siteconfig_qualify(), while
# POST /v1/pki/certs applies it to the name it creates. On an install
# with a site name set, creating "jump-ssh" yields
# "jump-ssh.<site>.<suffix>" and a literal lookup 400s -- so a
# deployment recipe would have had to hardcode one install's domain to
# work. site_name is "" on 192.168.15.95 (measured 2026-09-12), which
# makes qualification a no-op there and is exactly why this would have
# shipped unnoticed until the first install that set one. Same
# inconsistency ADR-0092 fixed for dns_register.
#
# jump's SSH host key changed on every rolling rebuild. Pointing sshd at
# the platform's own PKI (jump 1.9.0) worked -- sshd loaded the key and
# served with no host-key error -- and did not fix it. Measured on
# 192.168.15.95, 2026-09-12: across one follow_rolling rebuild the key
# still went 2048 SHA256:nCR2Epsa... -> 2048 SHA256:H8urRHcL...
#
# Not a bug. pki_issue sets the new cert's owner_container to the
# container's own name, and pki_cert_forget_owner() deletes a cert whose
# owner matches the container being deleted. A rolling rebuild is a
# delete-and-recreate, because apply cannot recreate in place while the
# address is still held (409), so every rebuild shredded the certificate
# and issued a fresh one. The platform did what it was asked to.
#
# The gap was conceptual: two identities with different lifetimes were
# being served by one field. A leaf TLS certificate SHOULD die with its
# service -- a decommissioned service must not keep a live credential --
# and a TLS client verifies the CA rather than the specific leaf, so
# rotation costs it nothing. An SSH host key must not: SSH is
# trust-on-first-use, the client pins the exact bytes, and there is no
# CA in the loop to absorb a change.
#
# The PKI record already separated the two. Identity is name plus sans;
# lifetime is owner_container, and nothing reads that field but
# pki_cert_owned_by() and pki_cert_forget_owner(). pki_cert_deliver()
# never consulted the owner at all -- its only precondition is
# cert_find(name) != NULL. The mechanism was already lifetime-agnostic
# and nothing could ask for it.
#
# POST /v1/containers gains pki_cert: deliver the already-existing
# certificate of this name, create nothing, own nothing. Mutually
# exclusive with pki_issue and refused with a 400, since both write
# tls.crt/tls.key into pki_cert_dir. The named cert must exist, also a
# 400 before the container is created -- checking at delivery time
# instead would produce a container that starts successfully and quietly
# lacks the identity it asked for, because delivery failures are
# best-effort by design. Delivery repeats on every start: a fresh
# instance starts with a fresh filesystem.
#
pkg_name="cix"
pkg_version="v2.57.104"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.104.tar.gz"
pkg_sha256="eaadd70ea7e06ee668cbe9c2598bdbe7eb20abdd579fcd0c032d81b48ff538e3"
pkg_artifact_sha256="5797c3e9e8c37967e4b9d4083c200aab4b3ebabb1ca2b89ffb6383c11aa63a65"
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
