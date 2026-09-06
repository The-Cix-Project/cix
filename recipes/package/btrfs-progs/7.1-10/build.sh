#
# btrfs-progs -- mkfs.btrfs (DISKFORMAT_MKFS_BTRFS_BIN, daemon/src/
# diskformat.c, ADR-0103/ADR-0104: real btrfs support for the disk-
# format REST endpoint, opt-in alongside the existing ext4 default).
# Scoped to exactly this real, confirmed need -- btrfs-progs' own full
# suite (btrfs, btrfsck/btrfs check, btrfs-image, btrfstune, ~15 more
# tools) builds cleanly alongside it, but nothing in this project calls
# any of them today -- not staged here, matching e2fsprogs.recipe's own
# "don't build unused tools speculatively" precedent.
#
# Real, confirmed build-time dependencies (a real local build in this
# sandbox, `./configure` summary output + a real `ldd` on the resulting
# mkfs.btrfs): blkid, uuid, zlib via pkg-config (PKG_CHECK_MODULES in
# configure.ac, unconditional -- no --disable flag exists to skip any
# of the three). --disable-zstd/--disable-lzo avoid needing zstd/lzo
# (neither has a recipe in this catalog, and neither is needed for the
# one thing this project uses mkfs.btrfs for: formatting a container-
# storage disk, not restore/receive compression). --disable-convert
# avoids needing e2fsprogs' libext2fs (btrfs-convert, an ext2/3/4-to-
# btrfs in-place migration tool, not a capability this project offers
# for any filesystem). --disable-documentation/--disable-python avoid
# needing python3/sphinx. --disable-libudev avoids needing libudev
# (multipath device dedup, irrelevant to this project's own disk
# model, ADR-0099). --with-crypto=builtin avoids needing a crypto
# library (openssl/libgcrypt/etc) for btrfs's own checksum tree --
# confirmed via configure's own summary output: "crypto provider:
# builtin". The resulting mkfs.btrfs's own real NEEDED list (confirmed
# via ldd): libuuid.so.1, libblkid.so.1, libz.so.1, libc.so.6 -- the
# same three non-libc libraries e2fsprogs' own mke2fs already needs,
# already staged into every assembled boot image by mkbootroot.c's
# existing shelled_bin_libs[] list (added for mke2fs's own sake, Phase
# C) -- no new runtime library staging needed for this binary at all.
#
# Source is btrfs-progs' own canonical kernel.org release (the real
# maintainer-hosted mirror, same convention as e2fsprogs.recipe/
# kernel.recipe), cross-checked against the project's own GitHub
# releases page for the same tag (v7.1, confirmed the current latest
# via the GitHub releases API directly, not assumed).
#
pkg_name="btrfs-progs"
pkg_version="7.1-10"
pkg_source="https://www.kernel.org/pub/linux/kernel/people/kdave/btrfs-progs/btrfs-progs-v7.1.tar.xz"
pkg_sha256="d1f55cc2971398c9142eaa79d203e63d586a3b4b867f956664a1d68322cd4e34"
pkg_artifact_sha256="ae82d2c126e657a85a97f4bc86c3683380cc430482a67385a82a35c0f556f544"
pkg_depends="zlib libuuid libblkid pkgconf"
# ADR-0199/0209: the build environment is composed from exactly these
# and nothing else -- there is no fallback (#168). 7.1 declared none, so
# it could not be built at all, and that is not a small gap: mkbootroot
# stages mkfs.btrfs into the control-plane root only when the
# cix-hosttools artifact actually has one, so with this package
# unbuildable, no installed Cix host has had mkfs.btrfs. That is why
# formatting sda on 192.168.15.95 fell back to ext4, and it is the
# blocker under ADR-0207 phase 4.
#
#   bash coreutils  the recipe functions, nproc/mkdir/cp
#   make            the build
#   gcc libc-dev    Tier 3. TCC cannot parse `__thread`, and
#                   common/units.c uses it:
#                     static __thread int ps_index = 0;
#                     common/units.c:28: error: ';' expected (got "int")
#                   Reduced to a two-line probe -- `static __thread int
#                   x = 0;` alone fails under TCC with the identical
#                   error and compiles under gcc. This is the SAME gap
#                   CLAUDE.md already records as the reason elfutils
#                   forces gcc, so it is a known TCC limitation rather
#                   than anything about this package.
#   binutils        ar/ranlib for the internal convenience archives, ld
#   pkgconf         configure.ac uses PKG_CHECK_MODULES for blkid, uuid
#                   and zlib, unconditionally -- the recipe header
#                   already records that no --disable flag skips them
#   zlib libuuid libblkid
#                   named here as well as in pkg_depends: a composed
#                   environment is built from pkg_build_depends ALONE,
#                   so these have to be present as headers and .pc files
#                   to compile against, not merely installed alongside
#                   the result
#   sed grep gawk findutils diffutils
#                   what an autoconf configure reaches for
pkg_toolchain="gcc"
pkg_toolchain_reason="missing flag: needs -MG, a dependency-generation option TCC does not implement (#209). NOTE: the reason recorded before 2026-09-01 was \"TCC cannot parse __thread\" and was measured FALSE against tcc 0.9.28rc-10, which parses and runs __thread correctly"
pkg_build_depends="bash coreutils make gcc linux-headers binutils pkgconf zlib libuuid libblkid sed grep gawk findutils diffutils"
pkg_changelog="7.1-8: also build and install the btrfs multi-tool, not just mkfs.btrfs -- ADR-0207 makes btrfs the storage substrate and nothing could maintain one (#163)"

