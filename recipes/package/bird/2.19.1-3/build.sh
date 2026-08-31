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
pkg_version="2.19.1-3"
#
# Source moved from Debian's pool to BIRD's own release archive,
# because the pool URL now 404s: Debian's pool holds only what is
# CURRENTLY packaged, so a superseded version simply disappears. That
# makes it unsuitable as a pkg_source -- the recipe stops being
# buildable through no change of its own.
#
# Note bird.network.cz 403s any non-browser User-Agent (already
# recorded in this recipe's history); bird.nic.cz serves the same
# archive and does not. Checksum recomputed from the downloaded bytes,
# since this is upstream's .tar.gz rather than Debian's repackaged
# .orig.tar.xz -- a different byte sequence for the same release.
#
pkg_source="https://bird.nic.cz/download/bird-2.19.1.tar.gz"
pkg_sha256="e91aedac07da4f2718f269f969f65dbcd12f4b58248a784fd32e587b7b4dd7e4"
pkg_depends=""
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils autoconf automake libtool m4"
pkg_changelog="2.19.1-3: sources from BIRD's own archive instead of Debian's pool, which 404s once a version is superseded; declares build tools (#206)"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils autoconf automake libtool m4"

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
	CC=tcc ./configure --prefix=/usr --runstatedir=/run
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
