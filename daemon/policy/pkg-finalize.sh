#
# ADR-0251 -- a package artifact carries what the platform runs, and
# nothing else.
#
# Written into the build container by cixd (daemon/src/pkg.c), never by
# a recipe. That is the whole point: before this existed, the scope of
# these rules was re-guessed once per recipe, and the result was that
# 37 of 115 recipes pruned anything at all, in twelve different
# spellings, while glibc shipped 9.46 MiB of debug sections in
# libc.so.6 and libc.a three times over.
#
# Sourced by the build command after pkg_install() returns, in the
# build container, with `set -e` already in effect. Uses bash builtins
# for the scan (no find, no file) so it adds no tool dependency of its
# own; strip is required only when the package actually produced ELF.
#

cix_finalize() {
	local dest="$PKG_DESTDIR"
	local elfmagic armagic f base stem magic arsize t
	local shared_stems=""
	local -a elf_dyn=() elf_rel=() archives=()

	if ! test -d "$dest"; then
		return 0
	fi

	# ADR-0250: name every tool this phase needs, and fail on absence
	# rather than skipping. rm and wc are what the prune needs -- both
	# from coreutils, so this asks for no package the prune did not
	# already require; strip is checked later, and only if ELF was
	# produced.
	for t in rm wc; do
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
			elf_dyn+=("$f")
			;;
		*.o|*.ko)
			elf_rel+=("$f")
			;;
		*)
			elf_dyn+=("$f")
			;;
		esac
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
	for f in "${archives[@]}"; do
		base="${f##*/}"
		stem="${base%.a}"
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
	# elfcheck read. .o and .ko keep .symtab, which linking and module
	# loading genuinely need, so those get --strip-debug instead.
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

cix_finalize
