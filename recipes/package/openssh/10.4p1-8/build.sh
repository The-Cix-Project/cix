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
# -2 through -6: real, confirmed-needed --with-pam revision for
# ADR-0144 task #838 (SSH login backed by real, live LDAP), plus five
# throwaway diagnostic iterations spent tracking down a real, genuine
# build-environment gap those revisions surfaced (see below) -- all
# retired (pkg recipe rm) once root-caused; -7 fixed the toolchain gap.
# Package versions are immutable once published (ADR-0107), so each
# dead end got its own new version rather than an edit in place.
#
# -8: a second, unrelated real gap found live during jumpbox1
# re-provisioning (post-ADR-0146 reinstall): with UsePAM yes, the
# actual PAM conversation (and therefore pam_ldap.so's own nslcd-
# socket conversation) runs inside sshd's own privsep pre-auth child,
# which chroot()s to --with-privsep-path before that conversation ever
# happens -- a real, standard OpenSSH security boundary, confirmed
# directly (getpwnam()-driven "invalid user" + a bind that succeeds
# fine from an unchrooted shell but fails identically from inside
# sshd) by checking `/proc/mounts` inside a real running container:
# `/run` is its own tmpfs (thincd mounts it there), while `/` (and
# therefore /var/empty, 10.4p1-7's own --with-privsep-path) is the
# overlay root -- two different filesystems, so making nslcd's own
# /run/nslcd/socket reachable from inside the chroot via a hard link
# (the standard, documented fix for this exact SSSD/nslcd-under-
# OpenSSH-privsep interaction) fails outright with EXDEV ("Invalid
# cross-device link"). Rather than reach for a bind-mount (this
# project's own minimal images have no `mount` binary at all, and
# adding one is a disproportionate new dependency for what is, at
# root, a path-placement mismatch), the real fix is putting the
# privsep chroot target on the SAME filesystem nslcd's socket already
# lives on: --with-privsep-path=/run/sshd-empty instead of the
# upstream-recommended /var/empty. Nothing else about the privsep
# security model changes -- it's still a real, dedicated, empty-at-
# start chroot target, just relocated to tmpfs so a same-device hard
# link into it actually works.
#
# UsePAM's own real upstream default stays "no" even in a --with-pam
# build (confirmed directly: servconf.h's SSHCONF_INTFLAG(use_pam,
# UsePAM, SSHCFG_GLOBAL, 0, ...) -- the "0" is the real compiled-in
# default) -- so a --with-pam build alone changes nothing for a
# container that doesn't also carry a real sshd_config setting
# `UsePAM yes` plus a real /etc/pam.d/sshd stack. Both are runtime
# config, staged per-container the same way this project already
# stages glauth.cfg/dnsmasq.conf/chrony.conf via the container's own
# `files` entry -- deliberately NOT baked into this recipe, matching
# every other recipe in this set that ships software, not the
# deployment's own configuration choices.
#
# THE REAL BUG -2 THROUGH -6 SURFACED (not an openssh-specific issue,
# a real build-environment gap worth this much detail since it can
# silently affect ANY future recipe): a local, from-scratch build in
# this dev sandbox with `CC=tcc` explicitly set correctly produced a
# real PAM-linked sshd (confirmed via readelf -d: libpam.so.0/
# libcrypt.so.1 both present). The formal recipe (-2 through -6, none
# of which set CC=tcc -- matching the convention nearly every other
# recipe in this set already follows, relying on this build sandbox's
# own default `cc`) instead produced, on the REAL target box
# specifically, an sshd/sshd-session/sshd-auth with `-lpam -lcrypt
# -ldl` genuinely present on their own link command lines (config.h
# correctly showed USE_PAM defined, configure's own summary correctly
# printed "PAM support: yes") but NEITHER library actually recorded as
# a real ELF DT_NEEDED entry in the linked output -- confirmed via a
# live, intentionally-failing diagnostic build (-6) that ran `readelf
# -d` on the real build output before install and dumped it to this
# project's own build-failure log. That same diagnostic also showed
# the link commands themselves spelled `cc`, not `tcc`, carrying real
# GCC-family hardening flags (`-fstack-protector-strong -pie
# -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack`) that TCC does not
# understand or emit -- proof this build sandbox's own default `cc`
# now resolves to a real GCC-family compiler, not TCC, on this
# specific box (almost certainly gcc.recipe's own toolchain output,
# merged into the shared, cumulative `g_pkgbuild_rootfs` build
# sandbox at some point after most of this project's other recipes
# were last built there -- see CLAUDE.md's own note on that sandbox's
# cumulative nature). The exact reason that specific gcc-driven build
# silently dropped two real, referenced libraries from its own DT_NEEDED
# despite listing them on its own link line was not further isolated
# (not needed once the actual fix was found) -- what IS confirmed,
# directly, is that forcing `CC=tcc` on this exact recipe reproduces
# this project's own local, already-verified-correct tcc build exactly:
# the same diagnostic build re-run with `CC=tcc ./configure ...`
# produced an sshd with all five real expected NEEDED entries
# (libcrypt.so.1, libc.so.6, libpam.so.0, libcrypto.so.3, libz.so.1).
# Fixed here by pinning CC=tcc explicitly, matching sysklogd.recipe's
# own precedent for a recipe that cannot silently trust this sandbox's
# own ambient default compiler. Any other recipe relying on that
# ambient default without an explicit CC=tcc should be treated as
# similarly at risk going forward, not just this one -- see CLAUDE.md's
# own new environment note.
#
pkg_name="openssh"
pkg_version="10.4p1-8"
pkg_source="https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-10.4p1.tar.gz"
pkg_sha256="ef6026dd2aea8d56059638d5d3262902c892ceba9f88395835e0d06d3fb63238"
pkg_depends="openssl zlib linux-pam"

