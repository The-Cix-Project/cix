#
# mtr -- combined traceroute/ping network diagnostic tool (task #729,
# the jump box recipe set).
#
# mtr has no formal dist-tarball release (confirmed: its GitHub
# Releases API returns zero release assets, only git tags) -- the
# source below is a plain git-tag archive, which ships configure.ac
# but not a pre-generated ./configure. pkg_build() below runs the
# project's own real bootstrap.sh (aclocal + autoheader + automake +
# autoconf) before configuring, using this project's own already-built
# autoconf/automake/m4/libtool -- these don't need to be listed in
# pkg_depends since they're part of the shared sandboxed toolchain
# image every pkg_build() already runs inside (image/src/
# mktoolchainimage.c), the same reason bird.recipe/keepalived.recipe
# have an empty pkg_depends despite needing a real C toolchain to
# build. Checksum is this exact tag archive's own hash -- no second
# mirror exists for a git-tag-only source, but GitHub's tag archives
# are content-addressed and immutable once generated.
#
pkg_name="mtr"
pkg_version="0.96-4"
pkg_source="https://github.com/traviscross/mtr/archive/refs/tags/v0.96.tar.gz"
pkg_sha256="73e6aef3fb6c8b482acb5b5e2b8fa7794045c4f2420276f035ce76c5beae632d"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/mtr-0.96-3.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="bf5801e2e18ea9472f72f77ee597e9892c5096b83734d9724808a13943362bb9"
pkg_depends="ncurses"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf ncurses"
pkg_changelog="0.96-4: declares its build tools so it can be rebuilt through the ordinary install path (#206)"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf ncurses"

# --without-gtk/--without-jansson/--without-ipinfo: none of GTK+3,
# libjansson (JSON output), or ipinfo.io lookup support (needs
# libcurl-dev, no recipe for that exists) are real needs for this
# jump box's own terminal-only diagnostic use -- confirmed real via a
# local build in this sandbox, both flags eliminate the corresponding
# checks entirely rather than leaving them to silently succeed/fail
# against whatever happens to be ambient. ncursesw is picked up by
# name (confirmed via configure.ac: AC_CHECK_LIB([ncursesw],[wprintw])
# is tried before the plain ncurses/curses fallbacks, and correctly
# succeeds once ncurses.recipe's own libncursesw.so is self-contained
# -- see ncurses.recipe's own comment on why --with-termlib is
# deliberately not used there).
#
# Linux capabilities (libcap, dropping CAP_NET_RAW down to
# mtr-packet's own unprivileged helper process after opening the raw
# socket) auto-enable if libcap happens to be present at configure
# time -- confirmed empirically in this sandbox (a dev box with
# libcap-dev installed silently turned "cap: yes" on with zero flags
# passed, no --without-cap exists to force it off). No libcap recipe
# exists in this project, so a real isolated build container simply
# won't find it and this correctly falls back to "cap: no" -- mtr then
# needs to run as real root for its raw ICMP socket, the same posture
# this project's own daemon/src/ping.c already has. Deliberate scope
# decision, not an oversight: adding a whole new libcap recipe purely
# for mtr's own optional privilege-drop enhancement is out of
# proportion for this task.
# Re-pinned to -2 to close a real, confirmed build failure:
#
#   checking for fcntl... no
#   checking for error... no
#   checking for verr... no
#   configure: error: cannot find working error printing function
#
# `fcntl` obviously exists, so autoconf was not really discovering
# anything about the C library -- its AC_CHECK_FUNC probes LINK a tiny
# program, and every one of those links was failing for an unrelated
# reason, which autoconf can only report as "function not found". The
# reason is this catalog's own already-documented, environment-specific
# TCC/glibc CRT gap: `undefined symbol '__dso_handle'` on the real build
# sandbox (see sed/sysklogd/m4/coreutils/grep/bison/gawk, and procps'
# own tenth gap found the same day as this one).
#
# Fixed the same proven way: a `weak` stub object passed as a bare
# object-file path in LIBS -- never `-lxxx`, which m4.recipe's own trail
# found gets conditionally-extracted away for a weak-only archive member
# on this toolchain. Passing it via configure's own LIBS= means every
# subsequent AC_CHECK_FUNC link test carries it too, which is precisely
# the point: the probes then measure the C library rather than the CRT
# gap.
pkg_build() {
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o

	./bootstrap.sh
	CC=tcc ./configure --prefix=/usr --without-gtk --without-jansson --without-ipinfo \
		LIBS="$(pwd)/dso_stub.o"
	make -j"$(nproc)"
}

# Real files from this recipe's own DESTDIR install, confirmed via a
# local DESTDIR install: mtr/mtr-packet land under usr/sbin (this
# project's own images have no separate /sbin, so this is the real,
# final path, not usr/bin) -- mtr (the ncurses UI + traceroute/ping
# engine) and mtr-packet (the small helper process mtr spawns to
# actually open raw sockets, confirmed via ldd it needs no libcap
# here, matching the "cap: no" build above). Man pages
# (usr/share/man/man8) and bash-completion
# (usr/share/bash-completion/completions) dropped -- consistent with
# every other recipe in this set trimming non-essential doc/completion
# output.
# -3 closes a real "installs cleanly, then does not run" gap, caught by
# actually invoking the binary rather than trusting the install:
#
#   mtr: error while loading shared libraries: libresolv.so.2:
#        cannot open shared object file
#
# mtr resolves hostnames, so it carries a genuine DT_NEEDED on glibc's
# libresolv.so.2 -- which pkg_seed_image_baseline()'s own runtime staging
# does not include (it covers libc/libm/libtinfo/libgcc_s/libnss_files,
# the set nothing before this needed to go beyond). Staged here, from the
# same glibc the rest of the image's runtime already comes from, exactly
# as other recipes stage the shared libraries their own binaries need.
#
# Worth noting for the next network tool added: this is a general gap,
# not an mtr quirk -- anything doing name resolution will hit it, and the
# more complete fix is adding libresolv to the image baseline itself.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a /lib/x86_64-linux-gnu/libresolv.so.2 "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/bash-completion"
}
