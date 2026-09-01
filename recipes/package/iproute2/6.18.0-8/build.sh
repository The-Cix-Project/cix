#
# iproute2 -- ip/ss/tc and the rest of the standard Linux networking
# toolset.
#
# Source verified against kernel.org's own published sha256sums.asc for
# this exact release.
#
# ============================================================
# DECLARED CAPABILITY LOSS (-5): the `ip vrf` command is GONE
# ============================================================
#
# REMOVED:  ip vrf exec / ip vrf identify / ip vrf pids
#           -- running a command bound to a VRF, and querying which
#           VRF a process or socket is associated with.
#
# KEPT:     ip link add ... type vrf, and every other VRF-related
#           thing `ip link` does. That lives in ip/iplink_vrf.c, a
#           different file with no dependency on the removed one, so
#           CREATING and inspecting VRF devices is unaffected. What is
#           lost is only the process/socket-binding half.
#
# ALSO GONE:  the vrf_reset() call ip/ipnetns.c makes when entering a
#           network namespace. It lives in the removed file, and it
#           detaches the process from a VRF cgroup before the switch.
#           With 'ip vrf' gone there is no way for this binary to have
#           bound one in the first place, so the reset has nothing to
#           undo -- but it is stated here rather than left as an
#           implementation detail, because it is a second behaviour
#           change and not merely a link fix.
#
# WHY:      ip/ipvrf.c builds a BPF program from an array of
#           designated compound literals, which this platform's TCC
#           miscompiles -- it uses a struct field's byte offset as an
#           array index and errors "index too large" (#211,
#           tccgen.c:6389). It is a real TCC bug with a 12-line
#           reduction, not anything wrong with iproute2.
#
# This is a deliberate, approved trade: shipping a from-source,
# provenance-clean iproute2 that is missing one command, rather than
# either shipping nothing or reaching for a non-TCC compiler. When
# #211 is fixed, this removal should be reverted -- it is a
# consequence of a compiler bug, not a decision about what iproute2
# ought to contain.
#
# The removal is asserted below in both directions: `do_ipvrf` must be
# absent from the binary, and `vrf_link_util` must still be present.
# A capability loss that is not verified is just a hope, and a
# half-removed command that still appears in `ip help` would be worse
# than either outcome.
#
# WHAT CHANGED IN -2:
#
# 6.18.0 copied FOUR of the build host's Debian libraries into the
# package -- libelf-0.188, libmnl, libcap and libz. All four now exist
# as real Cix packages (elfutils, libmnl, libcap, zlib), so they are
# declared instead of copied. The version numbers in the old copy list
# name Debian's builds outright (libelf-0.188.so, libz.so.1.2.13,
# libcap.so.2.66); Cix's own are 0.192, 1.3.2 and 2.78.
#
# The more important change is the assertion at the end. iproute2's
# ./configure is a plain shell script that PROBES for each optional
# dependency and degrades silently when one is missing -- it does not
# fail, it just writes a smaller Config and builds a reduced `ip`. That
# is exactly the #113 shape, and it is how this package could appear to
# build perfectly while quietly losing BPF program loading or netlink
# support. So the recipe now reads configure's own Config file back and
# fails if a feature it declared a dependency for did not actually turn
# on.
#
pkg_name="iproute2"
pkg_version="6.18.0-8"
pkg_source="https://www.kernel.org/pub/linux/utils/net/iproute2/iproute2-6.18.0.tar.xz"
pkg_sha256="6ba520e1975e4c50dc931eeae91ea37c198b8a173744885f8895b84325f9d456"
pkg_depends="libmnl elfutils libcap zlib"
#
# zlib is here because libelf's compressed-debug-info support pulls it
# in transitively, which the 6.18.0 copy list had already discovered the
# hard way.
#
# Deliberately NOT declared, though both are now available: iptables
# (xtables) and ipset. configure probes for them too and would enable
# tc's xtables action and ipset matching -- real features 6.18.0 never
# had. Adding them is a capability change that should be its own
# decision with its own verification, not a side effect of a
# provenance fix. Under a composed build environment (ADR-0199) the
# sandbox contains only what is declared here, so leaving them out is
# deterministic rather than accidental.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf libmnl elfutils libcap zlib"
pkg_changelog="6.18.0-8: removes ipnetns.c's vrf_reset() call, which -7 left calling into the file it had deleted; and maps strdupa to strdup, since glibc gates strdupa on __GNUC__ which TCC does not define. 6.18.0-7: builds with SHARED_LIBS=n, which drops the -Wl,-export-dynamic that TCC rejects. Not a capability loss: this configuration compiles the tc/ip modules IN rather than dlopening them, and this build produced no .so modules at all. 6.18.0-6: -5's changelog used backticks, then double quotes, inside a double-quoted string -- both made the shell try to run 'ip'. DECLARED CAPABILITY LOSS -- the 'ip vrf' command family (exec/identify/pids) is removed because it cannot be compiled by this platform's TCC (#211). 'ip link ... type vrf' is UNAFFECTED and still works. 6.18.0-4: drops -D_FILE_OFFSET_BITS=64, which steers glibc's glob.h onto a __REDIRECT_NTHNL declaration TCC cannot parse. The define is a no-op on x86-64, where off_t is already 64-bit. 6.18.0-3: reads config.mk, which is what configure actually writes (the -2 assertion looked for a file named Config and died on cat). Needs elfutils@0.192-11, the first revision to ship libelf.pc. 6.18.0-2: declares libmnl/elfutils/libcap/zlib instead of copying the build host's Debian copies into the package, and asserts configure actually enabled each of them rather than silently degrading (#206, #207, #113)"

