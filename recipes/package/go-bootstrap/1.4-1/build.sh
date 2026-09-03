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
pkg_version="1.4-1"
pkg_source="https://dl.google.com/go/go1.4-bootstrap-20171003.tar.gz"
pkg_sha256="f4ff5b5eb3a3cae1c993723f3eab519c5bae18866b5e5f96fe1102f0cb5c3e52"
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
pkg_changelog="1.4-1: first packaging. The C-written entry point into Go, so gitea and glauth -- the platform's chosen forge and LDAP -- can stop being recipes for software it cannot build (#261). Installs outside PATH under /usr/lib/go-bootstrap and is deliberately unusable for building packages: it exists to compile the next Go and nothing else."

pkg_build() {
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
	GOROOT_FINAL=/usr/lib/go-bootstrap \
	CGO_ENABLED=0 \
	GOOS=linux \
	GOARCH=amd64 \
	CC=/usr/bin/gcc \
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
	GOROOT="$PWD" ./bin/go build -o /run/gochk/hello /run/gochk/hello.go || {
		echo "go-bootstrap: the built compiler cannot compile a program" >&2
		exit 1
	}
	/run/gochk/hello
	if [ $? -ne 7 ]; then
		echo "go-bootstrap: the compiled program did not run correctly" >&2
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
