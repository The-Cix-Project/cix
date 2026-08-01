#
# bird -- the BIRD Internet Routing Daemon (v2). Same recipe contract
# as bash.recipe -- see that file's own header comment for the
# metadata-scanner-vs-sourced-shell-script split.
#
# Source is Debian's own ".orig.tar.xz" for their bird2 package -- the
# exact, unmodified upstream release tarball (Debian repackages
# nothing into "orig" tarballs, by their own packaging policy), used
# here rather than bird.network.cz's own download page directly
# because that page 403s any non-browser User-Agent. Checksum verified
# against Debian's own published .dsc Checksums-Sha256 field for this
# exact file, not just self-computed.
#
pkg_name="bird"
pkg_version="2.19.1"
pkg_source="https://deb.debian.org/debian/pool/main/b/bird2/bird2_2.19.1.orig.tar.xz"
pkg_sha256="9a5f6793866cc8d07112639568a5303be95a88e51385cef8e67ae03e3def90eb"
pkg_depends=""

# Confirmed directly: this tarball (Debian's own "orig" repackaging of
# BIRD's real upstream source) ships configure.ac but no pre-generated
# ./configure or sysdep/autoconf.h.in -- despite being a real release
# tarball, not a git checkout (it even still carries .gitlab-ci.yml).
# `autoreconf -fi` (force regeneration; install any missing standard
# auxiliary files) runs whichever of autoconf/autoheader/aclocal are
# actually needed, in the right order -- found empirically after
# discovering the individual tools one failure at a time (autoconf
# alone gets ./configure but not sysdep/autoconf.h, which needs
# autoheader too). --prefix=/usr matches every other recipe in this
# set; BIRD's own build otherwise defaults to installing under
# /usr/local/{sbin,etc}, which would land in the wrong place once
# merged into the shared base image. --runstatedir=/run: confirmed
# directly (grep against this exact release's own configure.ac/
# Makefile.in) that BIRD's own CONTROL_SOCKET is literally
# "$(runstatedir)/bird.ctl" -- without this override, autoconf's
# default runstatedir is ${localstatedir}/run, and --prefix=/usr alone
# makes that /usr/var/run (real, confirmed live, ADR-0041/Phase 24: no
# image ships that path, so birdc's control socket had nowhere to go).
# ADR-0041 also gives every image a real, empty /run generically, but
# fixing BIRD's own default is the actual root fix -- /usr/var/run
# still wouldn't exist without it even with a populated /run elsewhere.
pkg_build() {
	autoreconf -fi
	./configure --prefix=/usr --runstatedir=/run
	make -j"$(nproc)"
}

# `bird`/`birdcl` need nothing beyond what pkg_seed_image_baseline()'s
# global bootstrap set already covers, but `birdc` (the readline-
# enabled control client -- interactive line editing/history) links
# libreadline, confirmed via ldd against the real build -- never
# exercised until this recipe's own control client was actually run,
# not caught up front. libtinfo (readline's own transitive dependency)
# is already part of that global set; only libreadline itself needed
# staging here.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a /lib/x86_64-linux-gnu/libreadline.so.8 /lib/x86_64-linux-gnu/libreadline.so.8.2 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