# Confirmed real via a local --with-pam build in this sandbox, linking
# against linux-pam's own already-built libpam headers/library: the
# configure summary correctly shows "PAM support: yes" with zero new
# TCC compatibility gaps -- every one of this package's own source
# files compiles exactly as cleanly as the no-PAM 10.4p1 build did.
# PKCS#11/U2F-FIDO still show "yes" but remain the same real
# dlopen()-based lazy features 10.4p1's own comment already
# documents (no libp11/libfido2 linked at build time either way).
#
# --with-privsep-user=sshd / --with-pid-dir=/run: unchanged from
# 10.4p1, see that recipe's own comment for why each is needed.
# --with-privsep-path=/run/sshd-empty: changed from /var/empty as of
# -8, see this file's own header comment for why (puts the privsep
# chroot target on the same tmpfs nslcd's own socket lives on, so a
# same-device hard link into it actually works). CC=tcc: see this
# file's own header comment -- this build sandbox's own default `cc`
# can no longer be trusted to actually mean tcc.
pkg_build() {
	CC=tcc ./configure --prefix=/usr --sysconfdir=/etc/ssh \
	            --with-privsep-path=/run/sshd-empty --with-privsep-user=sshd \
	            --with-pid-dir=/run --with-pam
	make -j"$(nproc)"
}

# Same real DESTDIR install set 10.4p1 already documents, plus one
# new real runtime dependency --with-pam introduces: sshd/sshd-
# session/sshd-auth now carry a real ELF DT_NEEDED on libcrypt.so.1
# (glibc's crypt() implementation, split into its own shared object
# since glibc 2.28 -- confirmed directly via readelf -d against this
# recipe's own build output). Not covered by pkg_seed_image_baseline()
# (that table is for libraries every image needs; libcrypt is openssh-
# specific so far), and not provided transitively by the linux-pam
# dependency either (pam_unix.so links it internally, but sshd/sshd-
# session/sshd-auth each also link it directly) -- staged here
# directly from this build sandbox's own real libcrypt.so.1 (matching
# the exact cp -a symlink-plus-real-file pattern iproute2.recipe/
# ipset.recipe/iputils.recipe already establish for their own extra
# ambient-system runtime libs).
#
# libpam.so.0/pam_unix.so etc. need no equivalent staging here --
# they materialize onto this same target image automatically via the
# linux-pam dependency declared above (pkg's own dependency resolver
# installs a dependency's real DESTDIR content onto whatever image
# the depending package is installed onto, confirmed directly against
# resolve_chain()'s own per-image pkg_find() check).
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share/man"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a /lib/x86_64-linux-gnu/libcrypt.so.1 /lib/x86_64-linux-gnu/libcrypt.so.1.1.0 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