pkg_build() {
	export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"

	#
	# glibc declares strdupa only under `#if defined __USE_GNU && defined
	# __GNUC__` (string.h). TCC does not define __GNUC__ -- correctly, it
	# is not GCC -- so the macro does not exist, the call compiles as an
	# implicit declaration, and the link fails with `undefined symbol
	# 'strdupa'`. Five call sites across ip/ and lib/.
	#
	# Mapped to strdup. The difference is real and worth stating:
	# strdupa allocates on the STACK and is freed when the calling
	# function returns; strdup allocates on the HEAP and here is never
	# freed. That direction is safe -- the pointer stays valid longer,
	# never shorter, so it cannot dangle -- and the cost is a small
	# per-call leak bounded by the process lifetime. These are
	# short-lived CLI invocations, and the strings are argument
	# fragments and route specs.
	#
	# NOT solved by defining __GNUC__: that would steer every glibc
	# header onto its GCC branches, which is a far larger surface than
	# one missing macro.
	#
	export CPPFLAGS="-Dstrdupa=strdup"

	echo "=== libraries visible to pkg-config ==="
	for m in libmnl libelf zlib libcap; do
		if pkg-config --exists "$m" 2>/dev/null; then
			echo "  present  $m $(pkg-config --modversion "$m")"
		else
			echo "  ABSENT   $m"
		fi
	done

	#
	# iproute2's Makefile line 64 unconditionally adds:
	#
	#     DEFINES += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE \
	#                -D_LARGEFILE64_SOURCE
	#
	# _FILE_OFFSET_BITS=64 steers glibc's headers onto their
	# __USE_FILE_OFFSET64 branch. For glob.h that branch declares glob()
	# with __REDIRECT_NTHNL, which TCC cannot parse:
	#
	#     /usr/include/glob.h:163: error: identifier expected
	#
	# Confirmed by direct probe, not inferred: plain __REDIRECT and
	# __REDIRECT_NTH both parse fine under TCC and only __REDIRECT_NTHNL
	# fails, and of the common libc headers only glob.h takes that
	# branch (stdio.h, fcntl.h, unistd.h, sys/stat.h, dirent.h, ftw.h,
	# stdlib.h, sys/sendfile.h and aio.h all compile clean with the same
	# define).
	#
	# Removing the define is safe HERE specifically because this
	# platform is x86-64, where off_t is already 64 bits: sizeof(off_t)
	# is 8 with and without it, verified by compiling both ways. It is a
	# no-op on LP64 and exists for 32-bit targets. The two LARGEFILE
	# defines are left alone -- they only expose the *64 names, they do
	# not redirect, and they are not what breaks.
	#
	# Edited in the Makefile rather than overridden on the command line
	# because DEFINES accumulates with += from several places, so
	# assigning it on the command line would silently drop the others.
	#
	sed -i 's/-D_FILE_OFFSET_BITS=64//' Makefile
	if grep -q '_FILE_OFFSET_BITS=64' Makefile; then
		echo "-D_FILE_OFFSET_BITS=64 survived the strip" >&2
		exit 1
	fi

	#
	# Remove `ip vrf` (see the header). Three edits, each minimal:
	#   1. drop ipvrf.o from the link
	#   2. drop the command-table entry that would leave do_ipvrf
	#      undefined at link
	#   3. drop it from the usage string, so `ip help` does not offer a
	#      command that no longer exists
	#
	sed -i 's/ ipvrf\.o//' ip/Makefile
	sed -i '/{ "vrf",[[:space:]]*do_ipvrf}/d' ip/ip.c
	sed -i 's/ vrf |//' ip/ip.c

	# ipnetns.c calls vrf_reset(), which lives in the removed file --
	# -7 linked with it still referenced and failed on `undefined
	# symbol 'vrf_reset'`. See the header for what dropping the call
	# means; it is part of the declared loss, not a link workaround.
	sed -i '/^[[:space:]]*vrf_reset();[[:space:]]*$/d' ip/ipnetns.c
	sed -i '/^void vrf_reset(void);$/d' ip/ip_common.h

	for check in 'ip/Makefile:ipvrf\.o' 'ip/ip.c:do_ipvrf' 'ip/ip.c:vrf |' 'ip/ipnetns.c:vrf_reset'; do
		f=${check%%:*}; pat=${check#*:}
		if grep -q "$pat" "$f"; then
			echo "ip vrf removal incomplete: '$pat' still in $f" >&2
			exit 1
		fi
	done

	CC=tcc ./configure

	#
	# configure records what it found in Config. Read it back rather
	# than trusting that configure exited 0 -- it exits 0 either way.
	#
	# configure writes config.mk (CONFIG=config.mk in its own source),
	# not "Config" -- the -2 revision asserted against the wrong
	# filename and died on `cat` after an otherwise fine configure.
	echo "=== config.mk as configure wrote it ==="
	cat config.mk

	for feat in HAVE_MNL HAVE_ELF HAVE_CAP; do
		grep -q "^$feat:=y" config.mk || {
			echo "$feat did not get enabled -- configure degraded silently" >&2
			exit 1
		}
	done

	#
	# SHARED_LIBS=n, for a linker flag TCC does not implement:
	#
	#     tcc: error: unsupported linker option '-export-dynamic'
	#
	# ip/, genl/ and tc/ each add -Wl,-export-dynamic, and all three
	# add it INSIDE `ifeq ($(SHARED_LIBS),y)`. That flag is genuinely
	# load-bearing when it is used: it exports the binary's symbols so
	# dlopen'd modules can resolve back into it, and tc/ip really do
	# dlopen modules. Dropping the flag on its own would produce a
	# binary that links fine and fails at run time when a module is
	# loaded -- a silent failure, which is the outcome this recipe set
	# spends most of its effort avoiding.
	#
	# SHARED_LIBS=n is upstream's own supported alternative: the
	# modules are compiled INTO the binaries instead, ip generates a
	# static-syms table so its dlsym lookups resolve statically, and
	# neither -export-dynamic nor -ldl is needed.
	#
	# This costs nothing HERE, which is checked rather than assumed:
	# tc's TCSO list starts empty and only gains .so entries for the
	# xtables modules, which need TC_CONFIG_XT=y -- this recipe does
	# not declare iptables, so that is off. The -6 build confirmed it
	# directly: zero .so files were produced anywhere in the tree, so
	# there were no dynamically loaded modules to lose.
	#
	# Same class as libcap's -Wl,-x and -Wl,-e (see that recipe): real
	# gaps in TCC 0.9.27's linker option handling, not anything wrong
	# with the package.
	#
	make SHARED_LIBS=n -j"$(nproc)"

	# The modules must have been built in, not dropped. If TCSO were
	# ever non-empty in some future configuration, this would catch the
	# silent loss that SHARED_LIBS=n would then cause.
	if find . -name '*.so' | grep -q .; then
		echo "shared modules were built despite SHARED_LIBS=n -- re-check the trade" >&2
		exit 1
	fi
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	# No library copying: all four arrive via pkg_depends.

	echo "=== ip DT_NEEDED ==="
	readelf -d "$PKG_DESTDIR/usr/sbin/ip" | grep NEEDED

	# The binaries that justify the package existing.
	for b in ip tc ss; do
		test -x "$PKG_DESTDIR/usr/sbin/$b" || test -x "$PKG_DESTDIR/usr/bin/$b" || {
			echo "missing binary $b" >&2
			exit 1
		}
	done

	#
	# Verify the declared capability loss in BOTH directions, because
	# either half being wrong is worse than the loss itself.
	#
	if nm -a "$PKG_DESTDIR/usr/sbin/ip" 2>/dev/null | grep -q 'do_ipvrf'; then
		echo "do_ipvrf is still in the binary -- the removal did not take" >&2
		exit 1
	fi
	nm -a "$PKG_DESTDIR/usr/sbin/ip" 2>/dev/null | grep -q 'vrf_link_util' || {
		echo "vrf_link_util is gone -- ip link type vrf was lost too, which was not intended" >&2
		exit 1
	}
	echo "capability check: ip vrf removed (#211), ip link type vrf retained"

	# ip must really be dynamically linked against the netlink and ELF
	# libraries this recipe declared, not have quietly dropped them.
	readelf -d "$PKG_DESTDIR/usr/sbin/ip" | grep -q 'NEEDED.*libmnl\.so\.0' || {
		echo "ip is not linked against libmnl.so.0" >&2
		exit 1
	}
	readelf -d "$PKG_DESTDIR/usr/sbin/ip" | grep -q 'NEEDED.*libelf\.so\.1' || {
		echo "ip is not linked against libelf.so.1" >&2
		exit 1
	}
}
