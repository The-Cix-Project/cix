#ifndef CIX_SHELLWORDS_H
#define CIX_SHELLWORDS_H

/*
 * Splits one line typed at cixctl's interactive shell (and the console
 * shell, which is the same loop) into words, the way a POSIX shell
 * quotes, so a line pasted from a real shell means the same here (#717).
 *
 * - Whitespace separates words.
 * - '...' keeps everything inside literally.
 * - "..." keeps everything inside, except that a backslash before
 *   '"', '\\', '$' or '`' stands for that character.
 * - Outside quotes, a backslash stands for the next character.
 * - A quote may begin anywhere in a word and the quotes are removed:
 *   --secret=KEY='a b' is the one word --secret=KEY=a b.
 * - "" or '' on its own is an empty word.
 *
 * No expansion of any kind: $, `, *, ~ and the rest are ordinary
 * characters. The line is rewritten in place (a word never grows), and
 * tokens[] points into it.
 *
 * Returns the number of words, or -1 with *error set to a sentence for
 * the operator when a quote is never closed or a trailing backslash
 * has nothing to escape. Words past max_tokens are an error too, never
 * a silent truncation.
 */
int shellwords_split(char *line, char **tokens, int max_tokens, const char **error);

#endif
