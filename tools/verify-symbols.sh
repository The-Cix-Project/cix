#!/bin/bash
#
# verify-symbols.sh -- the link errors a per-file compile cannot see.
#
# The dev sandbox builds cixctl and nothing else, so cixd is never
# linked here and a whole class of defect survives a clean
# file-by-file compile. This compiles every source to an object and
# asks the symbol tables the three questions the linker would:
#
#   1. does anything define the same global twice
#   2. is anything referenced externally but defined only as a static
#   3. does main.c declare a static it never defines
#
# The second is not hypothetical. api_network.c carried a local forward
# declaration of dhcp_apply_and_maybe_restart(), which is static in
# main.c -- no shared header, so neither file had anything to disagree
# with. Both compiled clean under -Wall -Werror, and the failure
# appeared as "unresolved reference" only when a real Cix host linked
# it, one full build cycle later.
#
# It found that because it asks the symbol table rather than matching
# names: a symbol undefined in one object and defined LOCALLY (nm's
# lowercase t/d/b) in another is exactly that mistake, whatever it is
# called. An earlier version used a libc name heuristic instead and was
# useless -- it listed swapon and stdout and missed the real one.
#
# Proven rather than assumed: injecting that shape makes check 2 fail
# and name the symbol, while both compilers stay silent.
#
# Not a selftest: the host build links for real and would catch these
# anyway. This exists to catch them before a ten-minute build cycle is
# spent finding out.
#
# The full pre-release check. Both symbol checks run every time --
# dropping the unresolved one is exactly how an unresolved reference to
# dhcp_apply_and_maybe_restart reached a real build.
cd /home/osakka/new_project
rm -rf /tmp/objs && mkdir -p /tmp/objs
fail=0
for f in daemon/src/*.c daemon/src/vendor/*.c src/*.c netplane/src/*.c; do
  tcc -Wall -Werror -D_GNU_SOURCE -D_FORTIFY_SOURCE=0 -Iinclude -Idaemon/include -Inetplane/include -Itest -Ibuild -Ibuild/generated -c $f -o /tmp/objs/$(echo $f|tr '/' '_').o 2>/tmp/e.log || { echo "COMPILE FAIL $f: $(head -1 /tmp/e.log)"; fail=1; }
done
[ $fail -eq 0 ] && echo "compile:      OK ($(ls /tmp/objs|wc -l) objects)"
dup=$(nm --defined-only /tmp/objs/*.o 2>/dev/null|awk '/^[0-9a-f]+ [TDB] /{print $3}'|sort|uniq -d)
[ -z "$dup" ] && echo "duplicates:   OK" || { echo "DUPLICATE GLOBALS: $dup"; fail=1; }
unres=$(python3 - <<'PYEOF'
import subprocess, glob
# A project symbol referenced from one object and defined only as a
# STATIC (lowercase t/d/b) in another is precisely the failure that
# reached a real build: dhcp_apply_and_maybe_restart(), declared
# non-static in api_network.c and defined static in main.c. No name
# heuristics -- the symbol table says which are ours.
glob_defs=set(); local_defs=set(); undef=set()
for o in glob.glob("/tmp/objs/*.o"):
    for line in subprocess.run(["nm",o],capture_output=True,text=True).stdout.split("\n"):
        p=line.split()
        if len(p)==2 and p[0]=="U": undef.add(p[1])
        elif len(p)==3:
            if p[1] in "TDBRW": glob_defs.add(p[2])
            elif p[1] in "tdbrw": local_defs.add(p[2])
bad=sorted((undef - glob_defs) & local_defs)
print(" ".join(bad))
PYEOF
)

[ -z "$unres" ] && echo "unresolved:   OK" || { echo "UNRESOLVED PROJECT SYMBOLS: $unres"; fail=1; }
decl=$(python3 -c "
import re
s=open('daemon/src/main.c').read()
d=set(re.findall(r'^static [\w \*]+?\b(\w+)\s*\([^;]*\);\s*\$', s, re.M))
f=set(re.findall(r'^static [\w \*]+?\b(\w+)\s*\([^;]*\)\s*\$', s, re.M))|set(re.findall(r'^static [\w \*]+?\b(\w+)\s*\([^;]*\)\s*\{', s, re.M))
print(' '.join(sorted(x for x in d if x not in f)))")
[ -z "$decl" ] && echo "definitions:  OK" || { echo "DECLARED BUT UNDEFINED: $decl"; fail=1; }
exit $fail
