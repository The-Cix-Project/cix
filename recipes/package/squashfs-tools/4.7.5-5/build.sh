#
# squashfs-tools -- unsquashfs, this project's real, only need from
# this project (PKG_UNSQUASHFS_BIN, daemon/src/pkg.c's
# pkg_bootstrap_from_toolchain() import path). mksquashfs/sqfstar/
# sqfscat come along for free from the same build (one shared object
# file set) and are staged too, though nothing in this project's own
# code currently shells out to them -- cixd does its own squashfs
# assembly via image/src/mkbootroot.c/mktoolchainimage.c's own
# execve("/usr/bin/mksquashfs", ...) calls, which is a real, separate
# staged binary already, not this recipe's concern.
#
# Deliberately built with ONLY XZ_SUPPORT=1 -- a direct grep of every
# "-comp" flag this project's own code ever passes to mksquashfs
# (image/src/mkbootroot.c, image/src/mktoolchainimage.c) confirms
# "xz" is the only compression format this project ever produces or
# needs to read back. The upstream Makefile defaults ALL of gzip/lzo/
# lz4/zstd/xattr support to on, which would need three more from-
# source recipes (liblzo2, liblz4, libzstd) for compression backends
# this project never uses -- matching curl.recipe's own "don't build
# unused backends" reasoning. COMP_DEFAULT=xz keeps the build from
# refusing to compile with gzip (the Makefile's own hardcoded default)
# disabled.
#
# Source is squashfs-tools' own canonical GitHub repo (its only real
# distribution point -- no separate release-tarball site, confirmed),
# checksum verified against a second independent source (Debian's own
# orig tarball) -- byte-identical content (verified via `diff -r` on
# both extracted trees; GitHub's own auto-generated tag archive isn't
# byte-reproducible against Debian's repackaging, so a raw sha256
# comparison would give a false mismatch even for identical content).
#
# 4.7.5-2 (then -3), two real fixes from 4.7.5 -- the first real build-image
# rebuild of this recipe (building cix-hosttools, task #865) hit
# the same real, already-documented TCC/regex.h gap CLAUDE.md's own
# environment notes describe for daemon/src/logstore.c ("TCC can't
# parse glibc's own <regex.h> regexec() prototype as written... a
# real C99 VLA-in-prototype size expression... #define
# __STDC_NO_VLA__ 1 before #include <regex.h> steers the header onto
# [a branch TCC parses fine]"): mksquashfs.c/read_fs.c both #include
# <regex.h> directly for their own -regex/-iregex filter matching and
# hit the identical "'__nmatch' undeclared" error. Fixed the correct
# way for an unmodified third-party source tree (logstore.c can add
# the #define directly since it's this project's own code; this can't
# patch upstream) -- the Makefile's own EXTRA_CFLAGS extension point
# (confirmed via the real fetched source: "CFLAGS += $(EXTRA_CFLAGS)
# ..." is the very first thing appended to the base CFLAGS, before any
# feature-specific -D flag), which a command-line
# EXTRA_CFLAGS=-D__STDC_NO_VLA__=1 populates -- functionally identical
# to a #define at the top of the file, no source touched.
#
# A second, independent TCC gap surfaced right after the first was
# fixed: reader.c's own atomic_swap.h uses __atomic_exchange_n()/
# __ATOMIC_SEQ_CST, GCC/Clang C11 atomic builtins TCC doesn't
# implement at all. Also already anticipated by upstream, not patched:
# the Makefile has a real, first-class DONT_USE_ATOMIC_EXCHANGE_N=1
# variable (confirmed in the fetched source, "ifeq
# ($(DONT_USE_ATOMIC_EXCHANGE_N),1) CFLAGS += -DDONT_USE_ATOMIC_EXCHANGE_N")
# that switches reader.c onto its own portable non-atomic-builtin
# fallback path -- passed as an ordinary make variable exactly like
# this recipe's existing GZIP_SUPPORT=0/XZ_SUPPORT=1/etc.
#
# 4.7.5-4, a third real fix -- 4.7.5-3 built cleanly (both TCC gaps
# above resolved) and `mkbootroot`'s own real, live squashfs assembly
# on the real box ran it and reported success, but the resulting
# image failed `POST /system/update`'s own squashfs-magic validation
# ("image_path is not a squashfs image") -- looked at first like a
# genuine runtime correctness bug from concurrency (see -5 below for
# the real cause; this fix stayed since it's still correct in its own
# right, just not the actual culprit). Root cause investigated:
# DONT_USE_ATOMIC_EXCHANGE_N's own portable fallback is a plain,
# non-atomic read-modify-write, safe only single-threaded -- but this
# Makefile's own default reader-thread configuration
# (SMALL_READER_THREADS=4, BLOCK_READER_THREADS=4, real POSIX
# threads, confirmed in the fetched Makefile) still runs multiple
# real concurrent threads through that exact non-atomic path, a
# genuine data race TCC's own lack of real atomic-builtin support
# can't be papered over for under real concurrency. Fixed via the
# Makefile's own first-class SINGLE_READER_THREAD=1 variable
# (confirmed in the fetched source: "ifeq
# ($(SINGLE_READER_THREAD),1) CFLAGS += -DSINGLE_READER_THREAD"),
# upstream's own documented single-threaded mode.
#
# 4.7.5-5, the real fix -- -4's own SINGLE_READER_THREAD=1 did NOT
# actually fix the corrupt output (confirmed: a direct, isolated test
# -- running THIS EXACT BINARY inside a real container built from its
# own image, bypassing mkbootroot/LD_LIBRARY_PATH entirely -- produced
# a fully "successful"-looking mksquashfs run whose own first 4 output
# bytes read "sqsh", the exact byte-reversal of the real magic
# ("hsqs", confirmed against daemon/src/main.c's own documented
# on-disk magic check) -- a genuine, systemic BYTE-SWAP bug, not
# corruption or a race. Root cause: this project's own build/test
# directly confirmed TCC never predefines the bare `linux` macro GCC
# always does on a Linux target (`tcc -c` on a trivial `#ifdef linux`
# probe fails to compile) -- and squashfs-tools' own
# `endian_compat.h` branches its entire byte-order detection on
# exactly that macro (`#ifndef linux` -> a BSD-style `<sys/types.h>`
# fallback path; `#else` -> the correct, real glibc `<endian.h>`).
# Under TCC, every build silently took the wrong branch, computing
# the host's own byte order incorrectly and byte-swapping every
# multi-byte on-disk field it ever wrote, magic number included --
# with mksquashfs itself completely unaware anything was wrong (its
# own internal logic is fully symmetric with whatever byte order it
# believes it's running on, so a wrong-but-internally-consistent
# answer produces a fully "successful," fully self-consistent, and
# fully corrupt-to-every-other-reader file). Fixed the correct way,
# again a real upstream-anticipated escape hatch rather than a source
# patch: `-Dlinux=1` on the command line steers the exact same
# `#ifndef linux` check GCC's own predefined macro would already
# satisfy for real, taking the correct `<endian.h>` branch -- this
# is not inventing new behavior, it's supplying the one predefined
# macro a real GCC build already has that TCC happens not to.
#
pkg_name="squashfs-tools"
pkg_version="4.7.5-5"
pkg_source="https://github.com/plougher/squashfs-tools/archive/refs/tags/4.7.5.tar.gz"
pkg_sha256="547b7b7f4d2e44bf91b6fc554664850c69563701deab9fd9cd7e21f694c88ea6"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/squashfs-tools-4.7.5-5.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="d79bfaa16928cd234faf7980321f3bec93d28bc56f6fff14d6c2b457d9826bba"
pkg_depends="xz"

