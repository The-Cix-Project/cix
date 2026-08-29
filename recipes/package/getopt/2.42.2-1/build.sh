#
# getopt -- GNU getopt(1), the long-option argument normaliser.
#
# From util-linux, the same source libuuid.recipe and libblkid.recipe
# already build, at the same version and checksum. util-linux's own
# --disable-all-programs plus a single --enable-<thing> switch makes
# this the established shape here: one package, one job.
#
# Added because ccan's create-ccan-tree (a build step of
# sbsigntools.recipe) parses its arguments with
#   getopt -o ab: --long copy-all,build-type: ...
# and failed in a composed build environment with
#   create-ccan-tree: line 21: getopt: command not found
# Bash's builtin getopts cannot substitute: it has no --long support at
# all, which is precisely what that call uses.
#
# The alternative was to patch the argument parsing out of third-party
# source to route around a tool we simply did not have. That is the
# kind of workaround this project's own tenets call a hack, and it
# would have left the next recipe needing getopt to rediscover the same
# gap. A real package costs one small build and is reusable.
#
pkg_name="getopt"
pkg_version="2.42.2-1"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
pkg_depends=""
# ADR-0199/0209: composed from exactly these, no fallback (#168).
# Plain ./configure && make with everything but getopt switched off.
pkg_build_depends="bash coreutils make tcc libc-dev sed grep gawk binutils pkgconf findutils"

pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-all-programs --enable-getopt
	make -j"$(nproc)"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp -a getopt "$PKG_DESTDIR/usr/bin/"
	# Asserted rather than assumed: a configure that quietly did not
	# build getopt would otherwise produce an empty package whose
	# absence only surfaces in whatever recipe next declares it.
	test -x "$PKG_DESTDIR/usr/bin/getopt" || {
		echo "getopt: expected output missing -- configure did not build it" >&2
		exit 1
	}
}
