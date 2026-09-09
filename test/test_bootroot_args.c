/*
 * test_bootroot_args -- the control-plane root cannot be assembled with
 * a capability switched off by a hardcoded empty string.
 *
 * mkbootroot takes four optional staging arguments: firmware, the
 * kernel module tree, the kmod tool directory, and host tools. Each is
 * fully implemented in image/src/mkbootroot.c, each is skipped when
 * given "", and each has exactly one real caller --
 * spawn_cix_bootroot_assembly() in daemon/src/main.c.
 *
 * Two of the four were passed a literal "" there, with a comment
 * explaining why the capability was not needed, and both explanations
 * were wrong for the same reason: mkbootroot assembles a FRESH root
 * every time, so whatever it is not given, the assembled root does not
 * have.
 *
 *   - firmware (ADR-0029) shipped with a complete implementation, a
 *     test, and no caller. A wireless adapter had nowhere for
 *     rtw88/rtw8822b_fw.bin to come from.
 *   - modules and kmod (#347) shipped the same way. No kernel module
 *     could load on any Cix host ever assembled: GET
 *     /v1/system/kmod/e1000e answered "no such module
 *     (not built/available)" because there was no tree to find it in.
 *     A box with every driver built in never notices; a bare-metal box
 *     whose NIC driver is a module, and which has no shell to repair
 *     itself with, is unreachable.
 *
 * Twice out of four is a pattern, not an accident, and neither
 * instance was catchable by a runtime test: the code was correct and
 * complete, and the call site was where the feature was silently
 * turned off. So the gate is on the call site.
 *
 * Deliberately narrow: it asserts only that these arguments are
 * assigned from a variable rather than a string literal. What that
 * variable resolves to is the daemon's business, and every one of them
 * legitimately becomes "" at runtime on a box that never built the
 * image behind it. The bug this catches is the decision being made at
 * compile time, where no operator can see or change it.
 */
#include <stdio.h>
#include <string.h>

#define MAIN_C "daemon/src/main.c"

/* The optional staging arguments, by mkbootroot's own argv index. */
static const struct {
	const char *assign;
	const char *what;
} g_args[] = {
	{ "argv[6] =", "firmware (ADR-0029)" },
	{ "argv[7] =", "kernel modules (#347)" },
	{ "argv[8] =", "kmod tools (#347)" },
	{ "argv[9] =", "host tools (ADR-0078)" },
};
#define NARGS ((int)(sizeof(g_args) / sizeof(g_args[0])))

int main(void)
{
	FILE *f;
	char line[4096];
	int in_assembly = 0;
	int seen[NARGS];
	int bad = 0;
	int i;

	for (i = 0; i < NARGS; i++)
		seen[i] = 0;

	f = fopen(MAIN_C, "r");
	if (f == NULL) {
		printf("BOOTROOT ARGS: FAIL (cannot read %s -- run from the repository root)\n", MAIN_C);
		return 1;
	}

	/*
	 * Scoped to the function rather than the file: argv[6..9] is an
	 * ordinary spelling that appears in several unrelated exec call
	 * sites here (curl, the ISO builder, the kernel release fetch),
	 * and this gate has an opinion about exactly one of them.
	 */
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strstr(line, "static void spawn_cix_bootroot_assembly(const char *artifact_dir)\n") !=
		    NULL) {
			in_assembly = 1;
			continue;
		}
		if (!in_assembly)
			continue;
		if (line[0] == '}') /* end of the function body */
			break;
		for (i = 0; i < NARGS; i++) {
			const char *p = strstr(line, g_args[i].assign);
			const char *val;

			if (p == NULL)
				continue;
			seen[i] = 1;
			val = p + strlen(g_args[i].assign);
			while (*val == ' ' || *val == '\t')
				val++;
			if (*val == '"') {
				printf("FAIL: mkbootroot's %s argument is a hardcoded string literal in "
				       "spawn_cix_bootroot_assembly() -- %s\n",
				       g_args[i].what, g_args[i].assign);
				bad = 1;
			}
		}
	}
	fclose(f);

	if (!in_assembly) {
		printf("BOOTROOT ARGS: FAIL (spawn_cix_bootroot_assembly() not found in %s -- if it was "
		       "renamed, this gate must follow it, not be deleted)\n",
		       MAIN_C);
		return 1;
	}
	for (i = 0; i < NARGS; i++) {
		if (!seen[i]) {
			printf("FAIL: %s (%s) is never assigned in spawn_cix_bootroot_assembly() -- an "
			       "argument that stopped being passed at all is the same bug as one passed "
			       "empty\n",
			       g_args[i].assign, g_args[i].what);
			bad = 1;
		}
	}

	if (bad) {
		printf("BOOTROOT ARGS: FAIL\n");
		return 1;
	}
	printf("BOOTROOT ARGS: PASS (%d optional staging arguments, all resolved at runtime)\n",
	       NARGS);
	return 0;
}
