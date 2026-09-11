#
# probe-minisign -- is stock minisign actually reachable in a build
# container that declares it? (#406)
#
# This exists because the question cannot be answered from the release
# log. test_releasekey skips its oracle half when `command -v minisign`
# fails, and the build harness prints a test's stdout only when the test
# FAILS -- so a run that skipped and a run that exercised stock minisign
# produce the identical line, "test_releasekey PASS". Installing
# minisign into cix-builder and adding it to cix's own
# pkg_build_depends are both necessary, and neither is visible in the
# artefact that would tell you it worked.
#
# The composed-environment rule is the whole reason this is not
# obvious, and it caught this work twice on the way here: libsodium's
# first revision died on a missing grep, minisign's on missing
# linux-headers. A build container contains precisely what the recipe
# declares -- so "minisign is installed in cix-builder" says nothing
# about whether the container running the selftest has it.
#
# EXPECTED RESULT: this install SUCCEEDS and the build log shows a real
# minisign version banner. A FAILED install with "minisign: command not
# found" means a container declaring minisign does not get one, and
# test_releasekey is still skipping whatever its PASS line says.
#
# Source is GNU hello, the same tarball probe-missing-tool uses: a probe
# needs a real checksum-verified fetch to reach pkg_build() at all, and
# reusing one already proven to fetch keeps this probe about its one
# question.
#
pkg_name="probe-minisign"
pkg_version="1"
pkg_source="https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils minisign"

pkg_build() {
	# Exactly the test's own check, so this probes what test_releasekey
	# probes rather than something adjacent to it.
	if ! command -v minisign > /dev/null 2>&1; then
		echo "probe-minisign: minisign is NOT on PATH in this container --" >&2
		echo "  test_releasekey's oracle half is still skipping (#406)" >&2
		exit 1
	fi
	echo "probe-minisign: minisign found at $(command -v minisign)"
	minisign -v

	# And that it can do the one thing the oracle is for: verify a
	# signature over a file. A minisign that runs but cannot verify
	# would satisfy `command -v` and fail the test it exists to serve.
	minisign -G -W -p probe.pub -s probe.key > /dev/null
	printf 'probe payload\n' > probe.bin
	minisign -S -l -s probe.key -m probe.bin -t 'probe-minisign #406' > /dev/null
	minisign -Vm probe.bin -p probe.pub
	echo "probe-minisign: a real sign/verify round trip completed"
}

pkg_install() {
	# Nothing to ship. The answer is in the build log.
	mkdir -p "$PKG_DESTDIR/usr/share/doc/probe-minisign"
	echo "probe-minisign ran; see the build log" \
	    > "$PKG_DESTDIR/usr/share/doc/probe-minisign/README"
}
