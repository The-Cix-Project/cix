/*
 * test_listenbind -- a listener knows where it is bound, and nothing
 * else is allowed to answer that question for it (#454, ADR-0285).
 *
 * The defect this gates was not a hard one to write. Two rebind
 * functions, byte-identical apart from the conn they moved, each
 * short-circuiting on a comparison against the SHARED global
 * g_bind_addr rather than against the listener's own address:
 *
 *     if (strcmp(new_bind_addr, g_bind_addr) == 0 && ...)
 *         return 0;
 *
 * and a caller that passed g_bind_addr as new_bind_addr, making the
 * test strcmp(x, x) -- always true, always "nothing to do". The https
 * listener was therefore never rebound by a management address change.
 * It stayed bound to an address that had just been removed from the
 * interface, and only the next reboot repaired it. Measured on
 * 192.168.15.95, 2026-09-13: after a live .103 -> .95 move, http
 * answered 200 on the new address and https answered nothing anywhere.
 *
 * A runtime test cannot gate this here. test_daemon_bind_ip exists and
 * is NOT in the Makefile's SELFTESTS list, because it forks a real
 * cixd and a build container cannot -- so an assertion added there
 * would never once run. This is a source scan for the same reason
 * test_blocking_waits is: it asserts a property of the code that a
 * green build would otherwise hide.
 *
 * Two properties, both cheap and both load-bearing:
 *
 *   1. Exactly THREE places bind a listening socket. Adding a fourth is
 *      a deliberate act that edits this number in a diff -- the same
 *      instrument ADR-0224's toolchain count and ADR-0247's wait budget
 *      use, chosen because this project has repeatedly found that a
 *      prose rule does not hold and a number somebody must edit does.
 *
 *   2. The rebind guard asks the LISTENER (c->listen_addr /
 *      c->listen_port) and never the global. A guard that consults
 *      shared state is the bug itself, not a variant of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void check(int ok, const char *what)
{
	printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		g_failures++;
}

static char *slurp(const char *path, long *len_out)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long len;

	if (f == NULL) {
		fprintf(stderr, "test_listenbind: cannot open %s\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)len + 1);
	if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		fprintf(stderr, "test_listenbind: cannot read %s\n", path);
		exit(1);
	}
	buf[len] = '\0';
	fclose(f);
	*len_out = len;
	return buf;
}

/*
 * Real call sites of `needle`, ignoring comment lines and the
 * function's own definition.
 *
 * Written this way after the first draft simply counted substrings and
 * would have failed on its very first run: main.c mentions
 * create_listen_socket() in a prose comment 25,000 lines away from any
 * of them, so a naive count says 5 where the truth is 3. A gate whose
 * number moves when someone edits a comment is a gate people learn to
 * ignore.
 */
static int count_call_sites(const char *hay, const char *needle, const char *definition)
{
	int n = 0;
	const char *p = hay;

	while ((p = strstr(p, needle)) != NULL) {
		const char *line_start = p;
		const char *line_end;
		size_t line_len;
		char line[512];
		const char *t;

		while (line_start > hay && line_start[-1] != '\n')
			line_start--;
		line_end = strchr(p, '\n');
		if (line_end == NULL)
			line_end = hay + strlen(hay);
		line_len = (size_t)(line_end - line_start);
		if (line_len >= sizeof(line))
			line_len = sizeof(line) - 1;
		memcpy(line, line_start, line_len);
		line[line_len] = '\0';

		t = line;
		while (*t == ' ' || *t == '\t')
			t++;
		if (strncmp(t, "*", 1) != 0 && strncmp(t, "/*", 2) != 0 &&
		    strncmp(t, "//", 2) != 0 && strstr(line, definition) == NULL)
			n++;
		p += strlen(needle);
	}
	return n;
}

/*
 * The body of a top-level function, from its opening line to the first
 * line that is exactly "}" -- this file is formatted with tabs and
 * K&R-style top-level braces throughout, so a closing brace in column
 * zero really does end the function. Returns NULL if not found.
 */
static char *function_body(const char *src, const char *signature)
{
	const char *start = strstr(src, signature);
	const char *end;
	char *out;
	size_t n;

	if (start == NULL)
		return NULL;
	end = strstr(start, "\n}\n");
	if (end == NULL)
		return NULL;
	n = (size_t)(end - start);
	out = malloc(n + 1);
	if (out == NULL)
		exit(1);
	memcpy(out, start, n);
	out[n] = '\0';
	return out;
}

int main(void)
{
	long len;
	char *src = slurp("daemon/src/main.c", &len);
	char *body;
	int binds;

	printf("test_listenbind: a listener knows where it is bound (#454)\n");

	/*
	 * One for the definition, plus one call in each of the three
	 * functions permitted to bind. Any other number means a fourth
	 * binding path exists -- or that one of the three stopped binding,
	 * which is just as much a reason to look.
	 */
	binds = count_call_sites(src, "create_listen_socket(", "static int create_listen_socket(");
	check(binds == 3, "exactly 3 places bind a listening socket");
	if (binds != 3)
		printf("      found %d call sites of create_listen_socket() -- expected 3:\n"
		       "      rebind_listener_conn(), start_http_listener(), start_https_listener().\n"
		       "      A fourth means a listener can move without recording where it went,\n"
		       "      which is #454. Record it there too, then update this number.\n",
		       binds);

	body = function_body(src, "static int rebind_listener_conn(");
	check(body != NULL, "rebind_listener_conn() is the one rebind implementation");
	if (body != NULL) {
		check(strstr(body, "c->listen_addr") != NULL && strstr(body, "c->listen_port") != NULL,
		      "  its guard asks the listener (c->listen_addr / c->listen_port)");
		check(strstr(body, "g_bind_addr") == NULL,
		      "  and never consults g_bind_addr -- that comparison WAS the bug");
		free(body);
	}

	/* Each binder records what it bound, or the guard has nothing true
	 * to read. */
	body = function_body(src, "static int start_http_listener(");
	check(body != NULL && strstr(body, "c->listen_addr") != NULL &&
	          strstr(body, "c->listen_port") != NULL,
	      "start_http_listener() records the address and port it bound");
	free(body);

	body = function_body(src, "static int start_https_listener(");
	check(body != NULL && strstr(body, "c->listen_addr") != NULL &&
	          strstr(body, "c->listen_port") != NULL,
	      "start_https_listener() records the address and port it bound");
	free(body);

	/* A stopped listener is nowhere. Leaving a stale record is how a
	 * later rebind decides it has nothing to do. */
	body = function_body(src, "static void stop_http_listener(");
	check(body != NULL && strstr(body, "listen_addr[0] = '\\0'") != NULL,
	      "stop_http_listener() clears the record");
	free(body);

	body = function_body(src, "static void stop_https_listener(");
	check(body != NULL && strstr(body, "listen_addr[0] = '\\0'") != NULL,
	      "stop_https_listener() clears the record");
	free(body);

	free(src);
	if (g_failures > 0) {
		printf("test_listenbind: %d FAILED\n", g_failures);
		return 1;
	}
	printf("test_listenbind: all checks passed\n");
	return 0;
}
