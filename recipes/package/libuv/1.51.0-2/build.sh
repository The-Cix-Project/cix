#
# libuv -- bind's own bin/dig (dig/host/nslookup) hard-requires
# libuv >= 1.0 via pkg-config (confirmed directly in BIND's own
# configure.ac: AC_MSG_ERROR if missing, no way to build without it
# even though bin/dig itself never links it). This recipe exists
# purely as that prerequisite, for the jump box's own nslookup.
#
# Real GitHub release tag; ships no pre-generated ./configure (only
# autogen.sh + CMakeLists.txt -- no cmake recipe exists in this
# project, so autotools is the only real path).
#
# ACLOCAL/AUTOMAKE overridden to small `perl <path> "$@"` wrapper
# scripts rather than the bare tool names autogen.sh defaults to --
# confirmed directly (a scratch diagnostic build) that this build
# sandbox's own /usr/bin/automake and /usr/bin/aclocal are dangling
# Debian update-alternatives-style symlink chains (baked in from
# whatever real host `pkg bootstrap`'s own live-copy-from-host
# originally captured -- a real, pre-existing toolchain artifact, not
# introduced by this recipe) that the kernel's own #! shebang exec
# path fails to resolve (ENOEXEC, so bash's shell fallback then
# mis-parses the perl source as shell -- "package: command not
# found"), even though the exact same file opens and runs fine when
# perl is invoked on it directly rather than relying on shebang exec.
# autogen.sh itself already supports this override (reads
# ACLOCAL/AUTOMAKE/LIBTOOLIZE from the environment, confirmed by
# reading it directly) -- no patch to libuv's own source needed.
#
pkg_name="libuv"
pkg_version="1.51.0-2"
pkg_source="https://github.com/libuv/libuv/archive/refs/tags/v1.51.0.tar.gz"
pkg_sha256="27e55cf7083913bfb6826ca78cde9de7647cded648d35f24163f2d31bb9f51cd"
pkg_artifact_sha256="18b2c262393a3b71b63925282005e593eeb79dd17f201c3489fd91b55e1e3f4e"
pkg_depends=""
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils autoconf automake libtool m4"
pkg_changelog="1.51.0-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils autoconf automake libtool m4"

pkg_build() {
	mkdir -p /build/toolwrap
	printf '#!/bin/sh\nexec perl /usr/bin/aclocal "$@"\n' > /build/toolwrap/aclocal-wrap
	printf '#!/bin/sh\nexec perl /usr/bin/automake "$@"\n' > /build/toolwrap/automake-wrap
	printf '#!/bin/sh\nexec perl /usr/bin/libtoolize "$@"\n' > /build/toolwrap/libtoolize-wrap
	chmod +x /build/toolwrap/aclocal-wrap /build/toolwrap/automake-wrap /build/toolwrap/libtoolize-wrap

	ACLOCAL=/build/toolwrap/aclocal-wrap AUTOMAKE=/build/toolwrap/automake-wrap \
	LIBTOOLIZE=/build/toolwrap/libtoolize-wrap ./autogen.sh
	CC=tcc ./configure --prefix=/usr --disable-static
	make -j"$(nproc)"
}

pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -f "$PKG_DESTDIR"/usr/lib/*.la
}
