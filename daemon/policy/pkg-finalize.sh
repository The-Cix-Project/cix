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
	local elfmagic armagic f base stem magic d
	local shared_stems=""
	local -a elf_dyn=() elf_rel=() archives=()

	if ! test -d "$dest"; then
		return 0
	fi

	# ADR-0250: name every tool this phase needs, and fail on absence
	# rather than skipping. rm is the only external tool the prune
	# needs; strip is checked later, and only if ELF was produced.
	if ! command -v rm >/dev/null 2>&1; then
		echo "cix: package finalize needs rm and the build image has none." >&2
		echo "cix: add coreutils to pkg_build_depends (ADR-0199, ADR-0251)." >&2
		return 1
	fi

	shopt -s nullglob dotglob globstar

	# Clause 4: documentation and locale trees.
	for d in man info doc locale i18n; do
		rm -rf "$dest/usr/share/$d"
	done

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
	for f in "${archives[@]}"; do
		base="${f##*/}"
		stem="${base%.a}"
		case "$shared_stems" in
		*"|$stem|"*)
			rm -f "$f"
			;;
		*)
			elf_rel+=("$f")
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
	# elfcheck read. .o/.ko/.a keep .symtab, which linking and module
	# loading genuinely need, so those get --strip-debug instead.
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
