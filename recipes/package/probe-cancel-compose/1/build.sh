#
# probe-cancel-compose 1 -- can `pkg cancel` reach a build-environment
# COMPOSITION? (#339)
#
# Written because the fix cannot be proven where it was written. On
# 192.168.15.95 an ordinary install goes fetching -> building in about
# three seconds, because the build environment its declared tools need
# has already been composed and is reused. Cancel then catches the
# FETCH -- that is #239's path and was never broken. The composer
# branch, the one #339's fix adds, is reachable only while a
# composition is actually running.
#
# So this declares a deliberately UNUSUAL set of build tools. A build
# environment is keyed by a hash of its declared tools, so a set nothing
# else has asked for has no composed image to reuse, and the install
# sits in a real composition long enough to cancel into it.
#
# The source is the same tarball probe-cgroup-view/2 uses, with the
# same checksum: those exact bytes are already fetched and verified on
# this host, and nothing here reads them. The tools are otherwise
# arbitrary and the build never matters -- the point is the composition
# before it, and the recipe exits non-zero so a run that is NOT
# cancelled installs nothing either.
#
pkg_name="probe-cancel-compose"
pkg_version="1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.55.25.tar.gz"
pkg_sha256="c6b847f00098e607e6d5d627e23cbbdf118e9a981bf3f6e95cf231d256313b12"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils grep sed gawk findutils tar gzip make tcc binutils linux-headers openssl gcc"
pkg_changelog="1: force a real build-environment composition, so the composer branch of pkg_cancel() (#339) can be exercised on a host where every ordinary build environment is already composed."

pkg_build() {
	echo "=== if this line appears, the composition FINISHED and the cancel missed its window ==="
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
