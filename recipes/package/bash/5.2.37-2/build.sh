#
# bash -- the GNU Bourne-Again SHell.
#
# Read by thincd's own non-executing metadata scanner (pkg_name=/
# pkg_version=/pkg_source=/pkg_sha256=/pkg_depends=, daemon/src/pkg.c's
# parse_recipe()) AND sourced as a real POSIX shell script inside the
# isolated, network-less build container (". /build/recipe.sh" --
# daemon/include/pkg.h's own documented contract) to run pkg_build()/
# pkg_install() below. Never sourced or executed on the host itself.
#
# Source verified against two independent GNU mirrors (ftp.gnu.org and
# mirrors.kernel.org) -- byte-identical, same sha256.
#
pkg_name="bash"
pkg_version="5.2.37-2"
pkg_source="https://ftp.gnu.org/gnu/bash/bash-5.2.37.tar.gz"
pkg_sha256="9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff"
# Nothing at runtime beyond libc: this build takes no readline (the
# shell's own line editing is compiled in) and no NLS.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from
# exactly these and nothing else, so this list is not
# documentation -- it is the environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with CC=tcc
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln, used throughout configure,
#             config.status and the Makefile
#   sed       configure rewrites its own output with sed constantly
#   grep      every feature test that greps a compiler or header
#   gawk      AC_PROG_AWK, and config.status's own substitution fallback
#   binutils  bash builds lib/*.a convenience archives before linking
#
# Sufficiency is enforced by the build itself. Minimality is
# review, not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils"


# Run with $PWD already at /build/src (the extracted tarball, one
# leading path component already stripped) and PATH=/usr/bin:/bin --
# no network access. bash builds its own bundled ./lib/readline copy
# automatically whenever no suitable external readline is found, so no
# --without-* flags are needed either way; if the pkgbuild toolchain
# image does carry readline/ncurses headers (pkg_bootstrap_build_image()
# stages whatever the host itself has), configure uses those instead,
# which is fine either way.
pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)"
}

# PKG_DESTDIR is set by thincd itself (daemon/src/pkg.c) -- everything
# written under it is what actually gets merged into the shared "base"
# image once this build container exits successfully.
#
# /bin/sh: this project's own images deliberately have no /bin at all
# (usr/bin-only FHS convention, see CONSOLE_DEFAULT_CMD in main.c) --
# fine for a minimal runtime container, but a real, load-bearing gap
# for any image meant to actually build software: glibc's popen()/
# system() are POSIX-specified to always exec "/bin/sh" literally,
# with NO override mechanism (not $SHELL, not a SHELL= make variable,
# nothing) -- confirmed empirically the hard way (ADR-0056) via the
# Linux kernel's own scripts/kconfig/preprocess.c, which calls
# popen() to evaluate Kconfig's $(shell ...) macros. Without /bin/sh,
# that popen() call fails deep inside glibc's posix_spawn fast path
# and (misleadingly) surfaces as ENOMEM, not the expected ENOENT --
# not a thinC bug, a real gap in any build-toolchain image. Any
# image that installs bash now also gets a standard /bin/sh symlink,
# matching how every real Linux distribution guarantees this exact
# path exists.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	mkdir -p "$PKG_DESTDIR/bin"
	ln -sf /usr/bin/bash "$PKG_DESTDIR/bin/sh"
}
