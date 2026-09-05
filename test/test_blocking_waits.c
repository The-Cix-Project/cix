/*
 * test_blocking_waits -- the reactor's blocking-wait budget (ADR-0246
 * item 4).
 *
 * cixd is single-threaded. One epoll_wait() loop serves every request,
 * every container exit, every console byte, so a blocking call anywhere
 * a request handler can reach is not a slow endpoint -- it is a total
 * control-plane outage for as long as the call lasts. Two have been
 * found the hard way, each after the fact from a wchan, each costing a
 * manual reset of a real host: #283 during a C++ package build, and
 * #294 in a console pty master.
 *
 * ADR-0246's first answer was to make the daemon RESTARTABLE rather
 * than non-blocking. That was measured on 192.168.15.95 and it did not
 * hold: the supervisor restarted the worker correctly and the worker
 * could not come up, because the whole --init-mode startup path assumes
 * a fresh kernel. A recovery mechanism that produces a worse outage
 * than the failure is not a defence, so the real work is here instead:
 * the loop should not block in the first place.
 *
 * This is a STATIC scan, and that is the point rather than a
 * limitation. The failure it guards is not a wrong value, it is a NEW
 * call site added without thought -- which no runtime test reaches,
 * because the new path is exactly the one nothing exercises yet. It is
 * the same instrument test_curl_guards (#285) and test_toolchain_policy
 * (ADR-0224) use, and for the same reason: this project has learned
 * twice that a prose rule does not hold, and that what does hold is a
 * number somebody has to edit in a diff.
 *
 * It does NOT assert that the daemon never blocks -- it cannot, and
 * claiming otherwise would be worse than not checking. It asserts that
 * the set of places that CAN block is a known, counted set, so adding
 * one is a deliberate act that shows up in review.
 *
 * Crude on purpose, in the spirit of test_api_surfaces: it looks for
 * the text, not for syntax. There is no parse to get wrong.
 */
#include <stdio.h>
#include <string.h>

/*
 * Per-file budgets, measured 2026-09-05.
 *
 * A blocking wait is waitpid()/waitid() without WNOHANG. Each file's
 * number is what it has today and why that is currently acceptable.
 * Lowering one is progress and should be done in the same commit that
 * removes the call; RAISING one means a new place the control plane can
 * stop answering, and needs a real reason in the commit that does it.
 */
struct budget {
	const char *path;
	int allowed;
	const char *why;
};

static const struct budget g_budgets[] = {
	/*
	 * The largest holder, and most of it is safe by construction:
	 * fourteen of these sit in handle_*_event() pidfd callbacks, where
	 * EPOLLIN already means the child exited, so the wait collects a
	 * zombie and returns at once. Most of the rest reap the short-lived
	 * intermediate of a double fork, which exits immediately by design.
	 */
	{ "daemon/src/main.c", 28, "pidfd callbacks + double-fork intermediates" },
	{ "daemon/src/pkg.c", 10, "build helpers and fetch intermediates" },
	{ "daemon/src/targz.c", 4, "tar/gzip pipeline, bounded by the archive" },
	{ "daemon/src/diskpart.c", 4, "sfdisk/blkid, bounded external tools" },
	{ "daemon/src/exec.c", 3, "namespace-join intermediates" },
	{ "daemon/src/diskformat.c", 2, "mkfs intermediate" },
	{ "daemon/src/websocket.c", 2, "openssl digest, bounded" },
	{ "daemon/src/opensslrun.c", 1, "openssl, bounded" },
	{ "daemon/src/kmod.c", 1, "modprobe, bounded" },
	{ "daemon/src/storagemigrate.c", 1, "double-fork intermediate" },
	{ "daemon/src/containerstoragemigrate.c", 1, "double-fork intermediate" },
	{ "src/container.c", 4, "clone3 intermediates and container_wait" },
	{ "src/container_net.c", 3, "netns helper, bounded" },
	{ "src/mountns.c", 1, "mount helper, bounded" },
};

#define BUDGET_COUNT ((int)(sizeof(g_budgets) / sizeof(g_budgets[0])))

/* The whole-daemon ceiling, so a new FILE cannot slip past the table. */
#define TOTAL_ALLOWED 65

static int is_comment(const char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	return line[0] == '*' || (line[0] == '/' && (line[1] == '*' || line[1] == '/'));
}

static int count_blocking_waits(const char *path, int *found)
{
	char line[4096];
	FILE *f = fopen(path, "r");

	*found = 0;
	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		return -1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		if (is_comment(line))
			continue;
		if (strstr(line, "WNOHANG") != NULL)
			continue;
		if (strstr(line, "waitpid(") != NULL || strstr(line, "waitid(") != NULL)
			(*found)++;
	}
	fclose(f);
	return 0;
}

int main(void)
{
	int i, ok = 1, total = 0;

	for (i = 0; i < BUDGET_COUNT; i++) {
		int found = 0;

		if (count_blocking_waits(g_budgets[i].path, &found) != 0) {
			ok = 0;
			continue;
		}
		total += found;
		if (found == g_budgets[i].allowed)
			continue;
		ok = 0;
		if (found > g_budgets[i].allowed) {
			fprintf(stderr,
			        "FAIL: %s has %d blocking waits, budget is %d.\n"
			        "      A blocking wait in a path a request handler can reach stops the\n"
			        "      whole control plane for as long as the child runs -- cixd has one\n"
			        "      event loop and no threads. If the new one is genuinely bounded\n"
			        "      (a pidfd callback whose child has already exited, or the\n"
			        "      intermediate of a double fork), raise this number and say so.\n"
			        "      Otherwise make it asynchronous: fork, register the pidfd with\n"
			        "      epoll, and finish the work in the callback -- register_container_\n"
			        "      pidfd() and the CONN_PKG_FETCH family are the pattern.\n"
			        "      (%s)\n",
			        g_budgets[i].path, found, g_budgets[i].allowed, g_budgets[i].why);
		} else {
			fprintf(stderr,
			        "FAIL: %s has %d blocking waits, budget is %d -- fewer than expected.\n"
			        "      That is progress, so lower the budget in this same commit to\n"
			        "      lock it in. A budget nobody tightens is a ceiling that only ever\n"
			        "      rises.\n",
			        g_budgets[i].path, found, g_budgets[i].allowed);
		}
	}

	if (total != TOTAL_ALLOWED) {
		fprintf(stderr,
		        "FAIL: %d blocking waits across the tracked files, ceiling is %d.\n"
		        "      If a NEW file grew one, add it to g_budgets[] with its reason --\n"
		        "      the per-file table is what makes this reviewable, and a file that\n"
		        "      is not in it is not being watched at all.\n",
		        total, TOTAL_ALLOWED);
		ok = 0;
	}

	if (!ok)
		return 1;
	printf("test_blocking_waits: %d blocking waits across %d files, all within budget\n",
	       total, BUDGET_COUNT);
	return 0;
}