pkg_build() {
	CC=/usr/bin/gcc ./configure --prefix=/usr --disable-documentation --disable-python \
	            --disable-convert --disable-libudev --disable-zoned \
	            --with-crypto=builtin --disable-zstd --disable-lzo
	# DEPCOMMAND=true: skip dependency-file generation. -2 configured
	# perfectly -- its summary confirms "libudev: no" and "crypto
	# provider: builtin", exactly what this recipe intends -- and then
	# died on the first object with
	#   tcc: error: invalid option -- '-MM'
	# btrfs-progs generates .d files with `$(CC) -MM -MG -MF ...`; TCC
	# implements -MD but not -MM.
	#
	# Those files exist only to make INCREMENTAL rebuilds correct, and a
	# package build is a single clean pass over a fresh source tree that
	# is then discarded. The Makefile includes them with `-include`
	# (leading dash, line 1025), so their absence is silently tolerated
	# by design rather than by luck. Overriding the variable is upstream's
	# own seam -- no source is patched, and TCC stays the compiler, which
	# it would not with a Tier-3 exception taken for a dependency-tracking
	# flag.
	# Neither of -3's and -4's TCC workarounds is needed with gcc, and
	# both are gone rather than left in place harmlessly: gcc implements
	# -MM (so DEPCOMMAND works as upstream wrote it, and incremental
	# dependency tracking stays correct) and predefines __SIZEOF_LONG__
	# (so nothing has to assert a value on the compiler's behalf).
	# Carrying a workaround past the problem it solved is how a recipe
	# accumulates instructions nobody can later justify.
	# `btrfs` alongside mkfs.btrfs (issue #163). ADR-0207 makes btrfs
	# the platform's storage substrate, and this package shipped only
	# the tool that CREATES a filesystem -- nothing that maintains one.
	# So no Cix host could grow, scrub, or query a btrfs filesystem at
	# all: `btrfs filesystem resize` is what turns a grown partition
	# into usable space, and without it a resized btrfs disk reports
	# success and gains nothing.
	#
	# Still two named targets rather than a bare `make`, keeping this
	# recipe's deliberate economy: upstream also builds btrfs-convert,
	# btrfs-image, btrfstune and the library, none of which anything
	# here calls.
	make -j"$(nproc)" mkfs.btrfs btrfs
}

# Real files from this recipe's own build. Both go in /usr/sbin so the
# daemon's own DISKFORMAT_MKFS_BTRFS_BIN and DISKPART_BTRFS_BIN
# absolute paths resolve -- those macros are the authoritative list
# mkbootroot stages from, and a binary that is not staged fails at
# execve() on a real host while working perfectly in a build sandbox
# with a full /usr (which is exactly how sfdisk and mkfs.btrfs each
# shipped broken once).
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/sbin"
	cp mkfs.btrfs "$PKG_DESTDIR/usr/sbin/mkfs.btrfs"
	cp btrfs "$PKG_DESTDIR/usr/sbin/btrfs"
}
