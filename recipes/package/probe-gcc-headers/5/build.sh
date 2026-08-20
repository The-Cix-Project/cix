#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# v5: v4 confirmed gcc's own include-fixed/limits.h content matches a
# real, working reference gcc (Debian 12.2.0's own
# /usr/lib/gcc/x86_64-linux-gnu/12/include/limits.h) structurally
# byte-for-byte at every grepped anchor -- the file itself is NOT
# broken/truncated the way originally assumed for the kernel's own
# PATH_MAX gap. The real chaining trick lives in a part of the file
# not yet inspected: right after `#define _GCC_LIMITS_H_`, a working
# reference does `#ifndef _LIBC_LIMITS_H_` -> `#include "syslimits.h"`
# BEFORE defining any of its own ISO limits, so glibc's real header
# gets visited (defining the real MB_LEN_MAX=16, PATH_MAX, etc.) while
# gcc's own placeholder MB_LEN_MAX=1 is still guarded by an #ifndef
# that glibc's prior visit has already satisfied. Confirming directly
# whether that #include "syslimits.h" line is present here, and what
# syslimits.h itself actually contains (kernel.recipe -8's own
# "replace with a bare passthrough" fix discarded ALL of this,
# accidentally losing UCHAR_MAX/INT_MAX/etc too -- confirmed via
# elfutils 0.192-4's own real build failure on exactly that gap).
#
pkg_name="probe-gcc-headers"
pkg_version="5"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

pkg_build() {
	echo "=== lines 27-40 of include-fixed/limits.h (the syslimits.h chain-in point) ==="
	sed -n '27,40p' /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h
	echo "=== full content of include-fixed/syslimits.h ==="
	cat /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/syslimits.h 2>&1
	echo "=== END syslimits.h ==="
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
