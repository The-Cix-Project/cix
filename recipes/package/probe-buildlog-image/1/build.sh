#
# probe-buildlog-image -- attach to a build by NAME alone, while its
# chain is filed under a non-default image (#476, ADR-0297).
#
# The fix's own test cannot reproduce this half. test_pkg_build_log is
# not in SELFTESTS (it creates a real build container, which a composed
# build container cannot -- #224), and the case needs a build filed
# under some image other than "base", which there would mean
# bootstrapping a second image with its own toolchain for a three-line
# lookup rule. So it is measured here, on a real host, the way
# probe-missing-tool/1 and probe-build-clock/1 are.
#
# What it establishes:
#   - `pkg build-log --name=probe-buildlog-image`, with NO --image,
#     attaches to this build (101) while it runs. Before #476 the
#     daemon normalised the absent image to "base" and this returned
#     404 "no build in progress" -- measured on 192.168.15.95,
#     2026-09-17, against a running cix host build filed under
#     __hostbuild.
#   - `--name=` naming nothing that is building returns 404 whose
#     message names what IS, rather than asserting there is no build.
#
# Install it with --image=cix-builder (any non-default image will do).
# It ends in a deliberate `exit 1`, so the build always FAILS and the
# target image's manifest is never touched -- the probe's whole job is
# to exist for 90 seconds, not to install anything. probe-build-clock/1
# is the model, including the choice of a real small tarball as a source
# so the fetch path is exercised rather than bypassed.
#
pkg_name="probe-buildlog-image"
pkg_version="1"
pkg_source="https://mirrors.kernel.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils"
pkg_depends=""
pkg_changelog="1: a deliberately slow, deliberately failing probe so a build filed under a non-default image can be attached to by name alone (#476). test_pkg_build_log cannot reach this case -- see the header."

pkg_build() {
	i=0
	while [ "$i" -lt 18 ]; do
		echo "probe-buildlog-image: marker-$i -- attach with: cixctl pkg build-log --name=probe-buildlog-image"
		sleep 5
		i=$((i + 1))
	done
	echo "probe-buildlog-image: the attach window is over; failing deliberately so nothing installs"
	exit 1
}

pkg_install() {
	echo "probe-buildlog-image: pkg_install must never run -- pkg_build exits 1" >&2
	exit 1
}
