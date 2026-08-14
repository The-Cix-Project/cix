#
# openssh -- ssh/sshd/scp/sftp: the jump box's own core capability
# (task #729/#730/#731). ssh (client), sshd (server), and their
# supporting binaries -- a genuine from-source OpenSSH portable
# build.
#
# Source is OpenBSD's own canonical cdn.openbsd.org release, checksum
# verified against a second independent OpenBSD mirror
# (ftp.openbsd.org) -- byte-identical, same sha256.
#
pkg_name="openssh"
pkg_version="10.4p1"
pkg_source="https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-10.4p1.tar.gz"
pkg_sha256="ef6026dd2aea8d56059638d5d3262902c892ceba9f88395835e0d06d3fb63238"
pkg_depends="openssl zlib"

# Confirmed real via a local build in this sandbox: with no
# --with-pam/--with-kerberos5/--with-ldns/--with-libedit/--with-audit
# given, the resulting configure summary shows every one of them
# correctly "no" with zero extra libraries needed beyond openssl/zlib
# (`ldd ssh`/`ldd sshd`: libcrypto.so.3/libz.so.1/libc.so.6 only --
# unlike htop/mtr, none of PAM/Kerberos/audit/ldns silently
# auto-enable from ambient system libraries here, since each needs an
# explicit --with-X to even attempt detection). PKCS#11/U2F-FIDO
# support show "yes" in the summary but are real dlopen()-based lazy
# features (ssh-pkcs11.c/ssh-sk-client.c load a provider .so only if
# an operator configures one at runtime) -- confirmed via the same
# ldd check showing no libp11/libfido2 linked, so nothing extra is
# needed to build with these left at their real default.
#
# --with-privsep-user=sshd: sshd's own mandatory privilege-separation
# model (not optional since OpenSSH 7.5) needs an unprivileged system
# user to setuid() its network-facing pre-auth process into --
# getpwnam("sshd") must resolve at sshd startup, so whatever
# image/container this recipe's output runs in needs a real
# /etc/passwd entry for a low-privilege "sshd" user (this recipe only
# builds the software; provisioning that user, like every other
# system-state concern, belongs to the image/container setup that
# uses it -- task #730). --with-pid-dir=/run matches this project's
# own real /run convention (confirmed present on every image, unlike
# /tmp which minimal images lack).
pkg_build() {
	CC=tcc ./configure --prefix=/usr --sysconfdir=/etc/ssh \
	            --with-privsep-path=/var/empty --with-privsep-user=sshd \
	            --with-pid-dir=/run
	make -j"$(nproc)"
}

# Real files from this recipe's own DESTDIR install, confirmed via a
# local build: ssh/scp/sftp/ssh-add/ssh-agent/ssh-keygen/ssh-keyscan
# (usr/bin), sshd (usr/sbin), the privsep-model helper binaries sshd
# actually execs at runtime (usr/libexec/sshd-session,
# usr/libexec/sshd-auth, usr/libexec/sftp-server, usr/libexec/
# ssh-keysign, usr/libexec/ssh-pkcs11-helper, usr/libexec/
# ssh-sk-helper -- ALL of these are load-bearing, not optional: sshd
# re-execs sshd-session/sshd-auth per connection as part of its own
# privsep model, dropping any one of them silently breaks every
# incoming connection), etc/ssh/{ssh_config,sshd_config,moduli} (the
# real upstream defaults -- UsePAM already correctly defaults to "no"
# in the generated sshd_config since this build has no PAM support,
# confirmed by reading the generated file directly, no hand-authored
# override needed). Man pages dropped, matching every other recipe in
# this set.
#
# Deliberately NOT generated here: host keys (ssh-keygen -A). Baking
# a fixed host keypair into a shared image would mean every container
# built from it shares the exact same SSH host identity -- a real
# security anti-pattern (trivial impersonation/MITM between
# instances), the same reasoning this project's own PKI subsystem
# issues certs per-container-instance rather than baking one into a
# shared image. Host key generation is a first-boot/container-init
# concern for whatever consumes this recipe's output (task #730), not
# a build-time step here.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share/man"
}
