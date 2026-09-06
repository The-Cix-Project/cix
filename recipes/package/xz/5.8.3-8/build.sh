#
# xz -- the LZMA2 compressor CLI. GNU tar (tar.recipe) has no
# compression code of its own and shells out to a bare "xz" resolved
# via $PATH for every .tar.xz source (the same real gap this recipe
# set's own bzip2.recipe/gzip.recipe already close for their own
# formats).
#
# **IMPORTANT security note carried forward from upstream's own
# release announcement**: 5.8.3 fixes CVE-2026-34743 (a buffer
# overflow in lzma_index_append() reachable only via a narrow,
# unlikely API-usage pattern -- xz's own release notes call it
# unlikely to be triggerable by any real-world application, including
# this project's own plain CLI extraction use). Pinned here anyway,
# as the current, real, non-vulnerable release.
#
# Source is xz's own canonical GitHub release (the project's actual
# upstream host, tukaani-project/xz), checksum verified against a
# second independent source: Debian's own orig tarball content
# (different compression format, .tar.xz vs. this recipe's .tar.gz --
# verified via a real `diff -r` of both extracted trees, byte-for-byte
# identical, rather than a raw file checksum since the two container
# formats can never produce the same sha256 regardless of content).
#
# 5.8.3-2, one real fix from 5.8.3 -- the first real build-image
# rebuild of this recipe (building cix-hosttools, task #865) hit a
# genuine TCC gap that 5.8.3's own "not rebuild-verified" disclosure
# (task #845's CC=tcc audit) had flagged as a real, open risk:
# src/liblzma/common/common.c's own HAVE_SYMBOL_VERSIONS_LINUX branch
# expands LZMA_SYMVER_API() into a raw __asm__(".symver ...") ELF
# symbol-versioning directive TCC's assembler does not implement
# ("error: unknown opcode '.symver'"), gated behind configure's own
# --enable-symbol-versions probe (on by default when the target looks
# ELF/glibc-like, which this one genuinely is -- the probe itself
# isn't wrong, TCC's own assembler support is just narrower than
# real GNU as). Fixed the correct way -- a real, upstream-documented
# `./configure --disable-symbol-versions` flag (confirmed via a local
# `./configure --help` against this exact source tree), not a source
# patch: this project never modifies unmodified upstream code, only
# ever configures it correctly for the toolchain actually building it.
# Symbol versioning only matters for a shared library shipping
# multiple, simultaneously-installed ABI-compatible versions side by
# side on a glibc system with versioned symbols already in play --
# irrelevant here, this recipe's own liblzma.so is the only copy ever
# present on any image it's installed onto.
#
pkg_name="xz"
pkg_version="5.8.3-8"
pkg_source="https://github.com/tukaani-project/xz/releases/download/v5.8.3/xz-5.8.3.tar.gz"
pkg_sha256="3d3a1b973af218114f4f889bbaa2f4c037deaae0c8e815eec381c3d546b974a0"
pkg_artifact_sha256="e3828c45658a3ecf15793b2918a5d4fa2b947f47228d9d0b21b456257a76ca68"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/xz-5.8.3-3.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
# Nothing at runtime: liblzma links against libc alone.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from exactly
# these and nothing else, so this list is not documentation -- it is
# the environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with CC=tcc
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln, used throughout configure,
#             config.status and the Makefile
#   sed       an autoconf configure rewrites its own output with sed on
#             essentially every substitution it makes
#   grep      the same, for every feature test that greps a compiler or
#             header for a pattern
#   gawk      config.status generates every Makefile through awk --
#             measured, not assumed: a build environment without it
#             fails at `config.status: line NNNN: awk: command not
#             found` (see diffutils 3.10-3)
#   binutils  gnulib is archived into a static convenience library
#             before the final link, which needs ar and ranlib
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils"
pkg_changelog="5.8.3-8: install to /usr/lib rather than the multiarch directory (#184 stage 2). The multiarch triplet is a Debian convention for letting several architectures share one filesystem and a Cix image has one architecture, so this platform is collapsing onto a single library directory. Nothing about consumers changes: glibc's compiled-in search path is exactly slibdir plus libdir, and libdir has been /usr/lib since 2.44-7, so a linker and a loader both find the library there today. The artifact approval is dropped because those bytes came from the previous revision. 5.8.3-7: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 5.8.3-6: keep the pkg-config file -- this package ships the headers and .so it describes (#174)"


# Plain autotools, confirmed directly. --disable-doc skips the real
# generated HTML/man doc tree (matching every other recipe's own
# doc-stripping convention) -- xz's own configure calls this "doc",
# not "docs" (confirmed via a real local `./configure --help`, unlike
# curl's own --disable-docs spelling above). --disable-symbol-versions
# is the real TCC-compatibility fix this revision adds (see header).
pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-doc --disable-symbol-versions
	make -j"$(nproc)"
}

# Real files from this recipe's own build. usr/share (man pages in a
# dozen languages) is dropped, matching every other recipe's own
# doc-stripping convention. The lzcat/lzma/unlzma/etc. compatibility
# symlinks and xzdiff/xzgrep/xzless shell-script wrappers are real,
# genuinely part of upstream's own install, and kept -- they're plain
# files/symlinks alongside the real xz binary, no extra weight to
# justify stripping them individually the way the large translated doc
# tree is.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share"
	# liblzma stays where the build put it (#184). This used to move the
	# three shared-library files into the multiarch directory
	# immediately after installing them into /usr/lib -- undoing the
	# install to satisfy a layout convention rather than a requirement.
	rm -f "$PKG_DESTDIR/usr/lib/liblzma.a" "$PKG_DESTDIR/usr/lib/liblzma.la"
	# Issue #174: the generated pkg-config file is KEPT now.
	#
	# It used to be deleted on the premise that nothing in this project
	# uses pkg-config. That premise has expired -- 20+ current recipes
	# use it -- and this package genuinely ships the headers and the
	# shared library its .pc file describes, so stripping it made
	# pkg-config report "not found" for something really provided here.
	#
	# The rule (docs/guides/writing-recipes.md): ship a .pc exactly when
	# you ship the dev files it describes. Deleting it is still correct
	# for a deliberately runtime-only package such as procps, where a
	# restored .pc would make pkg-config SUCCEED and hand out paths to
	# files that are not in the package -- a false claim being worse
	# than a missing one.
}
