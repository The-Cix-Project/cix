#!/usr/bin/bash
#
# ADR-0251 -- a package artifact carries what the platform runs, and
# nothing else.
#
# EXECUTED, not sourced, and it takes the staged root as $1 (ADR-0307
# clause 2). It was sourced until then, reading $PKG_DESTDIR out of the
# environment. The change is not cosmetic: CBS runs an embedder's
# finalizer as `execlp(command, command, staged_root, NULL)`
# (cix-build-system src/main.c:514), so a PBS build can only reach this
# policy as a program taking one argument -- and ADR-0251's rules have
# to stay ONE definition across both recipe languages rather than
# growing a second expression for the PBS side.
#
# #!/usr/bin/bash and not /bin/bash: this project's own images ship
# bin/sh and usr/bin/bash and no /bin/bash at all, and a bad
# interpreter reports as exit 127 against a file that visibly exists.
#
# Written into the build container by cixd (daemon/src/pkg.c), never by
# a recipe. That is the whole point: before this existed, the scope of
# these rules was re-guessed once per recipe, and the result was that
# 37 of 115 recipes pruned anything at all, in twelve different
# spellings, while glibc shipped 9.46 MiB of debug sections in
# libc.so.6 and libc.a three times over.
#
# Run after the install phase, in the build container, against the tree
# that phase staged. The scan uses bash builtins and coreutils (rm, wc,
# od) and no find or file, so it adds no package the build image lacks;
# strip is required only when the package actually produced ELF.
#
set -e

