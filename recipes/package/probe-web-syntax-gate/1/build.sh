#
# probe-web-syntax-gate -- does test_web_syntax actually reject a
# dashboard file that does not parse? (#340, ADR-0294)
#
# The gate's own self_check() proves quickjs accepts valid JavaScript
# and rejects a dangling `else` -- the exact shape of the 2026-09-08
# outage -- but it proves that over two string literals. What it does
# NOT prove is the end-to-end link: a real file on disk, read through
# read_file(), reported as a failure, exiting non-zero so `make
# selftest` fails the build.
#
# That link cannot be proven by the gate passing. A gate that has
# never rejected anything is a gate nobody has tested, and the project
# rule is to reintroduce the bug and watch the test catch it. So this
# probe fetches the same source the release builds from, breaks
# web/app.js in exactly the way the refactor broke it, and asks the
# real binary about the real file.
#
# It uses the binary's argv form (test_web_syntax takes file paths),
# so it needs neither a generated web/api.js nor a full build -- one
# tcc invocation against test/test_web_syntax.c and -lquickjs.
#
# EXPECTED RESULT: this install FAILS, in pkg_build(), with the probe
# reporting that the broken file was REJECTED. A successful install
# means the gate did not fire and #340 is not actually closed.
#
pkg_name="probe-web-syntax-gate"
pkg_version="1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.192.tar.gz"
pkg_sha256="154545d42b75ac5ecba5e5db6ef7178f49b47b969347cf36d541000c309c2b0d"
pkg_build_image="cix-builder"
pkg_depends=""
pkg_build_depends="bash coreutils tcc quickjs grep sed"
pkg_changelog="1: proves the #340 web syntax gate rejects a real unparseable web/app.js, not just the string literals in its own self-check. Expected to FAIL the install."

pkg_build() {
	tcc -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 \
		-Iinclude -Inetplane/include \
		test/test_web_syntax.c -lquickjs -lm -o test_web_syntax

	# Control first: the file as shipped must be accepted. Without
	# this, a binary that rejected everything would look like a pass.
	if ! ./test_web_syntax web/app.js; then
		echo "probe: FAIL -- the shipped web/app.js was REJECTED." >&2
		echo "       The gate has a false positive, which is worse" >&2
		echo "       than no gate at all." >&2
		exit 1
	fi
	echo "probe: control passed -- the shipped web/app.js parses"

	# Now the outage. A refactor removed one arm of an if/else and
	# left the `else` behind, so the file's own braceless `if` style
	# put a perfectly legal `;` in front of it -- which is why a
	# textual check cannot tell this from correct code.
	printf '\nif (0) { }\nelse { }\nelse { }\n' >> web/app.js

	if ./test_web_syntax web/app.js; then
		echo "probe: FAIL -- a web/app.js with a dangling else was" >&2
		echo "       ACCEPTED. The gate does not fire on the very" >&2
		echo "       shape it was written for (#340)." >&2
		exit 1
	fi

	echo "probe: the broken web/app.js was REJECTED -- the gate works."
	echo "probe: failing deliberately now, so this never installs."
	exit 1
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-web-syntax-gate"
	echo "if this file was installed, the #340 gate did not fire" \
		> "$PKG_DESTDIR/usr/share/probe-web-syntax-gate/RESULT"
}
