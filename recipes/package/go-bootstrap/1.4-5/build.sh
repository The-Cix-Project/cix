#
# go-bootstrap -- Go 1.4, the last release written in C (#261).
#
# This exists for one reason: gitea and glauth cannot be rebuilt. They
# are the platform's chosen git forge and LDAP server (ADR-0109 picked
# glauth over lldap deliberately), they are Go with cgo, and Go is not
# packaged here. They were built in the accreted sandbox ADR-0199
# retired, where a Go toolchain existed ambiently and undeclared
# (#168) -- so two components this platform depends on are recipes for
# software it can no longer produce.
#
# Go cannot be built by a C compiler alone. That stopped at 1.4, which
# is why this stage exists and why it is pinned to 1.4 forever: it is
# the only entry point into the language that does not already require
# the language. Verified rather than assumed -- this tarball carries 55
# .c files under src/cmd/{6g,gc,dist}, which is the C toolchain doing
# the work.
#
# The source is Google's own bootstrap snapshot rather than the 1.4.3
# release. That snapshot exists precisely for this job: it carries
# fixes for building on systems much newer than 2014, which a real
# 1.4.3 tree does not.
#
# NOT a general-purpose Go. It installs outside PATH, under
# /usr/lib/go-bootstrap, and nothing should ever build a package with
# it -- it exists to compile the next Go and nothing else. A package
# built with a 2014 toolchain would be a provenance claim nobody wants
# to defend.
#
pkg_name="go-bootstrap"
pkg_version="1.4-5"
pkg_source="https://dl.google.com/go/go1.4-bootstrap-20171003.tar.gz"
pkg_sha256="f4ff5b5eb3a3cae1c993723f3eab519c5bae18866b5e5f96fe1102f0cb5c3e52"
pkg_artifact_sha256="500b87836b723b751d3102cf7dd8104ed6fc5fb89265625e9864cd9126f342c6"
pkg_depends=""
#
# ADR-0199/0209: composed from exactly these, with no fallback to
# inherit a missing tool from (#168).
#
# make.bash is a shell script that drives a C compiler and ar; there is
# no configure step and no make(1) involved in this stage at all.
#
pkg_build_depends="bash coreutils gcc binutils sed grep gawk"
pkg_toolchain="gcc"
pkg_toolchain_reason="Go 1.4's own C toolchain and make.bash assume a GCC-compatible driver; this is 2014 C that predates any expectation of another compiler, and it is bootstrap-only code that never ships in a package"
pkg_changelog="1.4-5: the gate killed a build that had already succeeded. 1.4-4 printed go version go1.4-bootstrap-20170531 linux/amd64, compiled a real program, ran it -- and the build then failed with exit status 7, which is that program's own deliberate exit code. Recipes run under set -e, so a command exiting non-zero aborts the script before the next line can inspect it; the check for the expected status never ran. The status is captured with a rc=0 then command or rc=$? idiom instead, which is the form set -e does not act on. Third revision in a row failing on the gate rather than on Go, which is worth recording: a check that cannot distinguish its own mechanics from the thing it checks is a check that costs build cycles. 1.4-4: the toolchain built; the gate was wrong. 1.4-3 got all the way to Installed commands in /build/src/bin and then failed its own check with go: cannot find GOROOT directory: /usr/lib/go-bootstrap -- because the check ran during pkg_build, before pkg_install has put the tree where GOROOT_FINAL says it will live. The binaries are correct: they were told where they will END UP, which is the whole point of GOROOT_FINAL. The check now points GOROOT at the build tree, which is where the toolchain actually is at that moment. 1.4-3: TMPDIR points at /run. The C bootstrap tool built fine once 1.4-2's wrapper landed, then died on mkdtemp(/var/tmp/go-cbuild-XXXXXX): No such file or directory -- this platform's minimal images have no /var/tmp, the same gap already recorded for /tmp. No patching needed: cmd/dist/unix.c reads TMPDIR and only falls back to /var/tmp when it is unset, so pointing it at /run -- a fresh tmpfs in every container -- is using the mechanism the source already offers rather than manufacturing the directory it expected. 1.4-2: builds against a 2026 compiler. gcc 16 defaults to C23, where bool is a keyword, so Go 1.4's own typedef int bool in cmd/dist/a.h is now illegal and every dist source failed on it -- plus a useless type name in empty declaration promoted to an error by their own -Werror. make.bash hardcodes -Wall -Werror and offers no CFLAGS hook, honouring only CC, so the fix goes in a thin compiler wrapper: the toolwrap pattern chrony and procps already use here. It appends -std=gnu99, which is what this code was written for, and -Wno-error, because 2014 C against a 2026 compiler will keep finding new warnings and none of them are defects worth failing a bootstrap over. 1.4-1: first packaging. The C-written entry point into Go, so gitea and glauth -- the platform's chosen forge and LDAP -- can stop being recipes for software it cannot build (#261). Installs outside PATH under /usr/lib/go-bootstrap and is deliberately unusable for building packages: it exists to compile the next Go and nothing else."


