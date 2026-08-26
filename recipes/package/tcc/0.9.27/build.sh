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
pkg_version="0.9.27"
pkg_source="https://download.savannah.nongnu.org/releases/tinycc/tcc-0.9.27.tar.bz2"
pkg_sha256="de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/tcc-0.9.27.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="5c18c4aae349c72dd9d2c37100f82dffad2882355ec6e5bbb4c0abeacfda3d52"
pkg_depends=""

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
	./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
