#
# probe-build-container -- verify PkgEntry.build_container live (#407).
#
# The negative half of that field's state gate is easy to check on an
# idle box: every installed package reports null. The positive half
# needs a build that is actually in flight when the endpoint is read,
# and every real package on this host either builds too fast to catch
# or is already installed (a reinstall is refused 409).
#
# So this recipe exists to be slow on purpose. It compiles nothing,
# installs one marker file, and spends its build phase sleeping -- long
# enough to read GET /v1/pkg/probe-build-container@cix-builder while
# state is "building" and see the container name the tab needs.
#
# Deliberately cheap: no pkg_source fetch of any consequence, no
# compiler, and its build_depends is the smallest set that gives it a
# shell and a sleep.
#
pkg_name="probe-build-container"
pkg_version="1"
pkg_source="https://mirrors.kernel.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils"
pkg_depends=""
pkg_changelog="1: a deliberately slow probe, so PkgEntry.build_container can be read while a build is genuinely in flight (#407)."

pkg_build() {
	echo "probe-build-container: sleeping so the build is observable"
	sleep 45
	echo "probe-build-container: done"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-build-container"
	echo "see #407" > "$PKG_DESTDIR/usr/share/probe-build-container/README"
}
