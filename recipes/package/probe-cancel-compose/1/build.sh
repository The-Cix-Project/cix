#
# probe-cancel-compose 1 -- can `pkg cancel` reach a build-environment
# COMPOSITION? (#339)
#
# Written because the fix cannot be proven where it was written. On
# 192.168.15.95 an ordinary install goes fetching -> building in about
# three seconds, because the build environment its declared tools need
# has already been composed and is reused. Cancel then catches the
# FETCH, which is #239's path and was never broken. The composer branch
# -- the one #339's fix adds -- is only reachable while a composition
# is actually running.
#
# So this declares a deliberately UNUSUAL set of build tools. A build
# environment is keyed by a hash of its declared tools, so a set nothing
# else has asked for has no composed image to reuse, and the install
# sits in a real composition long enough to cancel into it.
#
# The tools are otherwise arbitrary and the build never runs: the point
# is the composition before it, and the recipe exits non-zero so that a
# run which is NOT cancelled installs nothing either.
#
pkg_name="probe-cancel-compose"
pkg_version="1"
pkg_source=""
pkg_build_depends="bash coreutils grep sed gawk findutils tar gzip make tcc binutils linux-headers openssl gcc"

pkg_build() {
	echo "=== if this line appears, the composition FINISHED and the cancel missed its window ==="
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}

pkg_install() {
	:
}
