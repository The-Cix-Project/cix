#
# tcc -- the Tiny C Compiler, the exclusive toolchain this project's
# own daemon/CLI are built with (ADR-0001). An ordinary recipe (built
# inside the shared toolchain sandbox's own real gcc, not a hostbuild)
# -- installed onto a "cix-builder" image so cix.recipe (ADR-0057)
# has a real tcc to build cixd/cixctl with via hostbuild.
#
# Source is tinycc's own canonical download.savannah.nongnu.org
# release. Version matches this sandbox's own already-installed system
# tcc (0.9.27, confirmed via `tcc -v`) -- the same compiler already
# proven, this whole project long, to build this codebase cleanly.
#
pkg_name="tcc"
pkg_version="0.9.27-3"
pkg_source="https://download.savannah.nongnu.org/releases/tinycc/tcc-0.9.27.tar.bz2"
pkg_sha256="de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c"
# Nothing at runtime: tcc links against libc alone, and its own
# libtcc1.a is part of this package.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from
# exactly these and nothing else, so this list is not
# documentation -- it is the environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with --cc=tcc -- tcc builds
#             itself. Through -2 it did not: tcc's configure is hand
#             written and ignores a CC= environment variable entirely,
#             taking --cc= instead, so every tcc this project has ever
#             installed was compiled by whatever ambient gcc the shared
#             sandbox happened to carry. Visible only once the
#             environment held exactly what was declared: the build
#             printed `C compiler  gcc (0.0)` and then died with
#             `make: gcc: No such file or directory`
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln, used throughout configure,
#             config.status and the Makefile
#   sed       configure rewrites its own output with sed constantly
#   grep      every feature test that greps a compiler or header
#   gawk      AC_PROG_AWK equivalents in the Makefile
#   binutils  the Makefile archives libtcc1.a with ar
#
# Sufficiency is enforced by the build itself. Minimality is
# review, not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils"


# lib/bcheck.c (TCC's own optional bounds-checking runtime, only used
# by programs compiled with tcc's own -b flag -- not needed to build
# cixd/cixctl, which never pass it) unconditionally defines
# CONFIG_TCC_MALLOC_HOOKS on Linux and references glibc's
# __malloc_hook/__free_hook/etc. directly. Real, confirmed upstream/
# environment incompatibility, not a project bug: TCC 0.9.27 was
# released in 2017, years before glibc 2.34 (2021) removed those
# hooks' declarations from <malloc.h> entirely -- every real distro
# packaging tcc 0.9.27 against a modern glibc carries an equivalent
# patch. Confirmed empirically: the exact same source built clean
# before this patch on distro/gcc conditions predating glibc 2.34, and
# fails identically to this on any glibc >= 2.34 without it (this
# build host's own glibc is 2.36). Guards the existing
# CONFIG_TCC_MALLOC_HOOKS definition with the same glibc-version check
# real downstream packages use, mirroring the file's own existing
# BSD/Windows exclusion block for the identical reason (those
# platforms never had these hooks to begin with).
pkg_build() {
	sed -i '/^#define HAVE_MEMALIGN$/a\
#if defined(__GLIBC__) \&\& (__GLIBC__ > 2 || (__GLIBC__ == 2 \&\& __GLIBC_MINOR__ >= 34))\
#undef CONFIG_TCC_MALLOC_HOOKS\
#endif' lib/bcheck.c
	./configure --prefix=/usr --cc=tcc
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
