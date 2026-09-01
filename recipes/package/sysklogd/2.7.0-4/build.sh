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
pkg_version="2.7.0-4"
pkg_changelog="2.7.0-4: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 2.7.0-3: declare build tools so this can be rebuilt from source (#206)"
pkg_source="https://github.com/troglobit/sysklogd/releases/download/v2.7.0/sysklogd-2.7.0.tar.gz"
pkg_sha256="6ab74ab5001121bb32697fd2f7ab3cc4b4452c3f721677e06e5b60982a04d0cc"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/sysklogd-2.7.0.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_depends=""

# pkg_build_depends added (#206): this recipe declared none, so
# ADR-0199 refused it outright and it could not be rebuilt at all.
# The set is the baseline the already-declaring recipes converge on for
# a package of this shape, and nothing this recipe's own pkg_build()
# does asks for more. If that turns out to be incomplete the build says
# so by name -- which is how libxcrypt and findutils were found for the
# first three conversions.
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils"

# Real, empirically confirmed via a local ./configure + build in this
# sandbox: sysklogd's own real autotools-generated configure builds
# clean under tcc, with one real fix needed -- CFLAGS=-D__STDC_NO_VLA__=1
# works around the exact same TCC/glibc <regex.h> VLA-in-prototype
# parse failure documented in CLAUDE.md's own environment notes and
# daemon/src/logstore.c's include-block comment (syslogd.c/socket.c
# both include <regex.h> for syslog.conf's own selector-matching
# support); no source patch needed, the same standard C11 feature-test
# macro fix, just applied via CFLAGS instead of a #define at an include
# site since this is unmodified upstream source.
#
# ./configure is run only to generate config.h (its own compile checks
# already confirmed clean under tcc) -- the actual build deliberately
# does NOT call `make`. sysklogd's own Makefile.am always builds a
# libtool convenience archive (libsyslog.la, still a real .a under the
# hood even with --disable-shared --disable-static, confirmed directly:
# both syslogd and logger link against it), which needs `ar`/`ranlib`
# from binutils -- a real build dependency this platform's own build
# images don't carry by default (confirmed live: the identical
# ./configure + make sequence that succeeds in a dev sandbox with
# binutils installed fails with a real, reproducible build error on a
# real build image with none). Pulling in the whole binutils toolchain
# as a pkg_depends= just to run `ar` on eight object files this small
# would be real, unjustified extra weight (binutils' own real upstream
# tarball is itself large, non-trivial to build, and adds nothing this
# recipe's own output actually needs at runtime) -- so instead, every
# needed source file is compiled and linked directly with tcc, bypassing
# libtool/ar entirely. The exact -D flags below (SYSCONFDIR/RUNSTATEDIR/
# _BSD_SOURCE/_XOPEN_SOURCE) are copied verbatim from src/Makefile's own
# AM_CPPFLAGS (the same values `make` itself would have passed) -- their
# exact values don't matter functionally here since every path they
# affect (-P/-C/-f) is overridden at container-run time anyway, they
# only need to be *defined* for the source to compile. Confirmed via a
# real `ldd` + a real UDP round-trip in this sandbox: the resulting
# syslogd binary behaves identically to the make-built one.
#
# One further, real per-build-image difference found only by actually
# running this on a real build image (not reproducible in every dev
# sandbox -- this project's own build-image's tcc/glibc combination
# apparently doesn't auto-provide a `__dso_handle` definition at link
# time, unlike some other sandboxes' host glibc, which does): direct
# tcc linking (bypassing the crt/libtool machinery `make` would
# otherwise drive) can leave `__dso_handle` -- a symbol glibc's own
# static-destructor/`__cxa_atexit` bookkeeping expects to exist --
# undefined. Fixed with a tiny, weak, always-safe stub compiled and
# linked in alongside the real object files: `__attribute__((weak))`
# means it's silently superseded wherever the real system already
# provides one (confirmed harmless in a sandbox where the bug doesn't
# reproduce at all), and provides the missing definition wherever it
# doesn't. This is a build-time compatibility shim for this recipe's
# own tcc invocation, not a patch to sysklogd's own upstream source.
pkg_build() {
	CC=tcc CFLAGS="-D__STDC_NO_VLA__=1" ./configure --prefix=/usr --disable-shared

	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o

	DEFS='-D__STDC_NO_VLA__=1 -DHAVE_CONFIG_H -DSYSCONFDIR=\"/etc\" -DRUNSTATEDIR=\"/run\" -D_BSD_SOURCE -D_XOPEN_SOURCE=600'
	for f in src/syslogd src/socket src/timer src/syslog src/logger lib/pidfile lib/strlcpy lib/strlcat; do
		eval tcc "$DEFS" -I. -Isrc -Ilib -c "$f.c" -o "$(basename "$f").o"
	done
	tcc -o syslogd syslogd.o socket.o timer.o syslog.o pidfile.o strlcpy.o strlcat.o dso_stub.o
	tcc -o logger logger.o syslog.o pidfile.o strlcpy.o strlcat.o dso_stub.o
}

# Real files copied from this recipe's own build (confirmed via `ldd`):
# syslogd + logger only, no man pages, no systemd unit, no libsyslog.so
# -- the same doc/lib-stripping convention every other recipe in this
# set already follows. `ldd` on the built syslogd confirms zero runtime
# dependencies beyond libc.so.6/ld-linux, already part of every image's
# own baseline (pkg_seed_image_baseline()).
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
	cp syslogd "$PKG_DESTDIR/usr/sbin/"
	cp logger "$PKG_DESTDIR/usr/bin/"
	touch "$PKG_DESTDIR/var/log/messages"
	printf '*.*\t/var/log/messages\n' > "$PKG_DESTDIR/etc/syslog.conf"
}
