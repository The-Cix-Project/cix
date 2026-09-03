#compdef cixctl
# zsh completion for cixctl (#151). Same principle as the bash script
# beside it: every candidate comes from `cixctl __complete`, so this file
# describes no commands of its own and cannot fall behind the CLI.
_cixctl() {
	local -a candidates
	local -a words_so_far
	words_so_far=(${words[2,CURRENT-1]} "${words[CURRENT]}")
	candidates=(${(f)"$(cixctl __complete $words_so_far 2>/dev/null)"})
	compadd -- $candidates
}
_cixctl "$@"
