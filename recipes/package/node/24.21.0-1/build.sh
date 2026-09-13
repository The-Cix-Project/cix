#
# node -- Node.js 24.x LTS (Krypton), built from source with Cix's own
# gcc. The JavaScript runtime the agent CLIs need (claude-code is an npm
# package; herdr detects Node-based agents). No prebuilt binary ever
# enters this -- V8 and all of Node's bundled deps compile here (Build
# Provenance Mandate).
#
# Source is nodejs.org's own release tarball, checksum taken from the
# bytes downloaded and cross-checked against nodejs.org's own
# SHASUMS256.txt for v24.21.0.
#
pkg_name="node"
pkg_version="24.21.0-1"
pkg_source="https://nodejs.org/dist/v24.21.0/node-v24.21.0.tar.gz"
pkg_sha256="622424efb5dc0c26c93fbb619ff10737ee289c605b837c88778e186925d82777"

# node's own binary links libstdc++/libgcc_s (it is C++), plus libz for
# the bundled zlib's shared link and the usual glibc set. The exact
# runtime deps are confirmed from the built ELF on first install and
# declared here rather than guessed -- expect gcc's runtime libs and
# zlib. Left minimal until measured (elfcheck, ADR-0251, names anything
# missing precisely).
pkg_depends=""

# node's configure is a Python program and its build drives make + a C++
# toolchain over V8. gcc (with g++) and python are the load-bearing
# ones; the rest are the ordinary shell/text tools its build scripts use.
pkg_build_depends="bash coreutils make gcc binutils python sed grep gawk findutils diffutils"
pkg_toolchain="gcc"
pkg_toolchain_reason="Node.js is C++ (V8); its configure and build assume a GCC/Clang-compatible driver and its own bundled deps compile with the system C++ toolchain -- TCC is not a language runtime's toolchain (same basis as go/gcc/kernel)."
pkg_changelog="24.21.0-1: first packaging. Node.js 24.x LTS built from source with Cix gcc; --with-intl=small-icu (bundled, no download) and npm kept. Built with -j2 to stay within the host's ~7.7 GiB against V8's ~8 GiB/-j4 guidance."

pkg_build() {
	# Node's build wants a writable TMP and HOME; these minimal build
	# sandboxes have /run but not necessarily /tmp (Phase 23/33 note).
	mkdir -p /run/nodetmp
	export TMPDIR=/run/nodetmp
	export HOME=/run

	# configure is python; call the real interpreter explicitly.
	# --with-intl=small-icu uses Node's BUNDLED small ICU (English), so
	# nothing is fetched and the Intl surface still exists for tools that
	# probe for it. --without-corepack drops the yarn/pnpm shim we do not
	# need; npm stays. --shared-* are left OFF so V8/zlib/etc. are the
	# bundled, source-built copies (provenance), not host libraries.
	python3 ./configure \
		--prefix=/usr \
		--with-intl=small-icu \
		--without-corepack \
		--ninja=false 2>/dev/null || \
	python3 ./configure \
		--prefix=/usr \
		--with-intl=small-icu \
		--without-corepack

	# -j2, not $(nproc): V8 peaks near 8 GiB of RAM at -j4 and this host
	# has ~7.7 GiB, so a wider build risks the OOM killer mid-link. Slow
	# and safe beats fast and wedged.
	make -j2
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	# Trim what a runtime image never needs: node's own headers, the
	# npm/node man pages, and the bundled test corpora. Keep node, npm,
	# npx and the npm library tree (claude-code installs through npm).
	rm -rf "$PKG_DESTDIR/usr/include/node" \
	       "$PKG_DESTDIR/usr/share/man" \
	       "$PKG_DESTDIR/usr/share/doc"
	find "$PKG_DESTDIR/usr/lib/node_modules" -type d -name test -prune -exec rm -rf {} + 2>/dev/null || true
}
