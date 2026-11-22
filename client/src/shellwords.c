/*
 * shellwords.c -- POSIX-style word splitting for cixctl's interactive
 * shell (#717). See shellwords.h for the contract.
 *
 * Measured on gibsson-1, 2026-11-21: the tokenizer this replaced
 * treated a quote as a delimiter only as a word's first character, so
 * `deployment apply br-2 --secret=RIP_PASSWORD='...'` stored the single
 * quotes as part of the password, and the site router refused every
 * RIP packet. The same line in a real shell was correct.
 */
#include "shellwords.h"

#include <stddef.h>

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int shellwords_split(char *line, char **tokens, int max_tokens, const char **error)
{
	char *r = line; /* read */
	char *w = line; /* write: never ahead of r */
	int n = 0;

	*error = NULL;
	for (;;) {
		while (is_space(*r))
			r++;
		if (*r == '\0')
			break;
		if (n == max_tokens) {
			*error = "too many words on one line";
			return -1;
		}
		tokens[n] = w;
		while (*r != '\0' && !is_space(*r)) {
			if (*r == '\'') {
				r++;
				while (*r != '\0' && *r != '\'')
					*w++ = *r++;
				if (*r == '\0') {
					*error = "a single quote is never closed";
					return -1;
				}
				r++;
			} else if (*r == '"') {
				r++;
				while (*r != '\0' && *r != '"') {
					if (*r == '\\' && (r[1] == '"' || r[1] == '\\' || r[1] == '$' ||
					                   r[1] == '`'))
						r++;
					*w++ = *r++;
				}
				if (*r == '\0') {
					*error = "a double quote is never closed";
					return -1;
				}
				r++;
			} else if (*r == '\\') {
				r++;
				if (*r == '\0' || *r == '\n' || *r == '\r') {
					*error = "a backslash at the end of the line has nothing to escape";
					return -1;
				}
				*w++ = *r++;
			} else {
				*w++ = *r++;
			}
		}
		/* The terminator goes where the word ends. w < r here unless the
		 * word ended the line, where *r is the line's own '\0'. Writing
		 * it at w never overwrites unread input, since w <= r. */
		if (*r != '\0')
			r++;
		*w++ = '\0';
		n++;
	}
	return n;
}
