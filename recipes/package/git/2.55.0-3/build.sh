#
# git -- the distributed version control system. Built as a real,
# unprivileged runtime dependency for gitea (see gitea.recipe's own
# header comment: gitea shells out to a real `git` binary at runtime
# for its actual repository operations) but equally useful standalone.
#
# Read by cixd's own non-executing metadata scanner (pkg_name=/
# pkg_version=/pkg_source=/pkg_sha256=/pkg_depends=, daemon/src/pkg.c's
# parse_recipe()) AND sourced as a real POSIX shell script inside the
# isolated, network-less build container (". /build/recipe.sh" --
# daemon/include/pkg.h's own documented contract) to run pkg_build()/
# pkg_install() below. Never sourced or executed on the host itself.
#
# Source verified against kernel.org's own published sha256sums.asc for
# this exact release, and independently reproduced by downloading the
# tarball directly and running sha256sum against it before this recipe
# was written.
#
# NO_RUST=1: recent git versions (this one included) added an optional
# Rust component (a Cargo-built libgitcore.a) -- found by an actual
# build failing ("cargo: not found"), not anticipated up front. This
# project's own toolchain has no Rust/Cargo staged at all, and git's own
# Makefile explicitly documents NO_RUST as the supported way to disable
# it and fall back to the plain-C implementation, not a hack.
# NO_GETTEXT=1: this build host has no `msgfmt` (gettext) staged either
# -- found the same way, a real build failure trying to generate git-
# gui's own translation catalogs. Only affects translated *messages*,
# not functionality.
# NO_TCLTK=1: skips git-gui/gitk entirely -- both are Tcl/Tk desktop
# GUI tools, meaningless for a headless server binary gitea shells out
# to, and this project's toolchain has no Tcl/Tk staged regardless.
# All three flags are passed identically to both pkg_build() and
# pkg_install() -- install: all in git's own Makefile means `make
# install` re-triggers the same build-from-scratch dependency chain,
# and a flag mismatch between the two steps would silently retrigger
# the exact cargo/msgfmt failures these disable in the first place.
#
pkg_name="git"
pkg_version="2.55.0-3"
pkg_source="https://www.kernel.org/pub/software/scm/git/git-2.55.0.tar.xz"
pkg_sha256="457fdb04dc8728e007d4688695e6912e6f680727920f2a40bf11eacc17505357"
pkg_depends=""
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf zlib"
pkg_changelog="2.55.0-3: -D__STDC_NO_VLA__=1 for glibc regex.h, whose regexec() prototype TCC cannot parse. 2.55.0-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf zlib"

# Run with $PWD already at /build/src (the extracted tarball, one
# leading path component already stripped) and PATH=/usr/bin:/bin -- no
# network access. Git ships a pre-generated ./configure (no autoconf
# regeneration needed); zlib/OpenSSL/PCRE2 headers and libs it links
# against are already part of this toolchain's own wholesale
# /usr/{include,lib} staging (test_image_fixture_stage_toolchain()),
# confirmed directly -- configure found everything it needed with no
# extra staging.
pkg_build() {
	#
	# glibc declares regexec()'s array parameter with a real C99
	# VLA-in-prototype size expression referencing the NEXT parameter,
	# and TCC's parser rejects it outright:
	#
	#   /usr/include/.../regex.h:686: error: '__nmatch' undeclared
	#
	# The header has its own __STDC_NO_VLA__ branch for exactly this
	# situation -- a compiler without VLA support -- which TCC parses
	# fine. Defining it is accurate rather than a trick: TCC really
	# does not support this construct. Same fix as
	# daemon/src/logstore.c's own include block.
	#
	export CPPFLAGS="${CPPFLAGS:+$CPPFLAGS }-D__STDC_NO_VLA__=1"

	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" NO_RUST=1 NO_GETTEXT=1 NO_TCLTK=1
}

# PKG_DESTDIR is set by cixd itself (daemon/src/pkg.c) -- everything
# written under it is what actually gets merged into the target image
# once this build container exits successfully. Lands the real `git`
# binary at usr/bin/git plus its own libexec/git-core/* helper binaries
# (git-upload-pack, git-receive-pack, git-remote-http, ...) every real
# git operation -- clone, fetch, push, smart-HTTP -- actually needs at
# runtime, not just the one top-level binary.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" prefix=/usr NO_RUST=1 NO_GETTEXT=1 NO_TCLTK=1
}
