/*
 * test_curl_guards -- every fetch/upload this daemon performs carries a
 * stall guard (#285, #410).
 *
 * A connection can be accepted and then never answered, so a transfer
 * with no bound waits forever. Nine of the ten curl call sites this
 * test originally guarded had no guard at all, and the consequence
 * ranged from a package job stuck in `state: building` that neither
 * completes nor fails, to -- for the one synchronous call site, reached
 * straight from POST /v1/system/update -- the entire control plane
 * blocked for as long as the peer cares to stay silent.
 *
 * Not hypothetical: ftp.gnu.org did exactly this to this site's egress
 * for several days, connecting on both 443 and 80 and then answering
 * nothing.
 *
 * #410 replaced every execve(curl, ...) call site with an in-process
 * curlfetch_perform() call, so the guard moved from nearby argv text to
 * a field on the struct curlfetch_opts each call site builds. This
 * test moved with it: it now looks for `.connect_timeout =` or
 * `.max_time =` set in the lines BEFORE each curlfetch_perform( call,
 * rather than a guard macro in the lines after an argv literal --
 * opts are always built as a sequence of `opts.field = ...;`
 * assignments immediately preceding the call that uses them, never
 * after.
 *
 * A STATIC scan, deliberately, and that is the whole reason it is worth
 * having. The failure guarded against is not a wrong value, it is a NEW
 * call site written without one -- which no runtime test reaches,
 * because the new path is exactly the one nothing exercises yet.
 * Reading the source for "does every curlfetch_perform() call have a
 * guard near it" needs no network, no daemon and no container, and it
 * fails the build the moment someone adds a tenth site and forgets.
 *
 * Crude on purpose, in the same spirit as test_api_surfaces: it looks
 * for the text, not for syntax. There is no parse to get wrong.
 */
#include <stdio.h>
#include <string.h>

/* How far BACKWARD from curlfetch_perform( the opts-building guard
 * assignment may appear. Every real call site in this codebase sets it
 * within a handful of lines of the call; wide enough to cover the
 * longest real gap (a doc comment between the last opts.* assignment
 * and the call) without crediting an unrelated guard from somewhere
 * else in the function. */
#define GUARD_WINDOW_LINES 20

static int failures;
static int sites;

/* Same check test_blocking_waits.c uses: a line whose first non-blank
 * character starts a comment. Needed here because this file's own doc
 * comments mention "curlfetch_perform()" by name repeatedly -- without
 * this, every one of those mentions would be counted as a call site. */
static int is_comment(const char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	return line[0] == '*' || (line[0] == '/' && (line[1] == '*' || line[1] == '/'));
}

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
	 * newlines as one delimiter and so does not count blank lines --
	 * invisible until a failure names the wrong line number. See
	 * test_apigen's own identical reasoning.
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
		int j, guarded = 0, start;

		/* Only a real call, not a comment mentioning the function
		 * name (this file's own doc comments do, repeatedly) or the
		 * declaration/definition itself. */
		if (is_comment(lines[i]))
			continue;
		if (strstr(lines[i], "curlfetch_perform(") == NULL)
			continue;
		if (strstr(lines[i], "int curlfetch_perform") != NULL)
			continue; /* the declaration/definition itself */
		sites++;

		start = i - GUARD_WINDOW_LINES > 0 ? i - GUARD_WINDOW_LINES : 0;
		for (j = start; j <= i; j++) {
			if (strstr(lines[j], ".connect_timeout") != NULL ||
			    strstr(lines[j], ".max_time") != NULL) {
				guarded = 1;
				break;
			}
		}
		if (!guarded) {
			fprintf(stderr,
			        "FAIL: %s:%d calls curlfetch_perform() with no stall guard "
			        "(.connect_timeout or .max_time) set within %d lines before it -- a "
			        "transfer with neither will wait forever on a peer that connects and "
			        "then says nothing (#285/#410)\n",
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
	 *
	 * Nine sites as of #410: the twelve execve(curl, ...) sites this
	 * test used to count collapsed to nine curlfetch_perform() calls,
	 * because three with/without-header argv-branch pairs -- pkg_sync_
	 * start, and start_fetch_for's artifact-sha and signature fetches,
	 * each of which was TWO separate execve() sites before #410 -- now
	 * each share one curlfetch_perform() call, the header passed as an
	 * optional field instead of chosen between two argv branches.
	 * Five in pkg.c (pkg_sync_start, start_fetch_for's artifact-sha,
	 * signature and main-source-tarball fetches, pkg_artifact_push_
	 * try_start), four in main.c (fetch_update_image, start_iso_
	 * publish_upload, bootstrap_fetch_start, kernel_releases_fetch_
	 * start). If that changes, change this number deliberately.
	 */
	if (sites != 9) {
		fprintf(stderr,
		        "FAIL: found %d curlfetch_perform() call sites, expected 9 -- if a call site "
		        "was genuinely added or removed, update this number deliberately; a silently "
		        "different count is how an unguarded fetch hides\n",
		        sites);
		failures++;
	}

	printf("CURL GUARDS: %s (%d curlfetch_perform() sites checked)\n", failures == 0 ? "PASS" : "FAIL",
	       sites);
	return failures == 0 ? 0 : 1;
}
