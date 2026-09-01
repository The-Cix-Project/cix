#
# e2fsprogs -- mkfs.ext4 (DISKFORMAT_MKFS_EXT4_BIN, daemon/src/
# diskformat.c, multi-disk management Phase C). Scoped to exactly this
# real, confirmed need -- mke2fs and its mkfs.ext2/mkfs.ext3/mkfs.ext4
# hardlinks (upstream's own convention, confirmed via a real local
# build+install: all four are the identical binary, same inode).
# e2fsprogs' own full suite (e2fsck, debugfs, tune2fs, resize2fs,
# dumpe2fs, badblocks, ~25 more tools) is real and builds cleanly
# alongside it, but nothing in this project calls any of them today --
# not staged here, matching this recipe set's own "don't build unused
# tools speculatively" precedent (curl.recipe/squashfs-tools.recipe).
#
# Source is e2fsprogs' own canonical kernel.org release (the real,
# actual upstream release host, not just a mirror), checksum verified
# against a second independent source: GitHub's own tag archive for
# the identical version -- content-identical (verified via a real
# `diff -r` of both extracted trees; GitHub's own auto-generated
# archive isn't byte-reproducible against the maintainer's own release
# tarball even for identical source, so a raw sha256 comparison would
# give a false mismatch).
#
# 1.47.4-2, one real fix from 1.47.4 -- the first real build-image
# rebuild of this recipe (building cix-hosttools, task #865) hit a
# genuine build failure this recipe's own unqualified `make -j` had
# never actually exercised before: the bare `all` target builds every
# PROG_SUBDIRS entry (e2fsck, misc, and friends), not just misc/, and
# e2fsck's own link step fails under TCC with a long list of undefined
# ext2fs_*/__dso_handle symbols -- a real linking gap in a tool this
# recipe never needed or shipped in the first place (see header:
# pkg_install() only ever copies misc/mke2fs). Fixed the correct way:
# e2fsprogs' own top-level Makefile.in defines PROG_SUBDIRS as a plain,
# overridable `=` variable ("e2fsck $(DEBUGFS_DIR) misc $(RESIZE_DIR)
# tests/progs", confirmed via the real fetched source tree) that
# all-progs-recursive iterates directly, and all-progs-recursive
# already explicitly depends on all-libs-recursive completing first
# (confirmed in the same Makefile.in) -- so `make PROG_SUBDIRS=misc`
# builds every library mke2fs itself needs, then descends into misc/
# alone, never touching e2fsck/debugfs/resize2fs/tests at all. Not a
# source patch, not a "skip the failing step and hope": a real,
# upstream-supported build-scope narrowing to exactly the one binary
# this recipe has only ever needed.
#
pkg_name="e2fsprogs"
pkg_version="1.47.4-6"
pkg_source="https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.4/e2fsprogs-1.47.4.tar.gz"
pkg_sha256="da274408bebbfd13a5a2fc3cfc66e3ffff17c48534673aa67f88d49b99123b96"
pkg_artifact_sha256="1db9819280bd5b7e3ca181557eb61e0a72ef82150f5bd5495f3ca464f42d6981"
pkg_depends=""
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf"
pkg_changelog="1.47.4-6: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 1.47.4-5: also rewrites _INLINE_ to empty, since inline.c's extern-inline path emits file-local symbols under TCC and exported nothing. 1.47.4-4: -DNO_INLINE_FUNCS, because TCC advertises C99 without implementing C99 inline, so e2fsprogs' _INLINE_ functions were never emitted. 1.47.4-3: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

# Plain autotools (a pre-generated ./configure ships in the release
# tarball, confirmed -- no autoconf/automake needed to build it).
# --disable-nls skips gettext/locale infrastructure (matching this
# recipe set's own doc/locale-stripping convention elsewhere).
# PROG_SUBDIRS=misc is the real TCC-compatibility fix this revision
# adds (see header) -- narrows the program build to exactly what
# pkg_install() below actually ships.
pkg_build() {
	#

	# -DNO_INLINE_FUNCS, and the reason is the same root cause as m4's:

	# TCC defines a real C99 __STDC_VERSION__ without implementing C99

	# inline semantics. lib/ext2fs/bitops.h picks _INLINE_ purely from

	# that macro, lands on plain `inline`, and a bare inline definition

	# provides no external definition -- so nothing ever emits

	# ext2fs_fast_clear_bit and friends, and every caller fails to link:

	#

	#   tcc: error: undefined symbol 'ext2fs_fast_clear_bit'

	#

	# NO_INLINE_FUNCS is e2fsprogs' own designed-in escape (its bitops.h

	# says so in as many words) and compiles them as real functions.

	#

	export CPPFLAGS="${CPPFLAGS:+$CPPFLAGS }-DNO_INLINE_FUNCS"

	# -DNO_INLINE_FUNCS alone was not enough, and the remaining half is
	# the same TCC gap wearing a different hat. With it defined, the only
	# translation unit that still compiles the bodies is inline.c, which
	# defines INCLUDE_INLINE_FUNCS itself -- and that path selects
	# `extern inline`, purely on __STDC_VERSION__ again.
	#
	# In real C99 `extern inline` DOES provide the external definition,
	# which is exactly why upstream chose it. TCC emits it as a
	# FILE-LOCAL symbol instead (verified: nm shows lowercase t), so
	# inline.o compiles happily and exports nothing, and every caller in
	# every other object still fails to link:
	#
	#   tcc: error: undefined symbol 'ext2fs_fast_clear_bit64'
	#
	# Making _INLINE_ empty gives inline.c ordinary external definitions.
	# It is safe precisely BECAUSE NO_INLINE_FUNCS is set above: every
	# other translation unit skips the bodies entirely, so there is
	# exactly one definition of each and no duplicate-symbol risk.
	sed -i 's/^#define _INLINE_ extern inline$/#define _INLINE_/' \
		lib/ext2fs/bitops.h lib/ext2fs/kernel-jbd.h lib/ext2fs/ext2fs.h
	if grep -q '^#define _INLINE_ extern inline$' \
		lib/ext2fs/bitops.h lib/ext2fs/kernel-jbd.h lib/ext2fs/ext2fs.h; then
		echo "e2fsprogs: an 'extern inline' _INLINE_ survived the rewrite" >&2
		exit 1
	fi

	CC=tcc ./configure --prefix=/usr --disable-nls
	make -j"$(nproc)" PROG_SUBDIRS=misc
}

# Real files from this recipe's own build -- just mke2fs (mkfs.ext4 is
# the identical binary, upstream's own hardlink, confirmed via a real
# local install: all four mkfs.ext{2,3,4}/mke2fs share one inode).
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/sbin"
	cp misc/mke2fs "$PKG_DESTDIR/usr/sbin/mkfs.ext4"
}