pkg_build() {
	#
	# A thin wrapper around gcc, because make.bash gives no other way in
	# (#261).
	#
	# Line 133 of make.bash is:
	#
	#   ${CC:-gcc} $mflag -O2 -Wall -Werror -o cmd/dist/dist ...
	#
	# -- hardcoded flags, no CFLAGS hook, and CC is the only thing it
	# honours. So the flags have to arrive through CC itself. Same
	# toolwrap pattern chrony and procps already use here.
	#
	# -std=gnu99 is the substantive fix: gcc 16 defaults to C23, where
	# bool is a KEYWORD, and this code opens with
	#
	#   typedef int bool;
	#
	# which C23 forbids outright. gnu99 is what this source was written
	# against, so this is restoring its own assumptions rather than
	# working around them.
	#
	# -Wno-error is the pragmatic half and worth being explicit about:
	# make.bash asks for -Werror, and 2014 C compiled by a 2026 compiler
	# will keep producing new diagnostics indefinitely. None of them are
	# defects in a compiler that exists only to build the next compiler,
	# and failing the bootstrap over a changed warning would make this
	# package break every time gcc gets stricter. Later flags win, so
	# appending is enough -- their -Werror is not removed, it is
	# overridden.
	#
	mkdir -p /run/toolwrap
	cat > /run/toolwrap/gcc <<'WRAP'
#!/usr/bin/bash
exec /usr/bin/gcc "$@" -std=gnu99 -Wno-error
WRAP
	chmod 0755 /run/toolwrap/gcc

	cd src

	#
	# GOROOT_FINAL is where the built toolchain will BELIEVE it lives,
	# and it has to match pkg_install below or every later stage will
	# look for its own runtime under the build container's scratch
	# path and fail somewhere far from the cause.
	#
	# CGO_ENABLED=0 deliberately: this compiler never builds anything
	# but the next Go, which needs no cgo, and enabling it would pull
	# a 2014 cgo implementation into a job that does not want one.
	#
	#
	# TMPDIR, because cmd/dist falls back to /var/tmp and this
	# platform's images do not have one (the same gap already recorded
	# for /tmp). unix.c reads TMPDIR first, so this uses the mechanism
	# the source already offers rather than manufacturing the directory
	# it expected. /run is a fresh tmpfs in every container.
	#
	mkdir -p /run/gotmp
	TMPDIR=/run/gotmp \
	GOROOT_FINAL=/usr/lib/go-bootstrap \
	CGO_ENABLED=0 \
	GOOS=linux \
	GOARCH=amd64 \
	CC=/run/toolwrap/gcc \
		bash ./make.bash

	cd ..

	#
	# Gate: a toolchain that built without producing a working compiler
	# is the failure mode that matters here, because the next stage
	# would fail with an error about ITS own source rather than about
	# this one. Asked to identify itself, and asked to actually compile
	# something -- version alone would pass on a binary that cannot
	# code-generate.
	#
	#
	# GOROOT points at the BUILD tree here, not at GOROOT_FINAL.
	#
	# The binaries were built believing they will live at
	# /usr/lib/go-bootstrap, which is correct and is what makes them
	# work once installed -- but pkg_install has not run yet, so at this
	# instant they are still in the build directory. Checking them
	# without saying so asks a compiler to find a runtime that has not
	# been put in place, and fails with an error about GOROOT that has
	# nothing to do with whether the build worked.
	#
	export GOROOT="$PWD"
	out=$(./bin/go version 2>&1) || {
		echo "go-bootstrap: the built compiler does not run: $out" >&2
		exit 1
	}
	case "$out" in
	*go1.4*) echo "  built: $out" ;;
	*)
		echo "go-bootstrap: unexpected version from the built compiler: $out" >&2
		exit 1
		;;
	esac

	mkdir -p /run/gochk
	cat > /run/gochk/hello.go <<'GO'
package main

import "os"

func main() { os.Exit(7) }
GO
	./bin/go build -o /run/gochk/hello /run/gochk/hello.go || {
		echo "go-bootstrap: the built compiler cannot compile a program" >&2
		exit 1
	}
	#
	# rc=0 then "|| rc=$?", not a bare call followed by $?.
	#
	# Recipes run under set -e, so a command exiting non-zero aborts
	# the script immediately -- and this program exits 7 ON PURPOSE, as
	# the thing being verified. Written the obvious way, the build dies
	# with the exact status that proves it succeeded, before the check
	# can say so. This form is the one set -e does not act on.
	#
	rc=0
	/run/gochk/hello || rc=$?
	if [ "$rc" -ne 7 ]; then
		echo "go-bootstrap: the compiled program exited $rc, expected 7 -- the toolchain "\
		     "builds but does not produce working binaries" >&2
		exit 1
	fi
	echo "  compiled and ran a real program -- this toolchain works"
}

pkg_install() {
	#
	# The whole tree, not just bin/: a Go toolchain needs its own
	# pkg/ and src/ at runtime, and GOROOT_FINAL above already told
	# the binaries to look here.
	#
	mkdir -p "$PKG_DESTDIR/usr/lib/go-bootstrap"
	cp -a bin pkg src lib VERSION "$PKG_DESTDIR/usr/lib/go-bootstrap/" 2>/dev/null || \
	cp -a bin pkg src VERSION "$PKG_DESTDIR/usr/lib/go-bootstrap/"

	#
	# Deliberately NOT symlinked into /usr/bin. Nothing should build a
	# package with a 2014 toolchain; the only legitimate consumer is
	# the next Go stage, which is told where to find this explicitly.
	#
	rm -rf "$PKG_DESTDIR/usr/lib/go-bootstrap/src/pkg/debug/dwarf/testdata" 2>/dev/null || true
}
