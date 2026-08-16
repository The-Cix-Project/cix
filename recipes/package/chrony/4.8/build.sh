#
# chrony -- real NTP server (chronyd) + control client (chronyc), for
# ntp-1/ntp-2 (mirrors dns-1/dns-2's own "the actual protocol server
# runs as a normal containerized workload" shape, ADR-0007's "no
# hand-rolled workloads" reasoning). This project's own daemon/src/
# ntp.c (task #751-753) is a host-side SNTP *client* plus a mechanism
# for *registering* an already-running container as a time source --
# it was never meant to, and does not, provide the server software
# itself, the same division of labor DNS already has (dnsmasq.recipe
# is the real server; dns.c is client+registration).
#
# Source is chrony's own canonical chrony-project.org release tarball.
# No second independently-hosted mirror exists in a byte-comparable
# form (GitHub's own tag archive is a different, re-packaged tarball
# with a different sha256, confirmed directly -- expected, not a red
# flag: GitHub auto-generates tag archives from the raw tree, without
# the project's own pre-generated configure/getdate.c files the
# official release tarball ships) -- accepted as immutable-once-
# published, the same single-source posture htop.recipe/mtr.recipe
# already established for this project.
#
pkg_name="chrony"
pkg_version="4.8"
pkg_source="https://chrony-project.org/releases/chrony-4.8.tar.gz"
pkg_sha256="33ea8eb2a4daeaa506e8fcafd5d6d89027ed6f2f0609645c6f149b560d301706"
# libc-dev (recipes/package/libc-dev/2.36/): chrony's own configure
# hard-requires a real pthread_create() (regardless of --with-user=root
# -PRIVDROP's single-threaded design -- confirmed live, this is not
# optional/feature-gated in this chrony version's configure script).
# glibc >= 2.34 folds pthread_create() into libc.so.6 itself and ships
# no standalone libpthread.so -- but some tools built with an older
# assumption still pass -lpthread explicitly (the same real gap
# gcc.recipe's own scripts/sorttable HOSTCC step already hit, see
# libc-dev.recipe's own comment). Depending on libc-dev here forces a
# known-good, already-fixed copy into this build's own sandbox rather
# than trusting whatever ambient content the shared bootstrap toolchain
# already has -- the same reasoning m4.recipe's own binutils dependency
# uses.
pkg_depends="libc-dev"

# Real, empirically confirmed via a local ./configure + build in this
# sandbox: chrony's own hand-rolled configure script (not autoconf)
# builds clean under tcc with zero patches needed. Every optional
# crypto/privilege-drop backend explicitly disabled -- none of NTS
# (needs TLS), libcap-based privilege dropping, or seccomp filtering
# are things this project's own container model needs (every container
# already runs in its own isolated netns/mountns/pidns; a second,
# in-process sandboxing layer on top would be a real "No Parallel
# Implementations" violation, the same reasoning ADR-0007 already
# applies elsewhere). --with-user=root plus -PRIVDROP (confirmed in
# the configure summary's own "Features:" line) means chronyd never
# calls setuid()/looks up any account -- this project's own minimal
# images have no real /etc/passwd, the same constraint dnsmasq.recipe
# and jumpbox's openssh.recipe already had to design around.
# --with-pidfile=/run/chronyd.pid points at the one directory this
# project's own images actually have (no /var/run, confirmed the hard
# way for dnsmasq -- see CLAUDE.md's own environment notes) rather
# than chrony's own real upstream default of /var/run/chrony/chronyd.pid.
pkg_build() {
	echo "=== diagnostic: real tcc compile+link of a trivial pthread_create() test ==="
	printf '#include <pthread.h>\nint main(void) { pthread_t t; return pthread_create(&t, 0, 0, 0); }\n' > pthread_probe.c
	tcc pthread_probe.c -o pthread_probe 2>&1
	echo "probe rc (no -lpthread) = $?"
	tcc pthread_probe.c -lpthread -o pthread_probe 2>&1
	echo "probe rc (-lpthread) = $?"

	CC=tcc ./configure --prefix=/usr --disable-readline --without-nss \
	            --without-nettle --without-gnutls --without-tomcrypt \
	            --without-libcap --without-seccomp --disable-nts \
	            --with-user=root --with-pidfile=/run/chronyd.pid
	make -j"$(nproc)"
}

# Real files copied from this recipe's own build (confirmed via a
# local DESTDIR install): chronyd/chronyc only -- man pages dropped,
# matching every other recipe's own doc-stripping convention. A real
# `ldd` on the built chronyd confirms its only real runtime
# dependencies are libc.so.6/libm.so.6/ld-linux-x86-64.so.2, all
# already staged by pkg_seed_image_baseline() -- no lib-closure copy
# step needed here, unlike curl.recipe/openssl.recipe.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share"
}
