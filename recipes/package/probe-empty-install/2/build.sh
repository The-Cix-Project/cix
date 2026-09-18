#
# v2: version 1 called find(1) in its install phase, purely to print
# what it had staged. findutils was not in pkg_build_depends, so the
# #302 missing-command gate refused the build and this probe proved
# that gate instead of its own. The line is gone rather than the
# dependency declared -- a probe that needs a tool to make its point
# is a probe testing two things.
#
# probe-empty-install -- does the daemon refuse a build that stages
# nothing? (#486)
#
# A build whose install phase produces no files used to be published
# as a package and reported `installed`. Two reached the fleet that
# way, both measured on 192.168.15.95 on 2026-09-18:
#
#   probe-pbs 1-1      an 87-byte artifact, pushed to the SHARED
#                      artifact cache. Every later install took it as
#                      a cache hit and never built, so one build that
#                      staged nothing became every host's copy.
#
#   probe-minisign 2   zero files, `installed`, while its recipe
#                      visibly stages usr/share/doc/probe-minisign/
#                      README -- the finalize phase deleted
#                      usr/share/doc (ADR-0251 clause 4, withdrawn by
#                      ADR-0306) and emptied the package after the
#                      build had finished correctly.
#
# The second is why the gate sits in the daemon rather than in the
# build: the tree was right when the build ended and empty by the time
# it was packaged.
#
# This is a probe rather than a case in test_pkg because test_pkg is
# not in the Makefile's SELFTESTS list -- that list is the measured
# set of tests a build container CAN run (probe-selftest-env/4: 35 of
# 86), and test_pkg needs to create real build environments, which a
# build container cannot (#224). Same reasoning, same convention, as
# probe-missing-tool for the #302 gate.
#
# The install phase deliberately creates DIRECTORIES and stages no
# file into them. That is the exact shape the gate has to catch and
# the one a naive check misses: the tree is not empty by any syscall's
# reckoning, and the artifact would still ship no content.
#
# EXPECTED RESULT: this install FAILS, with failure_kind "build" and
# an error naming #486. A SUCCESSFUL install means the gate is not
# working, and the package it published is empty.
#
pkg_name="probe-empty-install"
pkg_version="2"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.217.tar.gz"
pkg_sha256="d39fff31e193f72b8d82812915ad450ff11768a816c766cbc3da60fe4647e061"
pkg_depends=""
pkg_build_depends="bash coreutils"
pkg_changelog="2: version 1 failed for the wrong reason and proved a different gate -- its install phase called find(1) to report what it had staged, findutils was not declared, and the #302 missing-command gate refused the build before the #486 gate was ever reached. The report is gone; a probe should exercise one thing. 1: probes the #486 empty-package gate against a real daemon, because the regression test for it cannot run where the gate runs -- test_pkg is not in SELFTESTS (#224). Stages directories and no files, which is the shape a naive emptiness check misses. Expected to FAIL the install."

pkg_build() {
	echo "probe: nothing to build -- the question is what the daemon does with an empty tree"
	true
}

pkg_install() {
	# Directories only. No file anywhere.
	mkdir -p "$PKG_DESTDIR/usr/share/probe-empty-install"
	mkdir -p "$PKG_DESTDIR/usr/lib/probe-empty-install/nested"
	echo "probe: staged two directories and zero files"
}
