#
# probe-build-clock -- read PkgEntry's run/build clocks live (#423).
#
# run_started_at, build_started_at and run_seconds are null unless a
# run is open, which is correct and makes them unobservable on an idle
# box. Every real package here either builds too fast to catch or is
# already installed, and a reinstall is refused 409 -- the same problem
# probe-build-container/1 was written for, so this follows it exactly:
# compile nothing, install one marker, and spend the build phase
# sleeping long enough to read the entry while state is "building".
#
# What it is meant to establish, and what would be a defect:
#   - run_started_at is set at FETCH, so it must already be non-null
#     while the entry is still fetching, and must not move afterwards.
#   - build_started_at appears only once compilation begins, so it must
#     be null during fetching and >= run_started_at once set.
#   - run_seconds must climb, and must agree with the wall clock
#     between two reads rather than drifting from it.
#
pkg_name="probe-build-clock"
pkg_version="1"
pkg_source="https://mirrors.kernel.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_artifact_sha256="3f86e2ab19a4170fb7172c5cf4949d41bcb79ac8a2c004a551c9bae7f47a0b04"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils"
pkg_depends=""
pkg_changelog="1: a deliberately slow probe, so #423's run_started_at, build_started_at and run_seconds can be read while a build is genuinely in flight. Modelled on probe-build-container/1, which exists for the same reason."

pkg_build() {
	echo "probe-build-clock: sleeping so the clocks are observable"
	sleep 60
	echo "probe-build-clock: done"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-build-clock"
	echo "see #423" > "$PKG_DESTDIR/usr/share/probe-build-clock/README"
}
