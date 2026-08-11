#
# sysklogd -- the real syslog receiver for syslog-1/syslog-2 (logging
# epic Part 2, ADR-0127): a redundant pair of ordinary containers
# running a real, standard syslogd, so operators who want familiar
# external tooling on top of this platform's own logging have
# something real to point it at. Same division of labor as dns-1/dns-2
# and ntp-1/ntp-2 (dnsmasq.recipe/chrony.recipe's own header comments):
# the actual protocol server is a real, unmodified upstream binary
# running as a normal containerized workload -- this platform's own
# daemon/src/syslogfwd.c is a client (an RFC 3164 UDP sender), never a
# server implementation of its own (ADR-0007's "no hand-rolled
# workloads" reasoning, same as every other protocol this project
# integrates with rather than reimplements).
#
# Source is the maintained troglobit/sysklogd fork's own GitHub release
# tarball -- it ships a pre-generated ./configure (not just
# configure.ac), so no autoreconf/autotools bootstrap is needed in this
# recipe, matching the "release tarball, not a raw git snapshot"
# convention every other recipe in this set already follows.
#
pkg_name="sysklogd"
pkg_version="2.7.0"
pkg_source="https://github.com/troglobit/sysklogd/releases/download/v2.7.0/sysklogd-2.7.0.tar.gz"
pkg_sha256="6ab74ab5001121bb32697fd2f7ab3cc4b4452c3f721677e06e5b60982a04d0cc"
pkg_depends=""

# Real, empirically confirmed via a local ./configure + build in this
# sandbox: sysklogd's own real autotools-generated configure builds
# clean under tcc, with one real fix needed -- CFLAGS=-D__STDC_NO_VLA__=1
# works around the exact same TCC/glibc <regex.h> VLA-in-prototype
# parse failure documented in CLAUDE.md's own environment notes and
# daemon/src/logstore.c's include-block comment (syslogd.c/socket.c
# both include <regex.h> for syslog.conf's own selector-matching
# support); no source patch needed, the same standard C11 feature-test
# macro fix, just applied via CFLAGS instead of a #define at an include
# site since this is unmodified upstream source. --disable-shared:
# nothing in this platform's own image model ever dynamically loads a
# libsyslog.so at runtime, so only the static variant is built.
pkg_build() {
	CC=tcc CFLAGS="-D__STDC_NO_VLA__=1" ./configure --prefix=/usr --disable-shared
	make -j"$(nproc)"
}

# Real files copied from this recipe's own build (confirmed via `ldd`):
# syslogd + logger only, no man pages, no systemd unit, no libsyslog.so
# (disabled above) -- the same doc/lib-stripping convention every other
# recipe in this set already follows. `ldd` on the built syslogd
# confirms zero runtime dependencies beyond libc.so.6/ld-linux, already
# part of every image's own baseline (pkg_seed_image_baseline()).
#
# /etc/syslog.conf: one catch-all rule writing everything to
# /var/log/messages -- /var/log is created here (staged directly into
# the image, mirroring dnsmasq.recipe's own /etc/passwd staging
# convention) since this platform's own minimal images have no /var by
# default (CLAUDE.md's own "no /tmp, no /bin" environment note --
# confirmed the same gap extends to /var, not just those two). Message
# content lives in the container's own persistent overlay upperdir
# (unlike /run, /var/log is never reset on restart, ADR-0119), so a
# real operator's external syslog tooling has something durable to
# read even across a container restart.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/sbin" "$PKG_DESTDIR/usr/bin" "$PKG_DESTDIR/etc" "$PKG_DESTDIR/var/log"
	cp src/syslogd "$PKG_DESTDIR/usr/sbin/"
	cp src/logger "$PKG_DESTDIR/usr/bin/"
	touch "$PKG_DESTDIR/var/log/messages"
	printf '*.*\t/var/log/messages\n' > "$PKG_DESTDIR/etc/syslog.conf"
}
