#
# minisign -- the independent verifier test_releasekey checks our
# signatures against (#406).
#
# This package exists to make an existing test actually run. ADR-0220's
# signature format is gated by test_releasekey, which is in SELFTESTS,
# and whose first action is to skip entirely when minisign is absent --
# and minisign was in no recipe and no image, so it skipped on every
# release from ADR-0220 until now. Four assertions about the format that
# stands between a substituted installer ISO and a machine that boots it
# were never checked on a Cix host.
#
# The point of an independent oracle is that it is independent: the test
# says so itself, "checking our own format with our own reader would
# prove only that we are self-consistent". ADR-0279 made that sharper by
# adding releasekey_verify_file() -- now BOTH directions matter, our
# output accepted by stock minisign and its output accepted by ours, and
# neither is worth anything if the only verifier present is the one
# under test.
#
# Compiled by invoking tcc on the four source files directly, rather
# than through upstream's CMake. sysklogd.recipe established this for a
# package "where pulling in the entire binutils toolchain as a
# build-only dependency would be disproportionate"; here the reason is
# simpler still. minisign IS four .c files, the CMakeLists does nothing
# but find libsodium and pass -pthread, and cmake is not in this build
# image. Driving a build system to compile four files is the cost, not
# the saving.
#
# No -pthread: the pinned tcc accepts it now (#216) but glibc >= 2.34
# folds pthread_create into libc.so.6, so on this platform it is a flag
# that selects nothing. chrony.recipe carries a whole wrapper to strip
# it; not adding it is cheaper than stripping it.
#
pkg_name="minisign"
pkg_version="0.12-2"
pkg_source="https://github.com/jedisct1/minisign/archive/refs/tags/0.12.tar.gz"
pkg_sha256="796dce1376f9bcb1a19ece729c075c47054364355fe0c0c1ebe5104d508c7db0"
pkg_build_image="cix-builder"
# linux-headers because glibc's own <limits.h> reaches for
# <linux/limits.h> through bits/local_lim.h -- the -1 revision
# omitted it and died on the very first include of the very first
# source file. Nothing about minisign asks for kernel headers;
# compiling any C at all against this glibc does.
pkg_build_depends="tcc bash coreutils linux-headers libsodium"
pkg_depends="libsodium"
pkg_changelog="0.12-2: declare linux-headers -- glibc's <limits.h> pulls <linux/limits.h> via bits/local_lim.h, and the -1 revision failed on minisign.c line 4. 0.12-1: minisign, the independent oracle test_releasekey has been skipping for want of (#406)."

pkg_build() {
	# ADR-0276: libsodium is declared in pkg_depends because this
	# binary genuinely links it, and the install-time check compares
	# what it links against what it declared.
	tcc -Wall -O2 -Isrc \
	    -o minisign \
	    src/minisign.c src/helpers.c src/base64.c src/get_line.c \
	    -lsodium

	# A binary that cannot answer -v is not a verifier, and finding
	# that out here costs one build instead of one release: this
	# package's entire purpose is to be run by a test, so "it compiled"
	# is not the property that matters.
	./minisign -v
}

pkg_install() {
	# mkdir + cp + chmod, not `install -D`: no other recipe in this set
	# uses install(1), so whether coreutils' copy is present in this
	# build image is an assumption rather than a fact, and the mode
	# matters here -- GET /containers/{name}/files carries no mode, and
	# an artifact that installs a binary as -rw-r--r-- passes every
	# checksum and then fails execve (#139).
	mkdir -p "$PKG_DESTDIR/usr/bin" "$PKG_DESTDIR/usr/share/man/man1"
	cp minisign "$PKG_DESTDIR/usr/bin/minisign"
	chmod 0755 "$PKG_DESTDIR/usr/bin/minisign"
	cp share/man/man1/minisign.1 "$PKG_DESTDIR/usr/share/man/man1/minisign.1"
}
