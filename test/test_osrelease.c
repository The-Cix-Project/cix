/*
 * The platform's identity: /etc/os-release.
 *
 * Two kinds of assertion here, and the second is the point.
 *
 * The ordinary kind: the fields that must be present are present, and
 * the file parses as the shell-compatible KEY=value the freedesktop
 * spec requires.
 *
 * The kind worth having: the fields deliberately ABSENT stay absent.
 * VERSION_ID is omitted because Cix is rolling-release and there is no
 * version to put in it; a later edit that adds one -- which is an easy,
 * plausible, well-meant change -- has to fail this test first and read
 * why. Same for the URL fields, which have nowhere public to point yet.
 * A test that only checks what exists cannot protect a decision not to
 * write something.
 *
 * In SELFTESTS: pure string rendering, no daemon, no container, no
 * filesystem -- it runs anywhere (#224).
 */
#include "osrelease.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void fail(const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	g_failures++;
}

/* True if `text` contains a line that is exactly `line`. */
static int has_line(const char *text, const char *line)
{
	const char *p = text;
	size_t n = strlen(line);

	while (*p != '\0') {
		const char *e = strchr(p, '\n');
		size_t len = (e != NULL) ? (size_t)(e - p) : strlen(p);

		if (len == n && strncmp(p, line, n) == 0)
			return 1;
		if (e == NULL)
			break;
		p = e + 1;
	}
	return 0;
}

/* True if any line starts with `key` followed by '='. */
static int has_key(const char *text, const char *key)
{
	const char *p = text;
	size_t n = strlen(key);

	while (*p != '\0') {
		const char *e = strchr(p, '\n');

		if (strncmp(p, key, n) == 0 && p[n] == '=')
			return 1;
		if (e == NULL)
			break;
		p = e + 1;
	}
	return 0;
}

int main(void)
{
	char buf[OSRELEASE_MAX];
	char small[16];

	if (osrelease_render(buf, sizeof(buf), "v9.9.9") != 0) {
		fail("osrelease_render() failed with a normal buffer");
		return 1;
	}

	/* --- the identity itself --- */

	/*
	 * ID is THE compatibility surface. Everything that reads this file
	 * keys on it, and a consumer finding no ID falls back to "linux".
	 * Asserted as a whole line, not a substring, so `ID=cixtest` or a
	 * trailing comment cannot pass.
	 */
	if (!has_line(buf, "ID=cix"))
		fail("ID=cix is missing -- every reader will identify this host as generic linux");
	if (!has_line(buf, "NAME=\"Cix\""))
		fail("NAME is missing or not exactly \"Cix\"");
	if (!has_key(buf, "PRETTY_NAME"))
		fail("PRETTY_NAME is missing");
	if (!has_line(buf, "BUILD_ID=\"v9.9.9\""))
		fail("BUILD_ID did not carry the version it was given");

	/* --- the deliberate omissions --- */

	if (has_key(buf, "VERSION_ID"))
		fail("VERSION_ID is present. Cix is rolling-release and has no version to put\n"
		     "       in it; a fabricated one tells a reader that two hosts with\n"
		     "       different values run different releases, which is not true here.\n"
		     "       BUILD_ID carries the build. If this is now genuinely wanted, that\n"
		     "       is a real decision -- record it and change this test deliberately.");
	if (has_key(buf, "VERSION"))
		fail("VERSION is present, for the same reason VERSION_ID must not be");
	if (has_key(buf, "HOME_URL") || has_key(buf, "SUPPORT_URL") ||
	    has_key(buf, "DOCUMENTATION_URL") || has_key(buf, "BUG_REPORT_URL"))
		fail("a URL field is present. The only URLs that exist point at git.home.arpa,\n"
		     "       which nothing outside this LAN can resolve -- a URL that fails for\n"
		     "       every reader looks like an answer. Add these when Cix is public.");

	/* --- shell-compatible shape, which the spec requires --- */
	{
		const char *p = buf;

		while (*p != '\0') {
			const char *e = strchr(p, '\n');
			size_t len = (e != NULL) ? (size_t)(e - p) : strlen(p);
			const char *eq = memchr(p, '=', len);

			if (len == 0) {
				fail("blank line in os-release");
			} else if (eq == NULL) {
				fail("line with no '=' in os-release");
			} else if (eq == p) {
				fail("line with an empty key in os-release");
			} else if (eq[-1] == ' ' || eq[1] == ' ') {
				fail("space around '=' -- the file must be shell-sourceable");
			}
			if (e == NULL) {
				fail("os-release does not end with a newline");
				break;
			}
			p = e + 1;
		}
	}

	/* --- BUILD_ID is omitted rather than invented when unknown --- */
	if (osrelease_render(buf, sizeof(buf), NULL) != 0)
		fail("osrelease_render() failed with a NULL version");
	else if (has_key(buf, "BUILD_ID"))
		fail("BUILD_ID present when no version was given -- it should be omitted,\n"
		     "       not written empty");
	if (osrelease_render(buf, sizeof(buf), "") != 0)
		fail("osrelease_render() failed with an empty version");
	else if (has_key(buf, "BUILD_ID"))
		fail("BUILD_ID present for an empty version string");

	/*
	 * A truncated os-release is silently wrong -- it can lose ID and
	 * still look like a file -- so a buffer too small must be an error,
	 * never a short write.
	 */
	if (osrelease_render(small, sizeof(small), "v9.9.9") == 0)
		fail("a buffer far too small returned success -- a truncated os-release can\n"
		     "       drop ID entirely and still parse");
	if (osrelease_render(NULL, 100, "v9.9.9") == 0)
		fail("a NULL buffer returned success");

	if (g_failures > 0) {
		fprintf(stderr, "OSRELEASE: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("OSRELEASE: PASS\n");
	return 0;
}
