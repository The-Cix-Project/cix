#
# linux-headers -- the kernel's own userspace API headers (asm/,
# asm-generic/, linux/, mtd/, rdma/, scsi/, sound/, video/, drm/),
# installed by `make headers_install` from the exact kernel source this
# platform builds and boots.
#
# The first step of #181: glibc cannot be built without Linux kernel
# headers, and the whole point of building glibc ourselves is that
# nothing underneath it comes from another distribution. Taking Debian's
# kernel headers to build our own libc would have missed the point
# entirely -- and would have described a kernel we do not run, since
# these headers define the syscall and structure ABI between userspace
# and the kernel.
#
# Same kernel source and checksum as kernel.recipe (6.18.40), because
# they must describe the same kernel. Nothing is compiled here:
# headers_install runs the kernel's own header-sanitising step and
# copies the result, which is why this needs no toolchain beyond make
# and the usual text tools.
#
pkg_name="linux-headers"
pkg_version="6.18.40-5"
# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# Built and published by 192.168.15.95; added after publication, never
# carried forward from another version. Required for test-floor
# membership (ADR-0209), which verifies each artifact against the
# checksum in its own recipe -- this is the package that replaces
# libc-dev's kernel UAPI headers there, as it already has in every real
# recipe (#187).
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431"
pkg_depends=""

# ADR-0199/0209: composed from exactly these, no fallback (#168).
#
# gcc is here despite nothing of the kernel being compiled: before
# headers_install runs, the kernel's build system builds its own
# scripts/basic/fixdep with HOSTCC. -1 left it out on the reasoning
# that a header-copying target needs no compiler, and the build said
# otherwise immediately:
#
#   HOSTCC  scripts/basic/fixdep
#   /bin/sh: line 1: gcc: command not found
#
# Named as "gcc" rather than pinned to a path because the kernel's own
# host-tool rules invoke it by bare name through $(HOSTCC).
pkg_toolchain="gcc"
pkg_toolchain_reason="missing flag: the build passes -MMD,<path>, a dependency-generation option TCC does not implement (#209)"
pkg_changelog="6.18.40-5: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"
pkg_build_depends="bash coreutils make gcc binutils sed grep gawk findutils diffutils perl"

pkg_build() {
	# INSTALL_HDR_PATH puts them under a staging prefix; pkg_install
	# below moves the include/ tree into place. ARCH must be named
	# explicitly: headers_install otherwise guesses from uname, and a
	# build container is not necessarily the machine that will run
	# these headers.
	# "headers", not "headers_install". The two do the same
	# sanitising work -- the HDRINST steps that write the cleaned
	# headers into the build tree's own usr/include -- but
	# headers_install adds a final copy to INSTALL_HDR_PATH that the
	# kernel performs with rsync:
	#
	#   INSTALL /build/hdr/include
	#   /bin/sh: line 1: rsync: command not found
	#
	# rsync is not packaged here, and packaging a file-synchronisation
	# tool to copy a directory that pkg_install() is about to copy
	# anyway would be a poor trade. "headers" stops after generating
	# them, which is exactly the part that matters.
	#
	# HOSTCC by absolute path. The kernel's own default is a bare
	# "gcc", which in this build environment resolved to TCC and failed
	# on a dependency-generation flag TCC does not implement:
	#
	#   tcc: error: invalid option -- '-MMD,scripts/basic/.fixdep.d'
	#
	# Pinning it is also what CLAUDE.md requires of any gcc invocation
	# here: a bare name makes gcc compute its installation prefix
	# relatively and fail cc1 with a misleading posix_spawnp error.
	make ARCH=x86 HOSTCC=/usr/bin/gcc headers
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/include"
	cp -a usr/include/. "$PKG_DESTDIR/usr/include/"

	# Asserted rather than assumed: these four are the ones glibc's own
	# configure looks for, and a headers_install that silently produced
	# nothing would otherwise surface much later as a confusing glibc
	# configure failure.
	for h in linux/version.h asm/unistd.h asm-generic/unistd.h linux/errno.h; do
		test -f "$PKG_DESTDIR/usr/include/$h" || {
			echo "linux-headers: $h missing from headers_install output" >&2
			exit 1
		}
	done
	echo "installed kernel headers: $(find "$PKG_DESTDIR/usr/include" -name '*.h' | wc -l) files"
	grep -m1 LINUX_VERSION_CODE "$PKG_DESTDIR/usr/include/linux/version.h"
}
pkg_artifact_sha256="5cd03e3376c4640bd7a05b578855935ca6ae880a6c6cfe6e516e914ea880e315"