# Plain hand-written Makefile (no autotools), confirmed via a real
# local build. Real, empirically confirmed via `readelf -d` on the
# resulting binaries: unsquashfs/mksquashfs need only libm/liblzma/
# libc -- no gzip/lzo/lz4/zstd/xattr library dependency at all with
# these flags. EXTRA_CFLAGS=-D__STDC_NO_VLA__=1 -Dlinux=1 and
# DONT_USE_ATOMIC_EXCHANGE_N=1 are the real TCC-compatibility fixes
# this revision adds (see header) -- -Dlinux=1 is the one that
# actually matters for correct output; the rest are needed to build
# at all.
pkg_build() {
	cd squashfs-tools
	make -j"$(nproc)" CC=tcc GZIP_SUPPORT=0 XZ_SUPPORT=1 LZO_SUPPORT=0 \
	                  LZ4_SUPPORT=0 ZSTD_SUPPORT=0 XATTR_SUPPORT=0 \
	                  COMP_DEFAULT=xz EXTRA_CFLAGS="-D__STDC_NO_VLA__=1 -Dlinux=1" \
	                  DONT_USE_ATOMIC_EXCHANGE_N=1 SINGLE_READER_THREAD=1
}

# No DESTDIR-aware install target in this Makefile (confirmed) --
# files copied directly. sqfscat/sqfstar are real symlinks to
# unsquashfs/mksquashfs respectively (upstream's own convention,
# confirmed via `ls -la` on the build output), kept as-is.
#
# No "squashfs-tools/" prefix here (confirmed live: an earlier version
# of this line had one and every real build failed at this exact step,
# "cannot stat 'squashfs-tools/mksquashfs'") -- pkg_build() and
# pkg_install() run as two calls in the *same* shell session (pkg.c's
# own build invocation: ". recipe.sh; cd /build/src && pkg_build &&
# pkg_install"), so pkg_build()'s own "cd squashfs-tools" is still in
# effect here; this function is already inside that directory, not
# back at /build/src.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp -a mksquashfs unsquashfs sqfscat sqfstar "$PKG_DESTDIR/usr/bin/"
}
