/*
 * test_timebounds -- a timing bound in a test must be finer than the
 * clock it is written against.
 *
 * #309. Three daemon-linked tests failed in one session, each cost a
 * build round, and two of them turned out to be defects in the tests.
 * One of those two has a shape precise enough to gate:
 *
 *     if (t1.tv_sec - t0.tv_sec > 2)   / * FAIL: too slow * /
 *
 * That is not a two-second bound. tv_sec is a whole-second counter, so
 * the difference of two of them is 2 for an elapsed 2.9 s that started
 * at .05, and 3 for an elapsed 2.1 s that started at .95. The real
 * threshold sweeps a full second depending on nothing but where the run
 * landed inside one -- so the same unchanged code passes or fails on
 * clock alignment. test_cix_init case 12 failed twice on exactly this
 * while nothing about cix-init had changed, and both failures were
 * investigated as regressions.
 *
 * The fix is always the same and always trivial: compute the elapsed
 * time in milliseconds (or nanoseconds) and compare that. What is not
 * trivial is remembering to, which is what this file is for -- a
 * whole-second difference is the natural thing to write, reads
 * correctly, and gives no sign of being wrong until a build fails for a
 * reason that has nothing to do with the change in it.
 *
 * COMMENTS ARE SKIPPED, and that is not a loophole -- a gate that fires
 * on prose describing the bug teaches people not to describe it. The
 * test is textual and so is the skip: a line whose first non-blank
 * character opens or continues a comment is not code. It costs a
 * contrived miss (a bound trailing a block comment on one line) against
 * a certain loss (this very file, and the explanation in test_cix_init,
 * both unable to name the shape they exist to stop).
 *
 * SCOPE, deliberately narrow. Only a difference of two tv_sec fields
 * fed to a relational operator is refused. Subtracting them to PRINT an
 * approximate duration is fine and common, and a bound written on
 * tv_nsec or on a millisecond helper is the thing being asked for. One
 * shape, no judgement calls, no false positives to argue with -- the
 * same posture test_curl_guards and test_blocking_waits already take.
 *
 * WHAT THIS DOES NOT CATCH, said plainly rather than left to be
 * discovered: the other #309 defect was a settle-poll that returned
 * SUCCESS on timeout, so a slow teardown surfaced as the next call's
 * failure. That one has no reliable textual signature -- a bounded
 * loop that reports nothing looks exactly like a bounded loop that
 * cannot fail -- and a regex approximating it would cost more in false
 * positives than it finds. It stays a review matter; this gate takes
 * the half that is mechanical.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define TEST_DIR "test"

/*
 * A line offends when a tv_sec difference reaches a relational
 * operator. "->" is stripped first: t1->tv_sec - t0->tv_sec is the
 * ordinary pointer form of the same expression and its own '>' is not a
 * comparison. Assignment-free by design -- "==" and "!=" are not
 * relational and a test comparing two timestamps for equality is not
 * this bug.
 */
static int line_is_comment(const char *line)
{
	size_t i = 0;

	while (line[i] == ' ' || line[i] == '\t')
		i++;
	if (line[i] == '*')
		return 1; /* a continuation line of a block comment */
	return line[i] == '/' && (line[i + 1] == '*' || line[i + 1] == '/');
}

static int line_offends(const char *line)
{
	char stripped[4096];
	const char *diff;
	size_t o = 0;
	size_t i;

	if (line_is_comment(line))
		return 0;

	for (i = 0; line[i] != '\0' && o + 1 < sizeof(stripped); i++) {
		if (line[i] == '-' && line[i + 1] == '>') {
			stripped[o++] = '.';
			i++;
			continue;
		}
		stripped[o++] = line[i];
	}
	stripped[o] = '\0';

	diff = strstr(stripped, "tv_sec -");
	if (diff == NULL)
		return 0;
	/* The bound sits after the difference, on the same expression. */
	return strpbrk(diff, "<>") != NULL;
}

int main(void)
{
	DIR *d = opendir(TEST_DIR);
	struct dirent *de;
	int offences = 0;
	int files = 0;

	if (d == NULL) {
		printf("TIMEBOUNDS RESULT: FAIL (cannot open %s -- run from the repo root)\n", TEST_DIR);
		return 1;
	}
	while ((de = readdir(d)) != NULL) {
		char path[512];
		char line[4096];
		FILE *f;
		int lineno = 0;
		size_t len = strlen(de->d_name);

		if (len < 3 || strcmp(de->d_name + len - 2, ".c") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", TEST_DIR, de->d_name);
		f = fopen(path, "r");
		if (f == NULL)
			continue;
		files++;
		while (fgets(line, sizeof(line), f) != NULL) {
			lineno++;
			if (!line_offends(line))
				continue;
			printf("%s:%d: a bound on whole tv_sec seconds -- its real threshold "
			       "sweeps a full second. Compare elapsed milliseconds.\n%s",
			       path, lineno, line);
			offences++;
		}
		fclose(f);
	}
	closedir(d);

	if (files == 0) {
		printf("TIMEBOUNDS RESULT: FAIL (no test sources found -- the gate read nothing)\n");
		return 1;
	}
	if (offences != 0) {
		printf("TIMEBOUNDS RESULT: FAIL (%d whole-second bound(s) across %d file(s))\n",
		       offences, files);
		return 1;
	}
	printf("TIMEBOUNDS RESULT: PASS (%d test sources, no whole-second bounds)\n", files);
	return 0;
}
