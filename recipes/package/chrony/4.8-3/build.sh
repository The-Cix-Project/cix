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
pkg_version="4.8-3"
pkg_depends=""
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes.
# See docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils"
pkg_source="https://chrony-project.org/releases/chrony-4.8.tar.gz"
pkg_sha256="33ea8eb2a4daeaa506e8fcafd5d6d89027ed6f2f0609645c6f149b560d301706"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/chrony-4.8.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="22f2048371a3d65e650f10a64c767463197fa8136ab02c67f145672db4f22368"
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
#
# Real root cause of the "pthread_create() not found" failure this
# recipe used to hit, found by reading chrony 4.8's own real upstream
# configure source directly (not guessed): its pthread check runs
# `$MYCC $MYCFLAGS -pthread -o conftest conftest.c` -- an unconditional,
# no-opt-out hard requirement (no --disable-async-resolv-style escape
# hatch exists; on success it also permanently adds -pthread to every
# further compile via MYCFLAGS). A direct probe confirmed
# `tcc -pthread foo.c -o foo` fails with `tcc: error: undefined symbol
# 'main'` -- tcc does not recognize `-pthread` (a GCC-specific driver
# flag, not a real compiler option) and its arg parser mishandles it in
# a way that drops the actual source file from the compile entirely,
# not merely a "flag ignored" no-op. A *bare* `tcc foo.c -o foo`
# calling pthread_create() (no -pthread at all) links clean -- glibc
# >= 2.34 folds pthread_create() into libc.so.6 itself, so the flag
# was never actually needed for correctness here, only for chrony's own
# GCC-oriented feature-detection convention. Fixed with a thin `tcc`
# wrapper (same "toolwrap" pattern procps.recipe already established
# for aclocal/automake/libtoolize) that strips a bare `-pthread` before
# delegating to the real compiler -- every other flag passes through
# unchanged.
pkg_build() {
	mkdir -p /build/toolwrap
	cat > /build/toolwrap/tcc-nopthread <<'WRAP'
#!/usr/bin/bash
args=()
for a in "$@"; do
	[ "$a" = "-pthread" ] || args+=("$a")
done
exec tcc "${args[@]}"
WRAP
	chmod +x /build/toolwrap/tcc-nopthread

	CC=/build/toolwrap/tcc-nopthread ./configure --prefix=/usr --disable-readline --without-nss \
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
