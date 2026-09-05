/*
 * test_curl_guards -- every curl this daemon forks carries a stall
 * guard (#285).
 *
 * curl waits forever by default once connected, so a peer that accepts
 * the TCP connection and then says nothing hangs the fetch with no
 * ceiling. Nine of the ten call sites had no guard at all, and the
 * consequence ranged from a package job stuck in `state: building`
 * that neither completes nor fails, to -- for the one synchronous call
 * site, reached straight from POST /v1/system/update -- the entire
 * control plane blocked for as long as the peer cares to stay silent.
 *
 * Not hypothetical: ftp.gnu.org did exactly this to this site's egress
 * for several days, connecting on both 443 and 80 and then answering
 * nothing.
 *
 * A STATIC scan, deliberately, and that is the whole reason it is worth
 * having. The failure guarded against is not a wrong value, it is a NEW
 * call site written without one -- which no runtime test reaches,
 * because the new path is exactly the one nothing exercises yet.
 * Reading the source for "does every exec of the curl binary have a
 * guard near it" needs no network, no daemon and no container, and it
 * fails the build the moment someone adds the eleventh site and
 * forgets.
 *
 * Crude on purpose, in the same spirit as test_api_surfaces: it looks
 * for the text, not for syntax. There is no parse to get wrong.
 */
#include <stdio.h>
#include <string.h>

/*
 * How far AFTER the argv starts the guard may appear.
 *
 * Forward from the argv, not backward from the execve(), and the
 * difference is not cosmetic. Backward was tried first and gave a false
 * failure immediately: the recipe source fetch builds its argv 49 lines
 * above its exec, because a long comment about `-C -` sits between
 * them, so any backward window wide enough to cover it is also wide
 * enough to credit an unrelated guard from somewhere else in the
 * function. Forward has no such ambiguity -- the guard is part of the
 * argv, so it is within a few lines of where the argv begins, always.
 */
#define GUARD_WINDOW_LINES 14

static int failures;
static int sites;

/*
 * One source file. Every place a curl argv is BUILT -- an initialiser
 * naming the binary, or the indexed `argv[0] = ...` form three call
 * sites use -- must name a guard within the window below.
 */
static void scan(const char *path)
{
	static char text[1 << 20];
	char *lines[40000];
	int count = 0;
	size_t n;
	char *p;
	int i;
	FILE *f = fopen(path, "r");

	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		failures++;
		return;
	}
	n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = '\0';

	/*
	 * Split by hand rather than with strtok(), which treats a run of
	 * newlines as one delimiter and so does not count blank lines.
	 * That is invisible until something fails: the scan still finds
	 * the right site, and then names a line number several hundred off
	 * -- measured, when a deliberately reintroduced regression was
	 * reported at 7802 for a site at 8476. A failure message that
	 * sends the reader to the wrong place is worse than one that gives
	 * no place at all.
	 */
	p = text;
	while (p != NULL && *p != '\0' && count < (int)(sizeof(lines) / sizeof(lines[0]))) {
		char *nl = strchr(p, '\n');

		lines[count++] = p;
		if (nl == NULL)
			break;
		*nl = '\0';
		p = nl + 1;
	}

	for (i = 0; i < count; i++) {
		int j, guarded = 0, limit;

		/* Where an argv is built, in both spellings this daemon uses.
		 * The execve() line itself is deliberately NOT a match: it
		 * names the binary too, and counting it would double every
		 * site. */
		if (strstr(lines[i], "(char *)PKG_CURL_BIN") == NULL &&
		    strstr(lines[i], "argv[0] = (char *)PKG_CURL_BIN") == NULL)
			continue;
		if (strstr(lines[i], "execve(") != NULL)
			continue;
		sites++;

		limit = i + GUARD_WINDOW_LINES < count ? i + GUARD_WINDOW_LINES : count;
		for (j = i; j < limit; j++) {
			if (strstr(lines[j], "PKG_CURL_STALL_GUARD_ARGS") != NULL ||
			    strstr(lines[j], "PKG_CURL_CONNECT_TIMEOUT") != NULL) {
				guarded = 1;
				break;
			}
		}
		if (!guarded) {
			fprintf(stderr,
			        "FAIL: %s:%d builds a curl argv with no stall guard within %d lines -- "
			        "add PKG_CURL_STALL_GUARD_ARGS to it, or curl will wait forever on a "
			        "peer that connects and then says nothing (#285)\n",
			        path, i + 1, GUARD_WINDOW_LINES);
			failures++;
		}
	}
}

int main(void)
{
	scan("daemon/src/pkg.c");
	scan("daemon/src/main.c");

	/*
	 * The count is asserted as well as the guards. A scan that finds
	 * nothing passes trivially, and "the pattern moved and this test
	 * silently stopped looking at anything" is the way a static check
	 * rots -- the same reason test_apigen pins its operation count.
	 * Ten sites today: six in pkg.c, four in main.c. If that changes,
	 * change this number deliberately.
	 */
	if (sites != 10) {
		fprintf(stderr,
		        "FAIL: found %d curl argv sites, expected 10 -- if a call site was genuinely "
		        "added or removed, update this number deliberately; a silently different "
		        "count is how an unguarded fetch hides\n",
		        sites);
		failures++;
	}

	printf("CURL GUARDS: %s (%d curl argv sites checked)\n", failures == 0 ? "PASS" : "FAIL",
	       sites);
	return failures == 0 ? 0 : 1;
}