cix_finalize() {
	local dest="$1"
	local elfmagic armagic f base stem magic arsize t compiler_runtimes
	local shared_stems=""
	local -a elf_dyn=() elf_rel=() archives=()

	# An empty argument is a CALLER bug, not an empty package, and the
	# two must not look alike: `test -d ""` is false, so the check
	# below would return 0 for it -- finalizing nothing and reporting
	# success, which is the exact shape ADR-0251 exists to stop (37 of
	# 115 recipes pruning, nobody counting).
	if test -z "$dest"; then
		echo "cix: package finalize was given no staged root" >&2
		return 1
	fi

	if ! test -d "$dest"; then
		return 0
	fi

	# ADR-0250: name every tool this phase needs, and fail on absence
	# rather than skipping. rm and wc are what the prune needs, and od
	# reads an ELF header's type -- all three from coreutils, so this
	# asks for no package the prune did not already require; strip is
	# checked later, and only if ELF was produced.
	for t in rm wc od; do
		if ! command -v "$t" >/dev/null 2>&1; then
			echo "cix: package finalize needs $t and the build image has none." >&2
			echo "cix: add coreutils to pkg_build_depends (ADR-0199, ADR-0251)." >&2
			return 1
		fi
	done

	shopt -s nullglob dotglob globstar

	# ADR-0251's clause 4 -- removing usr/share/{man,info,doc,locale,i18n}
	# -- is GONE. See ADR-0306. It is not commented out or made
	# conditional: a package's staged tree reaches the artifact as the
	# build left it, minus the three rules below, which remove things
	# the platform cannot use rather than things nobody reads.
	#
	# The deletion recovered a share of the artifact that was never
	# separately measured: ADR-0251's own table accounts for static
	# archives (54%), debug sections (26%) and locale SOURCES (10%),
	# leaving man/info/doc inside a 10% remainder. Measured against that:
	# it deleted usr/share/doc, where GNU packages install COPYING, so
	# every GPL artifact this platform published lost its licence text.
	# Across 171 installed package entries on 192.168.15.95 on
	# 2026-09-18, 25 licence files survived, every one of them by living
	# somewhere other than usr/share/doc.

	elfmagic=$(printf '\177ELF')
	armagic='!<ar'

	for f in "$dest"/**/*; do
		if test -L "$f"; then
			continue
		fi
		if ! test -f "$f"; then
			continue
		fi

		# Clause 3: libtool metadata describes how to link something
		# that is no longer being linked.
		case "$f" in
		*.la)
			rm -f "$f"
			continue
			;;
		esac

		magic=""
		LC_ALL=C IFS= read -r -n 4 magic < "$f" 2>/dev/null || true

		if test "$magic" = "$armagic"; then
			#
			# An archive with NO MEMBERS is exactly eight bytes --
			# the magic and nothing else -- and clause 2 below must
			# not touch it. That clause drops an archive because it
			# duplicates a shared object shipping beside it; an
			# empty archive duplicates nothing, and for glibc's
			# folded-in stubs it IS the link-time contract.
			#
			# glibc >= 2.34 folds pthread_create, and the rt and dl
			# entry points, into libc.so.6. What it still installs
			# is libpthread.a / librt.a / libdl.a at eight bytes
			# each, and libpthread.so.0 as a runtime stub -- and no
			# libpthread.so at all. So `ld -lpthread`, which every
			# gcc driver's -pthread expands to, has exactly one
			# thing it can resolve against, and clause 2 was
			# deleting it on a stem match with the runtime stub.
			# Every gcc-toolchain package passing -pthread then
			# failed to link (#324, found on btop).
			#
			# wc, and not a read builtin, because the shell cannot
			# measure binary at all: a NUL byte cannot be held in a
			# variable, and `read` stops at one regardless of -N or
			# -d. Both were tried against a NUL-padded archive and
			# both reported eight bytes for a 64-byte file, which
			# would have kept every archive on the system.
			#
			arsize=$(wc -c < "$f")
			if test "$arsize" -le 8; then
				continue
			fi
			archives+=("$f")
			continue
		fi
		if test "$magic" != "$elfmagic"; then
			continue
		fi

		case "$f" in
		*.so|*.so.*)
			base="${f##*/}"
			stem="${base%%.so*}"
			case "$shared_stems" in
			*"|$stem|"*) ;;
			*) shared_stems="$shared_stems|$stem|" ;;
			esac
			;;
		esac

		# Which strip is decided by what the file IS -- e_type, the
		# two bytes at offset 16 -- and never by its name. 1 is ET_REL,
		# a relocatable object, whose .symtab the linker needs. This
		# used to match *.o and *.ko, so any relocatable object named
		# otherwise got --strip-unneeded: Go ships its race runtime as
		# race/internal/amd64v1/race_linux.syso, and go@1.24.9-3's
		# stripped copy made every `go build -race` fail to link with
		# "hole in findfunctab" (measured on 192.168.15.95,
		# 2026-09-24, probe-go-race@1). od reads host byte order, and
		# every ELF this platform builds is little-endian x86-64, as
		# the host is.
		t=$(LC_ALL=C od -An -t u2 -j 16 -N 2 "$f" 2>/dev/null) || t=""
		if test "${t// /}" = 1; then
			elf_rel+=("$f")
		else
			elf_dyn+=("$f")
		fi
	done

	# Clause 2: an archive whose shared counterpart ships beside it is
	# dead weight on a platform that links dynamically always. An
	# archive with no shared counterpart -- libtcc1.a, libgcc.a,
	# libgcc_eh.a, libc_nonshared.a -- is kept, without being named.
	# So is one with no members, which never reaches this list at all
	# (see the scan above): it duplicates nothing, so the reason this
	# clause exists does not apply to it.
	#
	# A kept archive is left exactly as it is, deliberately. "ar
	# archive" does not imply "archive of ELF": go-bootstrap ships
	# Go 1.4's pkg/linux_amd64/*.a, which carry !<arch> magic over Go
	# object files, and strip rejects those outright. Stripping them
	# would fail that build for no gain -- the archives this clause
	# keeps are small, and the debug weight this policy exists to
	# remove is in shared objects and executables.
	#
	# EXCEPT a compiler runtime, which is not a duplicate of its shared
	# counterpart but the other half of a linking mode the driver
	# offers (ADR-0310, #521). `gcc -static-libstdc++` looks for
	# libstdc++.a specifically; dropping it does not make a package
	# smaller, it makes that flag impossible, and the flag exists so a
	# binary can carry the compiler's runtime and need only glibc
	# wherever it runs.
	#
	# The asymmetry is what gave this away: `-static-libgcc` worked and
	# `-static-libstdc++` did not, from the same flag pair with the
	# same intent, because libgcc.a has no libgcc.so beside it -- the
	# shared one is libgcc_s.so -- while libstdc++.a has libstdc++.so.
	# A filename decided which half of one feature survived. Measured
	# on 192.168.15.95, 2026-09-24: node's --partly-static died at
	# `ld: cannot find -lstdc++` against gcc@16.2.0-17 built under this
	# policy, while gcc@16.2.0-13, an artifact predating it, still
	# carried the archive.
	#
	# Named, and named here rather than inferred, because there is no
	# property of the FILE that distinguishes a toolchain's own runtime
	# from any other library shipping both forms. This is gcc's runtime
	# set; libtcc1.a needs no entry, having no shared counterpart to be
	# dropped against. CLAUDE.md's "never -static" is about glibc, and
	# -static-libstdc++ still links glibc dynamically.
	#
	compiler_runtimes="|libstdc++|libsupc++|libgcc|libgcc_eh|libgcc_s|libatomic|libgomp|libitm|libquadmath|libssp|libobjc|"
	for f in "${archives[@]}"; do
		base="${f##*/}"
		stem="${base%.a}"
		case "$compiler_runtimes" in
		*"|$stem|"*)
			continue
			;;
		esac
		case "$shared_stems" in
		*"|$stem|"*)
			rm -f "$f"
			;;
		esac
	done

	if test ${#elf_dyn[@]} -eq 0; then
		if test ${#elf_rel[@]} -eq 0; then
			return 0
		fi
	fi

	# Clause 1, and ADR-0250: a build that cannot find its tool is a
	# failed build. Never a silent skip.
	if ! command -v strip >/dev/null 2>&1; then
		echo "cix: this package produced ELF output but the build image has no strip." >&2
		echo "cix: add binutils to pkg_build_depends (ADR-0199, ADR-0251)." >&2
		return 1
	fi

	# --strip-unneeded keeps .dynsym, which is all the linker and
	# elfcheck read. A relocatable object (ET_REL: .o, .ko, Go's .syso)
	# keeps .symtab, which linking and module loading genuinely need,
	# so it gets --strip-debug instead.
	# Kernel modules here are unsigned (no CONFIG_MODULE_SIG), so
	# rewriting them does not invalidate a signature.
	for f in "${elf_dyn[@]}"; do
		if ! strip --strip-unneeded "$f"; then
			echo "cix: strip --strip-unneeded failed on $f" >&2
			return 1
		fi
	done
	for f in "${elf_rel[@]}"; do
		if ! strip --strip-debug "$f"; then
			echo "cix: strip --strip-debug failed on $f" >&2
			return 1
		fi
	done

	return 0
}

cix_finalize "$@"
