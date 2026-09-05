#
# cix-aggressive-test -- does the control plane stay up when something
# is actively trying to stop it?
#
# This is a HOSTILE package, deliberately separate from cix-tests. That
# package asserts the platform works, across roughly seventy binaries
# and every subsystem. This one asserts a single, different thing: that
# cixd cannot be wedged. There are no scenarios to pass here, only a
# daemon that either kept answering or did not, and one verdict line
# saying which.
#
# WHY IT HAD TO EXIST
#
# cixd is single-threaded. One epoll loop serves the API, the container
# runtime and every long operation, so any blocking call anywhere in it
# is a total outage -- no API, no health endpoint, and no way to read
# the diagnostic that already knows the answer.
#
# Two of those have now been found the hard way and both cost a manual
# reset of a real host. #283 was during a C++ package build. #294 was a
# console pty master, opened blocking and read from the epoll loop, and
# on 2026-09-05 it held 192.168.15.95 in n_tty_read for over seventeen
# minutes with the box otherwise idle -- load 0.19, kernel healthy, a
# container serving SSH logins throughout.
#
# Each was diagnosed after the fact, from a wchan, one at a time. That
# is the pattern this package exists to end: forty-seven blocking
# waitpid sites and a blocking run_cmd remain, and reading them one by
# one will never converge on an answer. Attack is a better instrument
# than inspection, and a build either survives it or is not fit to
# ship.
#
# WHAT IT ATTACKS AND WHAT IT MEASURES
#
# The harness spawns its OWN cixd on its own port and data directory,
# the same way sixteen other daemon-linked tests do, and attacks that
# one. Production is never in the blast radius -- and there is a
# sharper reason than caution: a build log is relayed through cixd, so
# a harness that successfully wedged the daemon it reported through
# would report nothing at all. That is not a hypothesis either; the tcc
# build in flight during the #294 outage left a zero-byte log.
#
# A prober process is forked before the first attack and asks for
# health every 100ms on its own connection, recording the longest gap
# between consecutive answers and which attack was running at the time.
# The threshold for failure is five seconds, chosen to equal
# stallwatch's own stall threshold so that the harness and the daemon's
# watchdog cannot disagree about what a stall is. The inner daemon's
# stallwatch records are then read straight off disk and cross-checked,
# never through the API -- which is precisely the retrieval path that
# fails when it matters.
#
# HOW IT IS KNOWN TO WORK
#
# A harness that has only ever passed proves nothing. This one was run
# against a cixd with the #294 fix reverted and reported the wedge, and
# against the fixed build and reported a clean run. Both numbers belong
# in this package's log.
#
# WHAT A PASS DOES NOT MEAN
#
# It does not mean cixd cannot be wedged. It means these attacks did
# not wedge it. The general answer is ADR-0246 -- a supervisor able to
# restart a worker that stops answering -- and when that lands, what a
# pass means changes with it: today a pass is never wedged, afterwards
# it becomes wedged and self-recovered, with a manual reset being the
# only failing outcome.
#
# Fails the build on a finding, deliberately, so this can gate a
# release rather than merely inform one.
#
pkg_name="cix-aggressive-test"
pkg_version="1.0.0"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.53.43.tar.gz"
pkg_sha256="4561c0aa6ab1f792e6495f0a69ed8124ffa401b62690abd45b99e8cf6f190f6e"
pkg_depends=""
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils util-linux e2fsprogs btrfs-progs iproute2 ncurses squashfs-tools findutils sed grep gawk"
pkg_build_image="toolchain"

pkg_build() {
	#
	# Only what the harness itself needs, rather than `all`: this
	# package's whole point is to be runnable often and quickly enough
	# that it can gate a release, and building seventy test binaries to
	# run one of them would work against that.
	#
	make CIX_VERSION="$pkg_version" build/cixd build/test_aggressive build/daemon_child \
	     build/console_term_child

	echo "=== 1/2: hostile run against this build -- must survive ==="
	if ! build/test_aggressive; then
		echo "cix-aggressive-test: this build was wedged by its own harness"
		exit 1
	fi

	#
	# 2/2: prove the harness can still detect a wedge.
	#
	# A harness that has only ever passed proves nothing, and a passing
	# hostile test is exactly the kind that rots silently -- an attack
	# that stops reaching its target still passes. So the fix it was
	# written against is reverted here, deliberately, and the harness
	# must then FAIL. If it does not, the harness has stopped measuring
	# and its earlier pass means nothing, which is a build failure in
	# its own right rather than a curiosity.
	#
	# The revert is the single flag from #294: the console pty master
	# goes back to blocking, which is the state that held a real host
	# in n_tty_read for seventeen minutes.
	#
	echo "=== 2/2: reverting the #294 fix -- the harness must now FAIL ==="
	sed -i 's@O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);@O_RDWR | O_NOCTTY | O_CLOEXEC);@' \
		daemon/src/exec.c
	if ! grep -q 'O_RDWR | O_NOCTTY | O_CLOEXEC);' daemon/src/exec.c; then
		echo "cix-aggressive-test: could not revert the #294 fix -- exec.c has moved, and this"
		echo "                     self-check cannot vouch for the harness without it"
		exit 1
	fi
	make CIX_VERSION="$pkg_version" build/cixd
	if build/test_aggressive; then
		echo "cix-aggressive-test: the harness PASSED against a daemon with the #294 fix removed."
		echo "                     It is no longer measuring what it claims to, so its result"
		echo "                     above cannot be trusted either."
		exit 1
	fi
	echo "=== the harness detected the reverted fix, so its pass above is real ==="
}

pkg_install() {
	#
	# Nothing is installed. The product of this build is the verdict in
	# its log, and putting an attack harness onto a host would be
	# shipping a weapon to serve a job only a test host has.
	#
	mkdir -p "$PKG_DESTDIR"
}
