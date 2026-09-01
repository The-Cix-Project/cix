#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# Re-test the EXACT claims of #209, #210 and #212 against the upgraded
# compiler (ADR-0223). Each issue names specific constructs; this runs
# those, not approximations of them, so the issues can be closed on
# evidence or kept open with a current measurement.
#
pkg_name="probe-tcc-conformance"
pkg_version="14"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
pkg_build_depends="tcc gcc binutils make linux-headers bash coreutils sed grep gawk findutils diffutils"
pkg_changelog="14: re-test #209 (-Wp comma lists), #210 (__REDIRECT_NTHNL, glob.h, regexec VLA) and #212 (C11) against the upgraded compiler, using each issue's own reproducer"

pkg_build() {
	mkdir -p /run/p14 && cd /run/p14
	echo "=== compiler ==="
	tcc -v 2>&1 | head -1

	echo
	echo "############ #209: -Wp, comma lists ############"
	printf '#include <stdio.h>\nint main(void){\n#ifdef FOO\nputs("FOO defined");\n#else\nputs("FOO NOT defined");\n#endif\nreturn 0;}\n' > wp.c
	printf '  %-34s ' "-Wp,-DFOO (single arg)"
	if out=$(tcc -Wp,-DFOO -o wp1 wp.c 2>&1); then ./wp1; else echo "FAILS: $(printf '%s' "$out" | head -1)"; fi
	printf '  %-34s ' "-Wp,-MMD,dep.d,-MT,wp.o (list)"
	rm -f dep.d
	if out=$(tcc -Wp,-MMD,dep.d,-MT,wp.o -c wp.c -o wp.o 2>&1); then
		echo "accepted; dep.d written: $([ -f dep.d ] && echo yes || echo NO)"
	else
		echo "FAILS: $(printf '%s' "$out" | head -1)"
	fi
	printf '  %-34s ' "-pthread"
	printf 'int main(void){return 0;}\n' > t.c
	if out=$(tcc -pthread t.c -o t1 2>&1); then echo "accepted; binary: $([ -x t1 ] && echo yes || echo NO)"; else echo "FAILS: $(printf '%s' "$out" | head -1)"; fi

	echo
	echo "############ #210: glibc header constructs ############"
	for m in __REDIRECT __REDIRECT_NTH __REDIRECT_NTHNL; do
		printf '#include <sys/cdefs.h>\nstruct g{int n;};\nextern int %s (myfn,(const char *p, struct g *pg), myfn64);\nint main(void){return 0;}\n' "$m" > r.c
		printf '  %-34s ' "$m"
		if out=$(tcc -c r.c -o r.o 2>&1); then echo "ok"; else echo "FAILS: $(printf '%s' "$out" | head -1)"; fi
	done
	for h in glob.h ftw.h stdio.h dirent.h; do
		printf '#include <%s>\nint main(void){return 0;}\n' "$h" > h.c
		printf '  %-34s ' "<$h> -D_FILE_OFFSET_BITS=64"
		if out=$(tcc -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -c h.c -o h.o 2>&1); then echo "ok"; else echo "FAILS: $(printf '%s' "$out" | tail -1)"; fi
	done
	printf '  %-34s ' "<regex.h> regexec VLA prototype"
	printf '#include <regex.h>\nint main(void){ regex_t r; regmatch_t m[1]; (void)r;(void)m; return 0; }\n' > rx.c
	if out=$(tcc -c rx.c -o rx.o 2>&1); then echo "ok"; else echo "FAILS: $(printf '%s' "$out" | head -1)"; fi

	echo
	echo "############ #212: C11 ############"
	printf '#include <stdio.h>\nint main(void){\n#ifdef __STDC_VERSION__\nprintf("%%ld\\n",(long)__STDC_VERSION__);\n#else\nprintf("undefined\\n");\n#endif\nreturn 0;}\n' > sv.c
	printf '  %-34s ' "__STDC_VERSION__"
	if tcc -o sv sv.c 2>/dev/null; then ./sv; else echo "probe did not build"; fi
	for f in "-std=c11" "-std=gnu11"; do
		printf '  %-34s ' "__STDC_VERSION__ with $f"
		if tcc $f -o sv2 sv.c 2>/dev/null; then ./sv2; else echo "flag rejected or probe failed"; fi
	done
	for h in stdatomic.h stdalign.h stdnoreturn.h; do
		printf '  %-34s ' "<$h>"
		printf '#include <%s>\nint main(void){return 0;}\n' "$h" > c11.c
		if out=$(tcc -c c11.c -o c11.o 2>&1); then echo "ok"; else echo "MISSING"; fi
	done
	printf '  %-34s ' "_Atomic keyword"
	printf 'int main(void){ _Atomic int x = 0; return x; }\n' > at.c
	if out=$(tcc -c at.c -o at.o 2>&1); then echo "ok"; else echo "MISSING: $(printf '%s' "$out" | head -1)"; fi
	printf '  %-34s ' "_Static_assert"
	printf 'int main(void){ _Static_assert(1,"ok"); return 0; }\n' > sa.c
	if out=$(tcc -c sa.c -o sa.o 2>&1); then echo "ok"; else echo "MISSING: $(printf '%s' "$out" | head -1)"; fi
	printf '  %-34s ' "libuv's own trigger line"
	printf '#include <stdatomic.h>\nstatic _Atomic int n;\nint main(void){ atomic_fetch_add(&n,1); return atomic_load(&n)==1?0:1; }\n' > uv.c
	if out=$(tcc -o uv uv.c 2>&1); then ./uv && echo "ok (compiles AND runs)" || echo "compiled but ran wrong"; else echo "FAILS: $(printf '%s' "$out" | head -1)"; fi

	echo
	echo "=== probe complete -- failing on purpose ==="
	exit 1
}

pkg_install() {
	:
}
