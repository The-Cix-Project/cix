#
# cmake 4.4.3-2 -- first working packaging.
#
# Added because fastfetch is CMake-only (its CMakeLists.txt line 1 is
# `cmake_minimum_required(VERSION 3.12.0)` and there is no autotools
# path), and nothing in this recipe set could build a CMake project at
# all. That makes this a dependency with reach well beyond one package:
# a large share of current C and C++ software ships CMake and nothing
# else, so every one of them was previously unbuildable here.
#
# Source is cmake.org rather than the GitHub release asset. Both carry
# the same tarball, and CLAUDE.md records cixd's own fixed curl failing
# specific GitHub release-asset redirect chains twice with a bare
# `curl exit 1` -- a fetch path this platform has already been bitten
# by is a poor choice for a build-only dependency that every later
# CMake package will pull. Bytes fetched twice from cmake.org and
# hashed identically before this recipe was written.
#
# BOOTSTRAP, not a normal build. CMake is written in C++ and built by
# CMake, so the tarball ships a `./bootstrap` script that compiles a
# minimal cmake with the host compiler first and then uses it to
# configure the real one. That is why this recipe does not look like
# the other configure-driven recipes here.
#
pkg_name="cmake"
pkg_version="4.4.3-2"
pkg_source="https://cmake.org/files/v4.4/cmake-4.4.3.tar.gz"
pkg_sha256="c46400618b4f1f2b43507f24fb22f3ae830c3416cf23b776e16e1d413aa892f0"
pkg_artifact_sha256="00e7e6fe5a644ba3192afeb4aea85c35256a14e9e398a31fe836793591758be6"
pkg_depends=""
#
# ADR-0224 as amended by ADR-0226: gcc, recorded for auditability rather
# than permission.
#
pkg_toolchain="gcc"
pkg_toolchain_reason="language: CMake is C++17 and TCC has no C++ front end at all, so this is not a TCC gap to fix or track -- it is the wrong tool, the same reasoning btop 1.4.7-1 records. Measured against gcc 16.2.0-13, which is installed in cix-builder with g++, libstdc++ and the C++ headers."
#
# ADR-0199/0209: the build environment is composed from exactly these.
#   gcc            g++, libstdc++ and the C++ headers -- the compiler
#   make           bootstrap generates Makefiles and then runs make
#   bash           bootstrap is a shell script; pkg_build() runs under it
#   coreutils      bootstrap's own uname/dirname/mkdir/cp/rm/printf/wc
#   sed grep       bootstrap's feature detection is sed/grep throughout
#   findutils      used while assembling the bootstrap source list
#   binutils       ar/ranlib/ld for a real C++ link
#   glibc          the C library the result links against
#   linux-headers  kernel uapi headers, which glibc's own headers include
#   openssl        TLS for CMake's bundled curl -- see below
#   pkgconf        how CMake's OpenSSL detection finds it (openssl ships
#                  libssl.pc/libcrypto.pc/openssl.pc)
#
# OPENSSL IS REQUIRED, and revision 1 got this wrong. Its comment
# asserted that CMake "vendors all of them under Utilities/" and that
# declaring the packaged copies "would not be used" -- true for zlib,
# libuv, expat and the rest, and FALSE for OpenSSL. CMake vendors curl;
# it does not vendor curl's TLS backend. The bootstrap therefore died at
# configure with:
#
#   CMake Error at Utilities/cmcurl/CMakeLists.txt:1014 (message):
#     Could not find OpenSSL.  Install an OpenSSL development package or
#     configure CMake with -DCMAKE_USE_OPENSSL=OFF to build without OpenSSL.
#
# That error names both options. This takes the first deliberately:
# -DCMAKE_USE_OPENSSL=OFF would build, and would produce a cmake whose
# `file(DOWNLOAD https://...)` cannot do TLS at all -- a silent
# limitation that would surface much later, inside somebody else's
# CMake project, as a download failing for no visible reason. The
# platform has a real openssl (3.0.20-5, measured present in
# cix-builder with 135 headers and its own .pc files), so cmake gets
# real TLS from Cix's own OpenSSL rather than none.
#
# Still deliberately NOT declared: curl, zlib, bzip2, xz, libuv, expat.
# Those really are vendored under Utilities/ and `--no-system-libs` is
# the bootstrap default. The cost is stated rather than hidden: the
# cmake binary carries its own copies, so a CVE in one is fixed by
# bumping cmake, not by bumping the platform's package.
pkg_build_depends="gcc make bash coreutils sed grep findutils binutils glibc linux-headers openssl pkgconf"
pkg_changelog="4.4.3-2: declares openssl and pkgconf. Revision 1 failed to bootstrap at all -- its comment claimed CMake vendors every dependency, which is false for OpenSSL: CMake vendors curl but not curl's TLS backend, so cmcurl's configure refused. Building with -DCMAKE_USE_OPENSSL=OFF was the other option the error names and is deliberately not taken, because it yields a cmake whose file(DOWNLOAD https://...) silently cannot do TLS."

pkg_build() {
	#
	# --parallel is passed explicitly: bootstrap defaults to serial, and
	# this is a large C++ build.
	#
	# --no-qt-gui and --no-system-libs are the defaults and are stated
	# anyway, because both are load-bearing here -- a Qt GUI has no place
	# in a build image, and system libs are deliberately not used (see
	# the dependency note above).
	#
	./bootstrap \
		--prefix=/usr \
		--parallel="$(nproc 2>/dev/null || echo 4)" \
		--no-qt-gui \
		--no-system-libs

	make -j"$(nproc 2>/dev/null || echo 4)"

	#
	# A build-time gate, not a smoke test. The whole point of this
	# package is that it can configure and build somebody else's CMake
	# project, and `cmake --version` proves only that a binary starts.
	# So: configure and build a real two-file project with the cmake
	# just built, and require the resulting binary to run and print what
	# it was told to.
	#
	# This exists because a bootstrap can produce a cmake that reports a
	# version and then fails on the first real project -- the failure
	# mode this recipe would otherwise ship.
	#
	gate=/run/cmake-gate
	rm -rf "$gate" && mkdir -p "$gate/src"
	cat > "$gate/src/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.12.0)
project(cixgate C)
add_executable(cixgate main.c)
EOF
	cat > "$gate/src/main.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("cmake-gate-ok\n"); return 0; }
EOF
	./bin/cmake -S "$gate/src" -B "$gate/build" -DCMAKE_BUILD_TYPE=Release
	./bin/cmake --build "$gate/build"
	out=$("$gate/build/cixgate")
	if [ "$out" != "cmake-gate-ok" ]; then
		echo "the cmake just built could not build a trivial project (got: $out)" >&2
		exit 1
	fi
	echo "=== cmake gate: configured, built and ran a real project ==="
	rm -rf "$gate"
}

pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	#
	# Refuse to ship an installation missing the binary everything else
	# will invoke. An incomplete install here fails at the next
	# package's configure step instead, naming that package rather than
	# this one.
	#
	if [ ! -x "$PKG_DESTDIR/usr/bin/cmake" ]; then
		echo "usr/bin/cmake missing from the artifact" >&2
		exit 1
	fi
}
