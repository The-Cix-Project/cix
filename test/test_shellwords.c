/*
 * test_shellwords.c -- cixctl's interactive shell splits a line the way
 * a POSIX shell quotes it (#717). The case that failed for real is the
 * first: a quote starting mid-word kept its quotes.
 */
#include "shellwords.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect(const char *input, int want_n, const char *const *want)
{
	char line[512];
	char *tokens[16];
	const char *err;
	int n, i;

	snprintf(line, sizeof(line), "%s", input);
	n = shellwords_split(line, tokens, 16, &err);
	if (n != want_n) {
		fprintf(stderr, "FAIL: [%s] gave %d words (%s), want %d\n", input, n,
		        err != NULL ? err : "no error", want_n);
		fails++;
		return;
	}
	for (i = 0; i < n; i++) {
		if (strcmp(tokens[i], want[i]) != 0) {
			fprintf(stderr, "FAIL: [%s] word %d is [%s], want [%s]\n", input, i, tokens[i],
			        want[i]);
			fails++;
		}
	}
}

static void expect_error(const char *input)
{
	char line[512];
	char *tokens[16];
	const char *err;
	int n;

	snprintf(line, sizeof(line), "%s", input);
	n = shellwords_split(line, tokens, 16, &err);
	if (n != -1 || err == NULL) {
		fprintf(stderr, "FAIL: [%s] was not refused (n=%d)\n", input, n);
		fails++;
	}
}

int main(void)
{
	{
		const char *w[] = { "deployment", "apply", "br-2", "--secret=RIP_PASSWORD=a b'c" };
		expect("deployment apply br-2 --secret=RIP_PASSWORD='a b'\"'\"'c'\n", 4, w);
	}
	{
		const char *w[] = { "--description=two words", "x" };
		expect("--description=\"two words\" x", 2, w);
	}
	{
		const char *w[] = { "run", "--", "/bin/sh", "-c", "sleep 1" };
		expect("run -- /bin/sh -c \"sleep 1\"", 5, w);
	}
	{
		const char *w[] = { "a\"b", "c\\d", "$HOME", "e f" };
		expect("\"a\\\"b\" \"c\\\\d\" '$HOME' e\\ f", 4, w);
	}
	{
		const char *w[] = { "", "x", "" };
		expect("'' x \"\"", 3, w);
	}
	{
		const char *w[] = { "abc" };
		expect("a'b'\"c\"", 1, w);
	}
	{
		const char *w[] = { "lit\\n" };
		expect("'lit\\n'", 1, w);
	}
	expect("   \t\r\n", 0, NULL);
	expect_error("--secret=K='unclosed");
	expect_error("say \"unclosed");
	expect_error("trailing\\");
	expect_error("a b c d e f g h i j k l m n o p q");

	if (fails != 0) {
		fprintf(stderr, "SHELLWORDS: FAIL (%d)\n", fails);
		return 1;
	}
	printf("SHELLWORDS: PASS\n");
	return 0;
}
