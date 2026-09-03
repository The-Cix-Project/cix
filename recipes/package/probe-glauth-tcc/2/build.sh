#
# probe-glauth-tcc -- does glauth still need gcc?
#
# glauth's recipe declares pkg_toolchain=gcc with a reason measured against
# TCC 0.9.27: cmd/cgo batches every C symbol probe into one translation unit
# and classifies the symbols from the resulting error list in a single pass,
# and TCC aborts a translation unit at the first error, so every name after
# the first in every probed package is misclassified (ADR-0170, #31).
#
# The compiler has changed since (ADR-0223: a pinned upstream snapshot, not
# the 2017 release), and ADR-0226 is explicit that a toolchain reason is a
# measurement with a date on it, to be re-measured when the compiler
# changes -- btrfs-progs is the worked example of a reason that had simply
# rotted. So this re-measures rather than assuming, and reports either way.
#
# Same prepared source as the real recipe, deliberately: a probe against a
# different tree would answer a different question. Nothing is installed
# and the real recipe is untouched -- the answer is the build log.
#
pkg_name="probe-glauth-tcc"
pkg_version="2"
pkg_source="http://192.168.15.31:8080/glauth-src-2.4.0-1-x86_64.tar.gz"
pkg_sha256="fb872485f894000a3f68a7065434322124f2173f21398b647207dbcd0d1a8c6d"
pkg_depends=""
pkg_build_depends="bash coreutils grep go tcc linux-headers"
pkg_changelog="2: declares linux-headers rather than libc-dev, which is not installed anywhere. 1: first probe. Re-measures glauth's gcc requirement against the current compiler, per ADR-0226's rule that a toolchain reason is a dated measurement rather than a verdict."

pkg_build() {
	export GOWORK="off"
	export GOFLAGS="-mod=vendor"
	export GOPROXY="off"
	export GOCACHE="/build/gocache"
	export GOTMPDIR="/build/gotmp"
	export CGO_ENABLED=1
	export CC=/usr/bin/tcc
	mkdir -p "$GOCACHE" "$GOTMPDIR"

	go version
	echo "=== building glauth with CC=/usr/bin/tcc ==="
	rc=0
	go build -tags embedsqlite -o glauth-tcc . 2>&1 | tail -40 || rc=$?
	if [ -f glauth-tcc ]; then
		echo "RESULT: TCC BUILT IT -- glauth no longer needs gcc, retire the exception"
	else
		echo "RESULT: TCC STILL CANNOT BUILD IT (go build rc=$rc) -- the gcc reason stands"
	fi
	echo "probe-glauth-tcc: failing on purpose so this reaches the log store"
	exit 1
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/doc/probe-glauth-tcc"
	echo "probe only" > "$PKG_DESTDIR/usr/share/doc/probe-glauth-tcc/README"
}
