#include "childdiag.h"
#include <ctype.h>
#include <string.h>

/* Case-insensitive test for a line that opens a diagnostic. Deliberately
 * only "error": "warning" is not a failure reason, and matching it would
 * pull the wrong line out of a build log that warned and then died. */
static int opens_diagnostic(const char *line)
{
	static const char marker[] = "error";
	size_t i;

	for (i = 0; marker[i] != '\0'; i++) {
		if (tolower((unsigned char)line[i]) != marker[i])
			return 0;
	}
	return 1;
}

void childdiag_reduce_to_error_line(char *buf)
{
	char *best = NULL;
	char *line;

	if (buf == NULL || buf[0] == '\0')
		return;

	/* Split in place. Every line is NUL-terminated by the end of this
	 * loop, so `best` stays a valid standalone string afterwards. */
	line = buf;
	for (;;) {
		char *nl = strchr(line, '\n');

		if (nl != NULL)
			*nl = '\0';
		if (line[0] != '\0') {
			if (opens_diagnostic(line))
				best = line;
			else if (best == NULL || !opens_diagnostic(best))
				/* Last non-empty line, but never displacing a
				 * diagnostic already found above it. */
				best = line;
		}
		if (nl == NULL)
			break;
		line = nl + 1;
	}

	if (best != NULL && best != buf)
		memmove(buf, best, strlen(best) + 1);
	else if (best == NULL)
		buf[0] = '\0';
}
