# SCRATCH DIAGNOSTIC, v2. v1 ruled out every "unsafe entry" rule in
# cbs_extract_archive(): the bzip2 1.0.8 tarball extracts to 56 regular
# files, 1 directory, and zero symlinks, hardlinks, devices or FIFOs.
#
# So the rejection is not about an entry's TYPE. The remaining candidate
# in that function is ORDER: it mkdir()s only for AE_IFDIR members and
# creates no parent directories for file members, so an archive that
# lists a file before the directory containing it would fail at open()
# with ENOENT and report libarchive's own empty error as "unsafe
# archive". This prints the raw member list, in archive order, which is
# the one thing that distinguishes that hypothesis from every other.
pkg_name="probe-bzip2-entries"
pkg_version="2"
pkg_source="https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz"
pkg_sha256="ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269"
pkg_depends=""
pkg_build_depends="bash coreutils findutils tar gzip"

pkg_build() {
	echo "=== where is the fetched tarball? ==="
	find /build -maxdepth 2 -name '*.tar.gz' -o -maxdepth 2 -name '*.tgz' | head
	ls -la /build || true
	echo "=== first 12 members, in archive order ==="
	for f in $(find /build -maxdepth 2 -type f -name '*.tar.gz' | head -1); do
		echo "listing $f"
		tar -tvzf "$f" | head -12
		echo "--- does it contain a directory member at all? ---"
		tar -tzf "$f" | grep -c '/$' || true
	done
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-bzip2-entries"
	echo done > "$PKG_DESTDIR/usr/share/probe-bzip2-entries/done.txt"
}
