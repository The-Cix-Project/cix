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
pkg_version="2.42.2-3"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
pkg_artifact_sha256="b97b518fcd713d40316c0e776f1489f2089d7d599355c744dfed1725c816490b"
pkg_depends=""
# ADR-0199/0209: composed from exactly these, no fallback (#168).
# Plain ./configure && make with everything but getopt switched off.
pkg_build_depends="bash coreutils make tcc linux-headers sed grep gawk binutils pkgconf findutils"
pkg_changelog="2.42.2-3: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

pkg_build() {
	# NOT "--disable-all-programs --enable-getopt", which -1 tried by
	# analogy with libuuid.recipe and which cannot work. Read from
	# util-linux's own m4/ul.m4: UL_BUILD_INIT registers no
	# AC_ARG_ENABLE for a PROGRAM -- it reads $enable_getopt, but when
	# --disable-all-programs sets ul_default_estate=no that branch
	# takes precedence and forces build_getopt=no regardless. autoconf
	# duly warned "unrecognized options: --enable-getopt" and the build
	# then succeeded while producing nothing:
	#   cp: cannot stat 'getopt': No such file or directory
	# libuuid gets away with the same shape only because LIBRARIES use
	# a different macro that does register the option.
	#
	# The supported route is the one configure prints itself -- "Type
	# 'make' or 'make <utilname>' to compile": configure normally, then
	# build the single target. util-linux is non-recursive automake, so
	# the binary lands at the top of the build tree as ./getopt.
	CC=tcc ./configure --prefix=/usr --without-python --without-systemd --without-udev
	make getopt
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
