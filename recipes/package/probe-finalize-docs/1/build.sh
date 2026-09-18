#
# probe-finalize-docs -- does the finalize phase still delete
# documentation and locale trees? (ADR-0306)
#
# ADR-0251's clause 4 removed usr/share/{man,info,doc,locale,i18n}
# from every staged tree. ADR-0306 withdraws it. This probe is how
# that is proven against a real daemon rather than against a fixture.
#
# It is a probe rather than a real package rebuild because a real
# package cannot isolate the question. Measured on 2026-09-18, 57 of
# the 147 current recipes delete something under
# $PKG_DESTDIR/usr/share themselves -- xz removes the whole tree in
# one line -- so a rebuilt package that ships no documentation proves
# nothing about the daemon's policy, and one that does could have been
# built before the policy existed. This stages all five trees and
# nothing else, so the file list of the resulting artifact answers
# exactly one question.
#
# test_pkg_finalize gates the same rule, and gates it more thoroughly
# (it also checks the three rules that STAYED). It is not in the
# Makefile's SELFTESTS list, which is the measured set of tests a
# build container can execute -- so, exactly as probe-missing-tool
# records for the #302 gate, the assertion would otherwise ship having
# never run against a real daemon.
#
# EXPECTED RESULT: the install SUCCEEDS and the published file list
# contains all five staged files, COPYING included. Any of them
# missing means clause 4 is still live.
#
# The source is this platform's own tarball: a probe needs a real,
# checksum-verified fetch to reach pkg_install(), and this is the one
# archive certain to fetch -- the daemon holds the token, substitutes
# it into {{REPO_TOKEN}} host-side, and has pulled this exact tag
# already. probe-pbs uses it for the same reason.
#
pkg_name="probe-finalize-docs"
pkg_version="1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.217.tar.gz"
pkg_sha256="d39fff31e193f72b8d82812915ad450ff11768a816c766cbc3da60fe4647e061"
pkg_depends=""
pkg_build_depends="bash coreutils"
pkg_changelog="1: proves ADR-0306 against a real daemon -- the finalize phase must no longer delete usr/share/{man,info,doc,locale,i18n}. Stages one file in each of the five trees, COPYING among them, and nothing else, so the published file list answers exactly one question. Needed as a probe because 57 of 147 current recipes delete something under usr/share themselves, so no real package rebuild isolates the daemon's policy."

pkg_build() {
	echo "probe: nothing to build -- the question is what survives finalize"
	true
}

pkg_install() {
	# One file per tree clause 4 used to remove. The COPYING is the
	# one that matters: usr/share/doc/<package>/COPYING is where GNU
	# packages install their licence, and deleting the tree deleted
	# the licence with it.
	mkdir -p "$PKG_DESTDIR/usr/share/doc/probe-finalize-docs"
	echo "Permission is granted to anyone to use this software." \
		> "$PKG_DESTDIR/usr/share/doc/probe-finalize-docs/COPYING"

	mkdir -p "$PKG_DESTDIR/usr/share/man/man1"
	echo ".TH PROBE 1" > "$PKG_DESTDIR/usr/share/man/man1/probe.1"

	mkdir -p "$PKG_DESTDIR/usr/share/info"
	echo "probe info node" > "$PKG_DESTDIR/usr/share/info/probe.info"

	mkdir -p "$PKG_DESTDIR/usr/share/locale/en"
	echo "probe message catalogue" > "$PKG_DESTDIR/usr/share/locale/en/probe.mo"

	mkdir -p "$PKG_DESTDIR/usr/share/i18n/locales"
	echo "probe locale source" > "$PKG_DESTDIR/usr/share/i18n/locales/en_PROBE"

	# A control in a directory clause 4 never touched, so a file list
	# with only this one in it distinguishes "the prune ran" from
	# "the install staged nothing" -- which is a real failure mode
	# here (#486).
	mkdir -p "$PKG_DESTDIR/usr/share/probe-finalize-docs"
	echo "control" > "$PKG_DESTDIR/usr/share/probe-finalize-docs/CONTROL"
}
