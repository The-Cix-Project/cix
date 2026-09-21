# SCRATCH DIAGNOSTIC. Answers one question and is then disposable.
#
# cbs refuses bzip2-1.0.8.tar.gz at the source stage with
# CPDL-E6001 "archive format or entry is unsupported: unsafe archive",
# while cixd's own libarchive extraction of the SAME tarball has worked
# for every bzip2 revision this platform has shipped. So the tarball is
# fine and cbs_extract_archive() is stricter somewhere; its message is a
# catch-all emitted whenever libarchive itself reported no error, so it
# names no entry and no rule.
#
# Reading the source narrowed it to: a char/block device, a FIFO, a
# symlink with an absolute or escaping target, a hardlink, or a path
# crossing a symlinked directory. This lists exactly those, from the
# tree cixd extracted, so the upstream ticket can name the entry instead
# of the guess.
pkg_name="probe-bzip2-entries"
pkg_version="1"
pkg_source="https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz"
pkg_sha256="ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269"
pkg_artifact_sha256="f3b3de8ac4bab765b81ae7dedcbf40357745f09b96769d4495812c6690be59d7"
pkg_depends=""
pkg_build_depends="bash coreutils findutils"

pkg_build() {
	echo "=== non-regular, non-directory entries ==="
	find . \( -type l -o -type p -o -type s -o -type c -o -type b \) \
	     -printf '%y %p -> %l\n' || true
	echo "=== hardlinked regular files (nlink > 1) ==="
	find . -type f -links +1 -printf '%n %p\n' || true
	echo "=== entry count by type ==="
	find . -type f -printf 'f\n' | wc -l
	find . -type d -printf 'd\n' | wc -l
	find . -type l -printf 'l\n' | wc -l
	echo "=== top level ==="
	ls -la
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-bzip2-entries"
	echo done > "$PKG_DESTDIR/usr/share/probe-bzip2-entries/done.txt"
}
