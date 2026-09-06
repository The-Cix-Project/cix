/*
 * See srcdepth.h for the grammar and why `n-1` and `n-0.1` are
 * deliberately different things.
 */
#include "srcdepth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *srcdepth_strerror(enum srcdepth_error e)
{
	switch (e) {
	case SRCDEPTH_OK:
		return "ok";
	case SRCDEPTH_ERR_SYNTAX:
		return "not a depth expression";
	case SRCDEPTH_ERR_NO_CANDIDATES:
		return "no candidate releases";
	case SRCDEPTH_ERR_NO_SUCH_LINE:
		return "not that many release lines";
	case SRCDEPTH_ERR_NO_SUCH_RELEASE:
		return "not that many releases in that line";
	}
	return "unknown";
}

/*
 * Reads one run of digits. Returns -1 for "no digits here", which is
 * how every syntax rejection below is expressed -- there is no separate
 * "is this a number" pass that could disagree with the parse.
 */
static int read_uint(const char **p)
{
	const char *s = *p;
	long v = 0;

	if (*s < '0' || *s > '9')
		return -1;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (*s - '0');
		if (v > 100000) /* absurd; treat as malformed rather than wrapping */
			return -1;
		s++;
	}
	*p = s;
	return (int)v;
}

enum srcdepth_error srcdepth_parse(const char *s, int *out_lines, int *out_releases)
{
	int lines = 0;
	int releases = 0;

	if (s == NULL || (s[0] != 'n' && s[0] != 'N'))
		return SRCDEPTH_ERR_SYNTAX;
	s++;
	if (*s != '\0') {
		if (*s != '-')
			return SRCDEPTH_ERR_SYNTAX;
		s++;
		lines = read_uint(&s);
		if (lines < 0)
			return SRCDEPTH_ERR_SYNTAX;
		if (*s == '.') {
			s++;
			releases = read_uint(&s);
			if (releases < 0)
				return SRCDEPTH_ERR_SYNTAX;
		}
		if (*s != '\0') /* trailing junk, or a third component */
			return SRCDEPTH_ERR_SYNTAX;
	}
	if (out_lines != NULL)
		*out_lines = lines;
	if (out_releases != NULL)
		*out_releases = releases;
	return SRCDEPTH_OK;
}

struct srcdepth_version srcdepth_version_parse(const char *s)
{
	struct srcdepth_version v;
	int n;

	memset(&v, 0, sizeof(v));
	if (s == NULL)
		return v;
	while (*s == ' ' || *s == '\t')
		s++;
	/*
	 * A leading "v" is common in tags and means nothing here. Accepting
	 * it costs one line and avoids every caller having to strip it.
	 */
	if (*s == 'v' || *s == 'V')
		s++;

	n = read_uint(&s);
	if (n < 0)
		return v;
	v.major = n;
	if (*s == '.') {
		s++;
		n = read_uint(&s);
		if (n < 0)
			return v;
		v.minor = n;
		if (*s == '.') {
			s++;
			n = read_uint(&s);
			if (n < 0)
				return v;
			v.patch = n;
		}
	}
	/*
	 * Anything left must be this platform's own recipe suffix ("-24")
	 * or nothing. A version with some other tail ("7.2.3-rc1") is not a
	 * release this should silently treat as 7.2.3, so it is rejected.
	 */
	if (*s == '-') {
		const char *q = s + 1;

		if (read_uint(&q) < 0 || *q != '\0')
			return v;
	} else if (*s != '\0') {
		return v;
	}
	v.valid = 1;
	return v;
}

int srcdepth_version_cmp(const struct srcdepth_version *a, const struct srcdepth_version *b)
{
	if (a->major != b->major)
		return (a->major < b->major) ? -1 : 1;
	if (a->minor != b->minor)
		return (a->minor < b->minor) ? -1 : 1;
	if (a->patch != b->patch)
		return (a->patch < b->patch) ? -1 : 1;
	return 0;
}

#define SRCDEPTH_MAX_CANDIDATES 512

