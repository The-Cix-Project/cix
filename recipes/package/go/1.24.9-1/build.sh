#
# go -- the FINAL stage of the bootstrap (#261).
#
# The chain exists because Go cannot be built by a C compiler alone
# past 1.4, and gitea needs Go 1.24. Each link is the newest release
# its predecessor is allowed to build:
#
#   go-bootstrap 1.4  (C, gcc)      -> this, 1.19.13
#   1.19.13                         -> 1.20.x   (1.20 requires >= 1.17.13)
#   1.20.x                          -> 1.22.x   (1.22 requires >= 1.20.6)
#   1.22.x                          -> 1.24.x   (1.24 requires >= 1.22.6)
#
# 1.19.13 is chosen as the first hop because 1.19 is the last release
# Go 1.4 can bootstrap at all, so this takes the largest single step
# available and leaves the fewest links behind it.
#
# One package name across every stage, versions ascending, the same
# shape the gcc bootstrap here already used (TCC -> 4.7.4 -> 6.4.0 ->
# 16.2.0). Later stages declare `go` as their own build tool, which
# this platform supports deliberately (#166) -- a compiler bootstrapped
# by an earlier one of itself is the normal case, not a cycle.
#
# This stage declares go-bootstrap instead, because `go` does not exist
# yet when it runs. That asymmetry is the whole point of the chain.
#
pkg_name="go"
pkg_version="1.24.9-1"
pkg_source="https://dl.google.com/go/go1.24.9.src.tar.gz"
pkg_sha256="c72f81ba54fe00efe7f3e7499d400979246881b13b775e9a9bb85541c11be695"
pkg_depends=""
#
# ADR-0199/0209: composed from exactly these, no fallback (#168).
# make.bash is bash driving Go itself; binutils is here for the linker
# the toolchain shells out to, and gcc for the same reason even with
# cgo off -- the build links real executables.
#
pkg_build_depends="bash coreutils gcc binutils sed grep gawk go"
pkg_toolchain="gcc"
pkg_toolchain_reason="Go's own build links with the system C toolchain, and its cgo support is defined in terms of a GCC-compatible driver; TCC is not a substitute for a language toolchain's own linker expectations"
pkg_changelog="1.24.9-1: the final stage of the Go bootstrap (#261). gitea's own go.mod requires go 1.24, which is what this chain was built to reach: go-bootstrap 1.4 in C, then 1.19.13, 1.20.14, 1.22.12 and this, each the newest release its predecessor is permitted to build. CGO_ENABLED=1 here, unlike the intermediate stages: gitea and glauth are Go WITH cgo, linking a C sqlite amalgamation, so a toolchain that compiles pure Go and cannot do cgo would pass every other check and still fail the only two packages this exists for. The gate therefore compiles and runs a real cgo program as well as a pure one -- one file here rather than a discovery at the far end of a gitea build. 1.22.12-1: stage 4 of the Go bootstrap (#261), compiled by stage 3. 1.22 raises the bar again, to 1.20.6 or later, which is why 1.20.14 rather than any 1.20 was the previous hop. The bootstrap assertion is tightened to match: a 1.19 that satisfied the last stage would silently fail this one deep inside the build rather than at the top of the recipe. 1.20.14-1: stage 3 of the Go bootstrap (#261), compiled by stage 2. 1.20 is the first release that refuses a Go 1.4 bootstrap -- it requires 1.17.13 or later -- which is exactly why the chain needs a 1.19 hop rather than jumping straight here. From this stage on the chain is self-referential: go declares go as its own build tool, which this platform supports deliberately (#166), and the recipe asserts the bootstrap it was actually given is new enough rather than trusting the composer to have picked the right one. 1.19.13-1: stage 2 of the Go bootstrap (#261) -- the first Go built by a Go. 1.19 is the last release Go 1.4 can bootstrap, so this takes the largest first step available and leaves the fewest links behind it. Built with CGO_ENABLED=0 because an intermediate stage exists only to compile the next one and needs no C interop of its own; the final stage enables it, which is what gitea and glauth actually require. TMPDIR is pointed at /run for the same reason go-bootstrap needed it: this platform's images have neither /tmp nor /var/tmp."

