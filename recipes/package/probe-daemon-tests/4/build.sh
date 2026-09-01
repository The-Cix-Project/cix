#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# #224, the question probe 2 turned up by accident.
#
# Probe 2 tried to build six daemon-linked tests and one of them did not
# COMPILE: test_pkg.c:1635, a string literal broken by a real newline.
# Nothing would ever have caught it -- the cix recipe builds eight named
# binaries and `make selftest` builds fourteen, so test_pkg is in
# neither. That is not a subtle regression; it is a syntax error sitting
# in the tree because nothing ever compiled the file.
#
# Which makes the prior question -- do the tests PASS -- premature. The
# first question is how many of the ~85 targets even BUILD. This runs
# `make -k all` (keep going, so one failure does not hide the rest) and
# reports every target that failed.
#
# Deliberately does not run anything. Compiling is the cheaper question
# and it gates the other one.
#
pkg_name="probe-daemon-tests"
pkg_version="4"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.10.5.tar.gz"
pkg_sha256="8792ed02d5719cf1f6e0575d9b07dcb80a55c4cd8cb7ca724508d97d9be1f91c"
pkg_depends=""
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils findutils grep sed gawk"
pkg_changelog="4: re-run against v2.10.5 -- does the whole tree compile now (#224)"

pkg_build() {
	echo "=== make -k all: compiling every target, not stopping at the first failure ==="
	make -k all > /run/all.log 2>&1 || true

	echo
	echo "=== targets that FAILED to build ==="
	grep -E "^make: \*\*\*|Error [0-9]+" /run/all.log | sed 's/^/  /' | sort -u | head -40
	echo
	echo "=== compiler errors ==="
	grep -E "error:" /run/all.log | sed 's/^/  /' | head -40

	echo
	built=$(ls build/ 2>/dev/null | wc -l)
	echo "=== TALLY ==="
	echo "  binaries present in build/: $built"
	echo "  distinct compile errors   : $(grep -cE 'error:' /run/all.log)"
	echo "  failed make targets       : $(grep -cE '^make: \*\*\*' /run/all.log)"
	echo
	echo "  (last 15 lines of the build log)"
	tail -15 /run/all.log | sed 's/^/    /'
	exit 1
}

pkg_install() {
	echo "probe-daemon-tests is a diagnostic; nothing is installed" >&2
	exit 1
}
