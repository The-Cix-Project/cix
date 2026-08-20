#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# v7: a NEW blocker past the syslimits.h fix -- tools/objtool's own
# final link (real gcc, since it links against our elfutils-provided
# libelf.so) fails with `/usr/lib/libz.so.1: undefined reference to
# '__va_start'`. zlib.recipe builds with CC=tcc (confirmed via
# `grep CC= recipes/package/zlib/*/build.sh`); TCC implements
# `va_start`/`va_arg` via real runtime calls to its own libtcc1.a
# helpers (`__va_start`/`__va_arg`) rather than compiler builtins the
# way GCC does, and zlib's own gzprintf() (a real varargs user) is the
# likely source. This dev sandbox's own local Debian-packaged tcc
# doesn't even show `__va_start` as a real libtcc1.a symbol at all
# (only `__va_arg` is real; `va_start` itself appears to get inlined) --
# but this dev sandbox's tcc is NOT necessarily the same tcc that built
# the real box's own libz.so.1 (this project's own self-hosted
# toolchain bootstrap, CLAUDE.md's own documented pivot). Confirming
# directly on the real box rather than extrapolating from a possibly
# unrepresentative local tcc: what's actually undefined in the real
# libz.so.1, and whether the real box's own libtcc1.a provides a
# `__va_start` symbol anywhere.
#
pkg_name="probe-gcc-headers"
pkg_version="7"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

pkg_build() {
	echo "=== undefined symbols in the real libz.so.1 ==="
	nm -D /usr/lib/libz.so.1 2>&1 | grep " U " | grep -i "va_"
	echo "=== does the real box's own libtcc1.a define __va_start anywhere? ==="
	TCC1=$(tcc -print-search-dirs 2>&1 | grep -A1 '^libtcc1:' | tail -1 | tr -d ' ')
	echo "libtcc1 path: $TCC1"
	nm "$TCC1" 2>&1 | grep -B2 "T __va_start\|T __va_arg\|__va_start\|__va_arg"
	echo "=== full list of undefined symbols libz.so.1 exposes ==="
	nm -D /usr/lib/libz.so.1 2>&1 | grep " U " | head -30
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
