/*
 * proctitle -- rewrite this process's argv area so /proc/<pid>/cmdline
 * reports a chosen string. See proctitle.h for why (#456).
 */
#include "proctitle.h"

#include <string.h>

/*
 * The argv strings sit contiguously at the top of the stack: argv[0]
 * first, each NUL-terminated, and the environment strings immediately
 * after the last argv string. The kernel serves /proc/<pid>/cmdline
 * from [mm->arg_start, mm->arg_end), which is exactly the span of the
 * argv strings -- so overwriting that span and NUL-padding the rest
 * changes what cmdline reports, without moving arg_end or touching the
 * environment that begins just past g_end.
 */
static char *g_start;
static char *g_end;

void proctitle_init(int argc, char **argv)
{
	if (argc < 1 || argv == NULL || argv[0] == NULL)
		return;
	g_start = argv[0];
	g_end = argv[argc - 1] + strlen(argv[argc - 1]) + 1;
}

void proctitle_set(const char *title)
{
	size_t avail, n;

	if (g_start == NULL || g_end <= g_start)
		return;
	avail = (size_t)(g_end - g_start);
	n = strlen(title);
	if (n > avail - 1)
		n = avail - 1; /* leave room for at least one terminating NUL */
	memcpy(g_start, title, n);
	memset(g_start + n, '\0', avail - n);
}