enum srcdepth_error srcdepth_resolve(const char *const *candidates, size_t count,
                                      int lines, int releases,
                                      char *out, size_t out_size,
                                      char *err, size_t err_size)
{
	struct srcdepth_version v[SRCDEPTH_MAX_CANDIDATES];
	const char *raw[SRCDEPTH_MAX_CANDIDATES];
	size_t n = 0;
	size_t i, j;
	int seen_lines = 0;
	int cur_major = 0, cur_minor = 0;
	size_t in_line = 0;

	if (err != NULL && err_size > 0)
		err[0] = '\0';
	if (out != NULL && out_size > 0)
		out[0] = '\0';
	if (candidates == NULL || count == 0)
		return SRCDEPTH_ERR_NO_CANDIDATES;

	/* Parse, dropping anything unparseable rather than failing: one bad
	 * entry in an upstream feed must not take a package offline. */
	for (i = 0; i < count && n < SRCDEPTH_MAX_CANDIDATES; i++) {
		struct srcdepth_version p = srcdepth_version_parse(candidates[i]);

		if (!p.valid)
			continue;
		/* Drop exact duplicates -- a feed listing a version twice must
		 * not make "one release back" mean "the same release". */
		for (j = 0; j < n; j++)
			if (srcdepth_version_cmp(&v[j], &p) == 0)
				break;
		if (j < n)
			continue;
		v[n] = p;
		raw[n] = candidates[i];
		n++;
	}
	if (n == 0) {
		if (err != NULL)
			snprintf(err, err_size, "no candidate release parsed as a version");
		return SRCDEPTH_ERR_NO_CANDIDATES;
	}

	/* Descending by version: newest line first, newest release first. */
	for (i = 0; i + 1 < n; i++) {
		for (j = 0; j + 1 < n - i; j++) {
			if (srcdepth_version_cmp(&v[j], &v[j + 1]) < 0) {
				struct srcdepth_version tv = v[j];
				const char *tr = raw[j];

				v[j] = v[j + 1];
				v[j + 1] = tv;
				raw[j] = raw[j + 1];
				raw[j + 1] = tr;
			}
		}
	}

	/*
	 * Two passes rather than one, because the error message has to name
	 * the line it was talking about. A single loop that breaks once it
	 * walks past the target has already advanced cur_major/in_line to
	 * the NEXT line by then, and would report "release 5 of line 7.1,
	 * which publishes only 0" when the truth is about 7.2. Counting
	 * first and choosing second keeps the numbers honest.
	 */

	/* Pass 1: how many distinct lines, and where does each start. */
	{
		size_t line_start[SRCDEPTH_MAX_CANDIDATES];
		size_t line_count[SRCDEPTH_MAX_CANDIDATES];
		size_t nlines = 0;

		for (i = 0; i < n; i++) {
			if (nlines == 0 || v[i].major != cur_major || v[i].minor != cur_minor) {
				cur_major = v[i].major;
				cur_minor = v[i].minor;
				line_start[nlines] = i;
				line_count[nlines] = 0;
				nlines++;
			}
			line_count[nlines - 1]++;
		}
		seen_lines = (int)nlines;

		if (lines < 0 || (size_t)lines >= nlines) {
			if (err != NULL)
				snprintf(err, err_size,
				          "depth asks to go back %d release line(s), but the channel "
				          "publishes only %d line(s)",
				          lines, seen_lines);
			return SRCDEPTH_ERR_NO_SUCH_LINE;
		}

		/* Pass 2: pick within that line. */
		in_line = line_count[lines];
		i = line_start[lines];
		if (releases < 0 || (size_t)releases >= in_line) {
			if (err != NULL)
				snprintf(err, err_size,
				          "depth asks for release %d back within line %d.%d, which "
				          "publishes only %d release(s)",
				          releases, v[i].major, v[i].minor, (int)in_line);
			return SRCDEPTH_ERR_NO_SUCH_RELEASE;
		}
		if (out != NULL)
			snprintf(out, out_size, "%s", raw[i + (size_t)releases]);
		return SRCDEPTH_OK;
	}
}
