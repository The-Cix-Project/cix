/*
 * test_blocking_waits -- the reactor's blocking-wait budget (ADR-0247).
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
 *
 * A SECOND COUNTED SET, same idea (#402/#474): a blocking wait is not
 * the only way to stop the reactor. `overlay_upperdir_size()` is an
 * nftw() walk, O(files) and unbounded, and three request paths called
 * it inline -- one of them, container stats, on every request with no
 * shortcut, on a path the dashboard polls. That is a stalled control
 * plane on a pid-1 daemon with no shell behind it, for as long as the
 * walk takes. The call sites are counted here so a fourth is a
 * deliberate act rather than an accident.
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
	 *
	 * 26 -> 23 (#399). Four waits on the console session's exec'd
	 * process are gone -- one in console_session_teardown() and three on
	 * try_console_upgrade()'s error paths -- replaced by one pidfd
	 * callback, CONN_CONSOLE_EXEC_REAP.
	 *
	 * Those four are why this comment is worth reading before raising
	 * any number here. The count was honest and the JUSTIFICATION was
	 * not: none of the four was a pidfd callback or a double-fork
	 * intermediate, they waited on a long-lived shell after a SIGKILL,
	 * and a SIGKILL to a task in uninterruptible D-state is only
	 * PENDED. So the budget passed while holding an unbounded wait on
	 * the reactor, which is precisely what it exists to prevent. It cost
	 * three stalls on 192.168.15.95 on 2026-09-10/11 -- two of nine
	 * seconds, one of 366 that ended in a hand reset.
	 *
	 * The rule the number stands for, stated so the next raise has to
	 * argue against it: a wait is in budget only if the child is
	 * ALREADY KNOWN TO HAVE EXITED (a pidfd callback) or CANNOT OUTLIVE
	 * the call by design (a double-fork intermediate, a bounded external
	 * tool). "We send it a signal first" is not in that set.
	 */
	/* 23 -> 24 (ADR-0278, #367): handle_helper_event() waits on a pidfd
	 * EPOLLIN, where the child is already gone. That is the only
	 * addition this counter sees.
	 *
	 * It was briefly set to 25, counting abandon_unwatchable_helper()'s
	 * WNOHANG wait as well -- and count_blocking_waits() skips any line
	 * containing WNOHANG, because a wait that does not wait is not what
	 * this gate is about. The too-high budget failed the build in the
	 * OTHER direction ("fewer than expected"), which is the point of
	 * checking equality rather than a ceiling: a stale allowance is
	 * room for a future blocking wait to arrive unnoticed.
	 *
	 * The three genuinely blocking waits the first draft of that
	 * primitive had were NOT in budget, this gate caught those too, and
	 * they were removed rather than accommodated. */
	/* 24 -> 23 (#410): fetch_update_image() no longer forks a curl
	 * child to wait on at all -- curlfetch_perform() is a synchronous
	 * library call, so the fork()+waitpid() pair around it had nothing
	 * left to isolate. */
	{ "daemon/src/main.c", 23, "pidfd callbacks + double-fork intermediates" },
	/* 10 -> 11 for ADR-0279's signature fetch: the artifact tier now
	 * pulls <artifact>.minisig alongside the artifact and waits for
	 * that curl. In budget for the same reason every other one in this
	 * file is -- it runs inside the FORKED fetch child (start_fetch_for
	 * has already forked by then), so no request handler and no reactor
	 * pass can reach it, and the wait it blocks is the child's own. */
	/* 9 -> 6 (#410): start_fetch_for()'s own artifact-sha, signature and
	 * main-source-tarball fetches each used to fork a curl child and
	 * wait on it (a double-fork intermediate, in budget); all three now
	 * call curlfetch_perform() directly inside the function's own
	 * already-forked child, with no inner fork left to wait on. */
	{ "daemon/src/pkg.c", 6, "build helpers and fetch intermediates; #352 dropped one (pkg_run_capture_sha256(), no more forked sha256sum), #411 dropped another (tarball_has_common_top_dir(), no more forked tar -tf), #410 dropped three more (start_fetch_for()'s curl children, replaced by in-process curlfetch_perform())" },
	{ "daemon/src/targz.c", 4, "tar/gzip pipeline, bounded by the archive" },
	{ "daemon/src/diskpart.c", 4, "sfdisk/blkid, bounded external tools" },
	{ "daemon/src/exec.c", 2, "namespace-join intermediates" },
	{ "daemon/src/diskformat.c", 2, "mkfs intermediate" },
	{ "daemon/src/websocket.c", 0, "#351: the SHA-1+base64 handshake digest moved in-process (EVP), no forked child left to wait on" },
	{ "daemon/src/opensslrun.c", 1, "openssl, bounded" },
	{ "daemon/src/kmod.c", 1, "modprobe, bounded" },
	{ "daemon/src/storagemigrate.c", 1, "double-fork intermediate" },
	{ "daemon/src/containerstoragemigrate.c", 1, "double-fork intermediate" },
	{ "src/container.c", 4, "clone3 intermediates and container_wait" },
	{ "src/container_net.c", 3, "netns helper, bounded" },
	{ "src/mountns.c", 1, "mount helper, bounded" },
};

