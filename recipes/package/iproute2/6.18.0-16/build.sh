#
# iproute2 -- ip/ss/tc and the rest of the standard Linux networking
# toolset.
#
# Source verified against kernel.org's own published sha256sums.asc for
# this exact release.
#
# The 'ip vrf' command family was REMOVED in -5 and is RESTORED here.
#
# -5 through -13 shipped without 'ip vrf exec/identify/pids' and
# without ipnetns.c's vrf_reset() call, as a capability loss declared
# under ADR-0222: ip/ipvrf.c builds a BPF program from designated
# compound literals, and TCC miscompiled that construct (#211).
#
# tcc@0.9.27-12 fixes the compiler. ADR-0222 is explicit that such a
# removal is a consequence of a defect rather than a decision about
# what the package ought to contain, and is reverted when the defect
# is fixed -- so it is, here, and this recipe is once again plain
# upstream iproute2 with nothing cut out of it.
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
pkg_version="6.18.0-16"
pkg_source="https://www.kernel.org/pub/linux/utils/net/iproute2/iproute2-6.18.0.tar.xz"
pkg_sha256="6ba520e1975e4c50dc931eeae91ea37c198b8a173744885f8895b84325f9d456"
pkg_artifact_sha256="8b9fc8206c3f3abf6c2cfd41637f8a54ce160f944d47386198c958398b11e2de"
pkg_depends="libmnl elfutils libcap zlib"
#
# zlib is here because libelf's compressed-debug-info support pulls it
# in transitively, which the 6.18.0 copy list had already discovered the
# hard way.
#
# bison and flex are for tc/, not ip/: tc generates its extended-match
# parser (emp_ematch.tab.c / emp_ematch.lex.c) from a real grammar. The
# ip half of this package builds without them, which is why they only
# surfaced once ip had linked -- a reminder that a build reaching its
# last directory has not proven its tool list, only the part of it the
# earlier directories needed.
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
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf bison flex libmnl elfutils libcap zlib"
pkg_changelog="6.18.0-16: stops stripping -D_FILE_OFFSET_BITS=64. That strip was the #210 workaround for tcc 0.9.27 being unable to parse glob.h's __REDIRECT_NTHNL declaration; the upgraded compiler parses it, verified with the issue's own reproducers. The assertion is inverted to match: the define must now be present. 6.18.0-15: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 6.18.0-14: RESTORES the 'ip vrf' command family. It was removed in -5 as a declared capability loss because TCC miscompiled ipvrf.c (#211); tcc@0.9.27-12 fixes that, and ADR-0222 requires such a removal to be reverted once its defect is fixed. 6.18.0-13: the -12 capability check read the BINARY's symbol table and reported both symbols absent, which is what a stripped binary looks like -- so the 'removed' half passed vacuously and the 'retained' half failed. It now checks the build inputs, which are deterministic, and reports the symbol-table size rather than trusting it. 6.18.0-12: the -11 build SUCCEEDED and its assertions failed -- they looked for the binaries in usr/sbin, but iproute2's SBINDIR defaults to /sbin, which is a real location on this platform. Assertions now locate the binary instead of assuming a path. 6.18.0-11: pins HOSTCC=tcc. netem/ builds host tools to generate its distribution tables, and HOSTCC came from the build ENVIRONMENT as gcc -- config.mk sets only CC:=tcc. It failed loudly only because gcc is not in this recipe's composed environment. 6.18.0-10: -9's changelog contained backticks inside a double-quoted string, so the shell ran 'bison:' as a command. Second time in this recipe; metadata strings take no backticks. 6.18.0-9: declares bison and flex -- tc/ generates its ematch parser with them, and bison was absent (a plain 'command not found'). 6.18.0-8: removes ipnetns.c's vrf_reset() call, which -7 left calling into the file it had deleted; and maps strdupa to strdup, since glibc gates strdupa on __GNUC__ which TCC does not define. 6.18.0-7: builds with SHARED_LIBS=n, which drops the -Wl,-export-dynamic that TCC rejects. Not a capability loss: this configuration compiles the tc/ip modules IN rather than dlopening them, and this build produced no .so modules at all. 6.18.0-6: -5's changelog used backticks, then double quotes, inside a double-quoted string -- both made the shell try to run 'ip'. DECLARED CAPABILITY LOSS -- the 'ip vrf' command family (exec/identify/pids) is removed because it cannot be compiled by this platform's TCC (#211). 'ip link ... type vrf' is UNAFFECTED and still works. 6.18.0-4: drops -D_FILE_OFFSET_BITS=64, which steers glibc's glob.h onto a __REDIRECT_NTHNL declaration TCC cannot parse. The define is a no-op on x86-64, where off_t is already 64-bit. 6.18.0-3: reads config.mk, which is what configure actually writes (the -2 assertion looked for a file named Config and died on cat). Needs elfutils@0.192-11, the first revision to ship libelf.pc. 6.18.0-2: declares libmnl/elfutils/libcap/zlib instead of copying the build host's Debian copies into the package, and asserts configure actually enabled each of them rather than silently degrading (#206, #207, #113)"

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
	# -D_FILE_OFFSET_BITS=64 is LEFT ALONE now, and that is the point
	# of this revision.
	#
	# Revisions -4 through -15 stripped it from the Makefile, because
	# it steers glibc's headers onto their __USE_FILE_OFFSET64 branch,
	# where glob.h declares glob() with __REDIRECT_NTHNL -- which
	# tcc 0.9.27 could not parse:
	#
	#     /usr/include/glob.h:163: error: identifier expected
	#
	# That was issue #210, and the strip was a workaround, safe only
	# because this platform is LP64 where the define is a no-op.
	#
	# The upgraded compiler (0.9.28rc, ADR-0223) parses it. Measured
	# with the issue's own reproducers rather than assumed
	# (probe-tcc-conformance/14): __REDIRECT_NTHNL compiles, and
	# <glob.h>, <ftw.h>, <stdio.h> and <dirent.h> all compile clean
	# under -D_FILE_OFFSET_BITS=64.
	#
	# So the workaround is removed rather than carried forward. A
	# workaround kept after its cause is gone becomes folklore: the
	# next person reads it as a live constraint.
	#
	# Asserted in the DIRECTION THAT CAN NOW FAIL: the define must
	# still be there. If some future change strips it again, or
	# upstream drops it, this build stops rather than quietly
	# reverting to the old behaviour.
	#
	grep -q '\-D_FILE_OFFSET_BITS=64' Makefile || {
		echo "iproute2: expected -D_FILE_OFFSET_BITS=64 in the Makefile and it is not there" >&2
		exit 1
	}

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
	#
	# HOSTCC=tcc is pinned deliberately, and the reason is a provenance
	# hazard rather than a build failure.
	#
	# netem/ compiles four small host tools (maketable, normal, pareto,
	# paretonormal) that generate the .dist tables `tc qdisc ... netem
	# distribution` uses. Both the top-level and netem Makefiles say
	# `HOSTCC ?= $(CC)`, and config.mk sets `CC:=tcc` and no HOSTCC --
	# yet the build invoked a literal `gcc`, so HOSTCC arrives from the
	# BUILD ENVIRONMENT.
	#
	# Here that surfaced as an honest failure (`gcc: No such file or
	# directory`) purely because gcc is not among this recipe's declared
	# tools, so ADR-0199's composed environment does not contain one.
	# In an environment that happens to have gcc, the same variable
	# would have silently built part of this package with a compiler the
	# recipe never asked for. Pinning it on the make command line -- which
	# beats both the environment and the makefile -- makes the answer a
	# property of this recipe.
	#
	# All four tools were confirmed to compile clean under tcc before
	# pinning, so this is not a trade: nothing is lost.
	#
	make SHARED_LIBS=n HOSTCC=tcc -j"$(nproc)"

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

	#
	# Locate the binaries rather than assume a path. iproute2's
	# SBINDIR defaults to /sbin, not /usr/sbin, and both are real
	# locations on this platform -- the -11 build installed perfectly
	# and then failed its own assertion on that assumption.
	#
	ip_bin=""
	for cand in "$PKG_DESTDIR/sbin/ip" "$PKG_DESTDIR/usr/sbin/ip"; do
		if [ -x "$cand" ]; then
			ip_bin="$cand"
			break
		fi
	done
	if [ -z "$ip_bin" ]; then
		echo "ip binary not found in sbin or usr/sbin" >&2
		exit 1
	fi
	echo "ip installed at: ${ip_bin#$PKG_DESTDIR}"

	for b in tc ss; do
		if [ ! -x "$PKG_DESTDIR/sbin/$b" ] && [ ! -x "$PKG_DESTDIR/usr/sbin/$b" ]; then
			echo "missing binary $b" >&2
			exit 1
		fi
	done

	#
	# 'ip vrf' must be present again. Asserted against the build inputs
	# rather than the binary's symbol table: the installed binary is
	# stripped (nm reports 0 entries), which is how the -12 check
	# managed to report both a removed and a retained symbol missing.
	#
	grep -q 'ipvrf\.o' ip/Makefile || {
		echo "ipvrf.o is not in IPOBJ -- ip vrf was not restored" >&2
		exit 1
	}
	grep -q 'do_ipvrf' ip/ip.c || {
		echo "the vrf command is not registered in ip.c" >&2
		exit 1
	}
	grep -q 'iplink_vrf\.o' ip/Makefile || {
		echo "iplink_vrf.o is not in IPOBJ -- ip link type vrf lost" >&2
		exit 1
	}
	echo "capability check: ip vrf restored (#211 fixed in tcc@0.9.27-12)"

	echo "=== ip DT_NEEDED ==="
	readelf -d "$ip_bin" | grep NEEDED

	readelf -d "$ip_bin" | grep -q 'NEEDED.*libmnl\.so\.0' || {
		echo "ip is not linked against libmnl.so.0" >&2
		exit 1
	}
	readelf -d "$ip_bin" | grep -q 'NEEDED.*libelf\.so\.1' || {
		echo "ip is not linked against libelf.so.1" >&2
		exit 1
	}
}