pkg_build() {
	#
	# GOROOT_BOOTSTRAP is the whole mechanism: it names the EXISTING Go
	# that compiles this one. It points at go-bootstrap's install
	# prefix, which that recipe deliberately keeps off PATH so nothing
	# builds a package with a 2014 toolchain by accident.
	#
	#
	# Bootstrapped by the PREVIOUS go, not by go-bootstrap. From here
	# on the chain is self-referential: each stage declares `go` as its
	# own build tool and is compiled by whatever version is installed,
	# which this platform supports on purpose (#166).
	#
	prev=$(/usr/lib/go/bin/go version 2>&1) || {
		echo "go: /usr/lib/go/bin/go is missing or does not run -- the previous stage must be" >&2
		echo "    installed in this build environment before this one can run" >&2
		exit 1
	}
	echo "  bootstrapping with: $prev"
	case "$prev" in
	*go1.2[2-9]*) ;;
	*)
		echo "go: 1.24 requires a bootstrap of go1.22.6 or later, found: $prev" >&2
		exit 1
		;;
	esac

	#
	# Neither /tmp nor /var/tmp exists in this platform's images, and
	# Go's own tooling reaches for whichever the C library or its own
	# defaults name. /run is a fresh tmpfs in every container.
	#
	mkdir -p /run/gotmp

	cd src
	TMPDIR=/run/gotmp \
	GOROOT_BOOTSTRAP=/usr/lib/go \
	GOROOT_FINAL=/usr/lib/go \
	CGO_ENABLED=1 \
	GOOS=linux \
	GOARCH=amd64 \
		bash ./make.bash
	cd ..

	#
	# Gate: identify itself, then actually compile and run something.
	#
	# GOROOT points at the build tree, not GOROOT_FINAL -- the binaries
	# were told where they WILL live, and pkg_install has not put them
	# there yet. Getting this wrong cost a build cycle on stage 1.
	#
	export GOROOT="$PWD"
	out=$(./bin/go version 2>&1) || {
		echo "go: the built compiler does not run: $out" >&2
		exit 1
	}
	case "$out" in
	*go1.24.9*) echo "  built: $out" ;;
	*)
		echo "go: unexpected version from the built compiler: $out" >&2
		exit 1
		;;
	esac

	mkdir -p /run/gochk
	cat > /run/gochk/hello.go <<'GO'
package main

import "os"

func main() { os.Exit(7) }
GO
	TMPDIR=/run/gotmp HOME=/run ./bin/go build -o /run/gochk/hello /run/gochk/hello.go

	#
	# rc=0 then "|| rc=$?": recipes run under set -e, and this program
	# exits 7 on purpose. Written the obvious way the build dies with
	# the very status that proves it worked -- which it did, once.
	#
	rc=0
	/run/gochk/hello || rc=$?
	if [ "$rc" -ne 7 ]; then
		echo "go: the compiled program exited $rc, expected 7" >&2
		exit 1
	fi
	#
	# And a cgo program, which is the whole reason this chain exists.
	#
	# gitea and glauth are Go WITH cgo -- they link a C sqlite
	# amalgamation. A toolchain that compiles pure Go and cannot do cgo
	# would pass every check above and still fail the only two packages
	# this was built for, at the far end of a long build. Cheaper to
	# find out here, in one file.
	#
	cat > /run/gochk/cgohello.go <<'GO'
package main

/*
#include <stdlib.h>
static int seven(void) { return 7; }
*/
import "C"

import "os"

func main() { os.Exit(int(C.seven())) }
GO
	CGO_ENABLED=1 CC=/usr/bin/gcc TMPDIR=/run/gotmp HOME=/run \
		./bin/go build -o /run/gochk/cgohello /run/gochk/cgohello.go || {
		echo "go: cgo does not work -- gitea and glauth cannot be built by this toolchain" >&2
		exit 1
	}
	rc=0
	/run/gochk/cgohello || rc=$?
	if [ "$rc" -ne 7 ]; then
		echo "go: the cgo program exited $rc, expected 7" >&2
		exit 1
	fi
	echo "  compiled and ran a cgo program -- gitea and glauth have a toolchain again"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/lib/go" "$PKG_DESTDIR/usr/bin"
	cp -a bin pkg src lib api VERSION "$PKG_DESTDIR/usr/lib/go/" 2>/dev/null || \
	cp -a bin pkg src VERSION "$PKG_DESTDIR/usr/lib/go/"

	#
	# On PATH, unlike stage 1. This one is a real Go: later stages and
	# eventually gitea and glauth invoke it as `go`.
	#
	ln -sf ../lib/go/bin/go "$PKG_DESTDIR/usr/bin/go"
	ln -sf ../lib/go/bin/gofmt "$PKG_DESTDIR/usr/bin/gofmt"

	# Tests and their fixtures are a large fraction of the tree and
	# nothing here ever runs them.
	rm -rf "$PKG_DESTDIR/usr/lib/go/pkg/obj" 2>/dev/null || true
}
