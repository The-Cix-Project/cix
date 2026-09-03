# bash completion for cixctl (#151).
#
# The candidate list comes from cixctl itself (`cixctl __complete`), which
# reads the same command tree the CLI is built from. Nothing about the
# command surface is described here, so this file cannot go stale when a
# command is added -- which is the usual fate of a hand-written completion
# script.
#
# `__complete` makes no HTTP call, so pressing Tab never blocks on a slow
# or unreachable daemon.
#
# Install: source this from ~/.bashrc, or drop it in
# /usr/share/bash-completion/completions/cixctl
_cixctl_complete()
{
	local words=("${COMP_WORDS[@]:1:COMP_CWORD}")

	# COMP_WORDS drops the trailing empty word when the line ends in a
	# space, and that is exactly the "offer everything valid here" case,
	# so it is put back.
	[ "${#words[@]}" -eq 0 ] && words=("")

	COMPREPLY=($(compgen -W "$(cixctl __complete "${words[@]}" 2>/dev/null)" -- "${COMP_WORDS[COMP_CWORD]}"))

	# A flag that takes a value ends in '='; leaving the space off lets
	# the value be typed straight after it.
	if [ "${#COMPREPLY[@]}" -eq 1 ] && [[ "${COMPREPLY[0]}" == *= ]]; then
		compopt -o nospace 2>/dev/null
	fi
}
complete -F _cixctl_complete cixctl