#define BUDGET_COUNT ((int)(sizeof(g_budgets) / sizeof(g_budgets[0])))

/* The whole-daemon ceiling, so a new FILE cannot slip past the table.
 *
 * Deliberately a SECOND number rather than a sum of the table: a sum
 * would rise on its own whenever a per-file budget did, which is the
 * one thing a ceiling must not do. Raising it is a separate, visible
 * act -- 60 -> 61 here for ADR-0279's signature fetch in pkg.c, the
 * same wait the per-file entry above explains. Lowered 61 -> 58 for
 * #351/#352 (websocket.c's two forked-openssl waits and pkg.c's
 * forked-sha256sum wait), then 58 -> 57 for #411 (pkg.c's forked
 * tar -tf listing wait, replaced by libarchive's own header stream),
 * then 57 -> 53 for #410 (main.c's fetch_update_image() dropped one,
 * pkg.c's start_fetch_for() dropped three -- every remaining curl
 * subprocess across both files replaced by in-process libcurl calls
 * via curlfetch_perform()). */
#define TOTAL_ALLOWED 53

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

/*
 * Every place that walks a tree with overlay_upperdir_size(), and
 * whether it does so on the reactor. Measured 2026-09-15.
 *
 * Mentions in comments count too -- the scan is textual, and a number
 * that moved because prose moved is a number someone has to look at,
 * which is the point. Each entry below says what its count is made of.
 */
struct walk_budget {
	const char *path;
	int allowed;
	const char *why;
};

static const struct walk_budget g_walk_budgets[] = {
	{ "daemon/src/main.c", 2,
	  "one call, and it is the RIGHT one: container_disk_measure_work() runs in a "
	  "helper child (ADR-0278), off the reactor. Plus one mention in that block's own "
	  "comment." },
	{ "daemon/src/api_volume.c", 2,
	  "one call on the reactor (#402) and one comment. GET /volumes/{name}/usage walks "
	  "only as a FALLBACK, when the btrfs qgroup query fails -- so on the platform's own "
	  "substrate it does not run at all, and #161 is retiring the substrate where it "
	  "would. Contained rather than fixed, deliberately: fixing it properly needs either "
	  "a cache like the one container stats grew, or deferred HTTP responses, which this "
	  "daemon does not have." },
	{ "daemon/src/api_image.c", 1,
	  "one call on the reactor: `image gc --measure` walks every unreferenced image "
	  "version in one request. An operator-initiated, deliberately expensive call rather "
	  "than something polled, and it is bounded by what gc found -- but it is still the "
	  "reactor, and it is counted so that stays visible." },
	{ "src/overlay.c", 2, "the definition and its own comment, not calls." },
};

#define WALK_BUDGET_COUNT ((int)(sizeof(g_walk_budgets) / sizeof(g_walk_budgets[0])))

static int count_walks(const char *path)
{
	char line[4096];
	FILE *f = fopen(path, "r");
	int n = 0;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strstr(line, "overlay_upperdir_size(") != NULL)
			n++;
	}
	fclose(f);
	return n;
}

static int check_walk_budgets(void)
{
	int failures = 0;
	int i;

	for (i = 0; i < WALK_BUDGET_COUNT; i++) {
		int n = count_walks(g_walk_budgets[i].path);

		if (n < 0) {
			fprintf(stderr, "FAIL: cannot read %s\n", g_walk_budgets[i].path);
			failures++;
			continue;
		}
		if (n != g_walk_budgets[i].allowed) {
			fprintf(stderr,
			        "FAIL: %s mentions overlay_upperdir_size() %d times, expected %d.\n"
			        "      That walk is O(files) and unbounded. What the expected count "
			        "is made of: %s\n"
			        "      A NEW CALL on the reactor stalls the control plane for as long "
			        "as the tree takes to walk; measure it in a helper child instead "
			        "(container_disk_measure_work() is the worked example).\n",
			        g_walk_budgets[i].path, n, g_walk_budgets[i].allowed,
			        g_walk_budgets[i].why);
			failures++;
		}
	}
	return failures;
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

	if (check_walk_budgets() != 0)
		ok = 0;

	if (!ok)
		return 1;
	printf("test_blocking_waits: %d blocking waits across %d files, all within budget; "
	       "%d files counted for unbounded tree walks\n",
	       total, BUDGET_COUNT, WALK_BUDGET_COUNT);
	return 0;
}
