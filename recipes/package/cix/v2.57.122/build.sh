#
# v2.57.122: a post-reset cert redelivery never reached the container,
# and test_pki is a release gate (#417)
#
# pki_cert_deliver() wrote into a RUNNING container with
# persist_atomic_write(), which renames a new inode over the path --
# the one writer #276 measured as invisible through a mounted overlay.
# test_pki's resetlive check compares the delivered tls.crt through
# /proc/<pid>/root across POST /v1/pki/reset; on v2.57.121 the bytes
# did not change. So after a CA reset a running container went on
# serving a cert signed by the destroyed CA, reported as delivered.
# Now persist_write_file_inplace(), which exists for this destination.
#
# test_pki joins DAEMON_SELFTESTS_2. It was excluded on the belief that
# a build container cannot create a container; the real cause was
# cix-init (fixed in v2.57.121), and with creation working it went from
# 9 failures to 2. Its own durablehost round 2 recreated a container
# immediately after deleting it and raced ADR-0180's asynchronous
# teardown -- it now waits out that one 409. And that 409 said "already
# exists", which reads as a clash with a container `container ls` does
# not show; it now distinguishes a mid-teardown holder, the way
# registry_ip_holder() has for an address since #148.
#
#
#
# v2.57.121: cert issuance no longer depends on stack contents (#417)
#
# pki_cert_create() read `off` before writing it -- #414 moved the
# assignment inside the loop whose guard reads it -- so when the stack
# garbage was >= sizeof(sanbuf) the loop never ran and an uninitialised
# buffer went to openssl as -addext. Measured in a build container:
# "Duplicate extension: p.internal" / "ernal" / "", three tails of an
# unrelated string. It happens to work on this host, which is why it
# shipped in #414 and stood.
#
# Also: SELFTEST_HELPERS now builds cix-init, without which every
# container-creating test fails; and a reset that skips a container's
# cert redelivery says so, on both stderr and the log store.
#
#
# v2.57.116: same as v2.57.115, which failed its build gate.
#
# test_api_surfaces refused a hand-typed "/v1/containers/%s/stop" in the
# CLI -- the exact violation its own comment names. The generated
# CIX_API_stopContainer / CIX_API_startContainer constants instead.
#
#
# v2.57.115: stage the managed listeners before clone3 (#419, ADR-0282)
#
# v2.57.114 rendered the listener values into each server's live config
# and glauth never adopted them: it binds listeners at startup, its
# config watcher reloads only records, and a restart re-staged the
# recipe's own value over the rendered one. The values are applied to
# the STAGED content now, before clone3 -- the same ordering fix
# pki_cert_deliver() needed in #414. Plus an opt-in
# `ldap config set --restart-servers`, off by default.
#
#
# v2.57.114: glauth's listeners are configuration (#419, ADR-0282)
#
# PUT /ldap/config owns [ldap]/[ldaps] enabled+listen, rendered into
# each registered server's own config. client_tls_port is gone -- the
# port clients get is the server's own. listeners_managed keeps the
# render off an upgrading host's live listeners until an operator sets
# one, because a default derived from nothing would rewrite them within
# seconds of the first boot.
#
#
# v2.57.113: a register renders its own server (#418)
#
# Recreating an ldap_server container left its glauth config with no
# [[users]] stanzas, so every bind was refused with invalidCredentials
# while the container looked healthy. create_container_persisted() syncs
# BEFORE register_declared_server_roles() runs, so the sync iterated
# bindings that did not include the new container yet. The seeding write
# moves into ldap_server_register(), where ldap_record_sync_all()'s own
# comment already (falsely) claimed it was.
#
#
# v2.57.112: the plaintext LDAP listener comes down (#416)
#
# ldap-1/ldap-2 1.6.0 set [ldap] enabled = false. That exposed a bug the
# open port had been hiding: the server-health probe was fixed at
# HOSTAUTH_LDAP_DEFAULT_PORT, so both servers reported unhealthy against
# a deliberately closed door. It now reads ldap_client_port(), the same
# accessor the client URI reads. Also: ldap_tls_log() reported "no
# OpenSSL error queued" for a plaintext peer -- SSL_get_error()+errno
# now, since an empty queue is the normal outcome there.
#
#
# v2.57.111: the daemon's own LDAP bind speaks TLS (#416)
#
# cixd is an LDAP client too, and ldapclient.c had no TLS at all -- so
# #414's client_tls protected the container-side clients and never the
# daemon's own bind. ldap_tls on PUT /system/hostauth-config runs it
# over LDAPS, verified against this host's own CA read at dial time
# (pki_trust_bundle_pem(), new; the file writer is reimplemented on it).
#
#
# v2.57.110: the CA can leave the box, encrypted, and come back
#            (#415, ADR-0281)
#
# v2.57.109 shipped this feature with POST /v1/pki/export returning an
# opaque 500: pki_export() and pki_import() each assigned 17 openssl
# arguments plus a NULL into `char *argv[16]`, writing two pointers past
# the end of the stack frame. -Wall -Werror cannot see a runtime index.
# Both are argv[24] now (matching the file's five older invocations) and
# every append is bounds-checked. The export failure paths also wrote
# only to stderr, which cixd never mirrors into the log store, so the
# 500 had no recorded cause -- they log now.
#
# v2.57.109 added one correction to v2.57.108's own code: pki_tmp_create()
# unlinked its path and then created it O_EXCL, so the flag guarding the
# files that hold the export passphrase and the decrypted CA key could
# never actually fail. Now it can, and only a leftover from a previous
# daemon under a reused pid is cleared and retried.
#
# v2.57.107 is the same change and failed its selftest on a number:
# apigen found 301 operations against an expected 299. That is the gate
# doing exactly what its own message says -- "a silently different count
# is how a lost route hides" -- and the two new operations are
# POST /pki/export and POST /pki/import. Updated deliberately, which is
# the only way that assertion is meant to move.
#
# PKI state lives at /config/state/pki, on the cix-config partition --
# a deliberate placement: the CA survives a reboot, an A/B update and a
# rolling rebuild. And cix-install.c formats that partition, so a
# reinstall destroyed the root CA, the intermediate and every leaf
# under them, with nothing to carry one across.
#
# ADR-0033 had already decided this, explicitly and with the owner,
# weighing three options and keeping the "never leaves via API"
# guarantee absolute -- on the stated grounds that PKI backup was "a
# genuinely separate, host-level concern (/var/lib/cix/pki/, backed up
# directly, outside the API)". That reasoning was sound. Its premise
# stopped being true in two unrelated steps: the path became
# /config/state/pki (#250), and -- decisively -- there is nowhere to
# back it up FROM, because the host is shell-less and API-only by
# charter. "Backed up directly, outside the API" names an operation an
# operator cannot perform on this platform. The guarantee stayed
# absolute while the alternative it depended on disappeared.
#
# The release signing key was lost to a reinstall on 2026-09-06 -- a
# key in exactly this position, protected by exactly this reasoning.
#
# POST /v1/pki/export carries the whole store, AES-256-CBC under a
# passphrase the daemon never stores; /import puts it back, refusing a
# box that already has a CA. Every cipher parameter is explicit on both
# sides, because openssl enc defaults have moved across versions and a
# bundle a later release cannot decrypt is not a backup. Import
# validates what it decoded rather than trusting that it decoded, since
# CBC carries no integrity tag. And cix-install now KEEPS a cix-config
# that already holds a CA, loudly, with --wipe-config to force the
# format.
#
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
pkg_version="v2.57.122"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.122.tar.gz"
pkg_sha256="bc9fe250e39b6f8b885d88c67eedb1f052750c254744f691dc710699b8440745"
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
