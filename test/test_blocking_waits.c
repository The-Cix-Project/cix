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
 * the only way to stop the reactor. `treesize_bytes()` is an
 * nftw() walk, O(files) and unbounded, and three request paths called
 * it inline -- one of them, container stats, on every request with no
 * shortcut, on a path the dashboard polls. That is a stalled control
 * plane on a pid-1 daemon with no shell behind it, for as long as the
 * walk takes. The call sites are counted here so a fourth is a
 * deliberate act rather than an accident.
 */
#include <dirent.h>
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
	/* 23 -> 24 (ADR-0307 clause 3): handle_pkg_unpack_event() reaps
	 * the forked `cbs extract`. In budget under the FIRST category
	 * this table names rather than by an argument -- it is a pidfd
	 * callback, so EPOLLIN has already said the child exited and the
	 * wait collects a zombie and returns at once. The asynchrony
	 * that made this callback exist is the whole reason clause 3 is
	 * shaped the way it is: the alternative was waiting for the
	 * extraction itself, on the reactor. */
	/* 24 -> 23 (#464): run_cmd() is gone. It waited on a forked
	 * `cp --reflink=auto -a` of an entire image rootfs, from the request
	 * that creates a userns container on a non-btrfs host -- an unbounded
	 * wait on the reactor, the kind this table exists to exclude, counted
	 * here under a justification it never met. The copy is in-process now
	 * (cix_tree_copy()). */
	/* 23 -> 24 (ADR-0356, #617): handle_pkg_produce_event() reaps an
	 * install's image producer. A pidfd callback, the first category
	 * above: EPOLLIN has already said the child exited. The production
	 * itself, which held the loop up to 1.2 s, moved into that child. */
	{ "daemon/src/main.c", 23, "pidfd callbacks + double-fork intermediates; 24 -> 23 when ADR-0360 removed migrate-storage and its event" },
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
	/*
	 * 6 -> 7 (ADR-0305): run_cbs_explain() waits for `cbs explain
	 * --json`, and this is the argument the comment above demands of
	 * any raise, against the rule it states.
	 *
	 * In budget as a BOUNDED EXTERNAL TOOL, and bounded by construction
	 * rather than by expectation. `explain` parses one local file and
	 * writes JSON to a pipe: it opens no socket (CBS fetches only in
	 * `build`, over a libcurl it dlopen()s there), takes no lock, forks
	 * nothing, and waits on nothing. Its runtime is a function of a
	 * recipe's size.
	 *
	 * The one way it could have outlived the call is the reason to read
	 * the code and not just this sentence: if the reader stopped
	 * draining stdout while the child was still writing, the child
	 * would block on a full pipe forever and so would the wait. That is
	 * why the read end is CLOSED before the wait on every path,
	 * including the truncation path -- a child writing into a closed
	 * pipe dies rather than blocking. The first draft returned early
	 * from truncation with a second waitpid() of its own; it was
	 * restructured to a single wait precisely so this entry has one
	 * thing to justify instead of two.
	 *
	 * What it is NOT: a wait after a signal. This is the pattern the
	 * comment above names as out of budget, and it cost 366 seconds and
	 * a hand reset on 192.168.15.95. Nothing here signals the child.
	 *
	 * The same wait now serves `cbs revise` too (ADR-0323, the author
	 * stage): the function is run_cbs(), and run_cbs_explain() is a
	 * wrapper over it, so the count does not move. The bound is the
	 * same one: revise reads one local recipe and writes the revised
	 * bytes to stdout, and it fetches nothing (cix-build-system
	 * src/main.c revise_file(), read at tag v0.1.102).
	 */
	/*
	 * 7 -> 8 (ADR-0307 clause 7, #496): cbs_engine_version() waits for
	 * `cbs --version`, and here is the argument this table demands.
	 *
	 * Bounded by construction, and more narrowly than `cbs explain`
	 * above: `--version` is a single printf of a compile-time constant
	 * followed by `return 0` (cix-build-system src/main.c:664, read at
	 * tag v0.1.29). It opens no file, no socket and no lock, and its
	 * runtime is not a function of any input -- there is no input. The
	 * read end is drained and then CLOSED before the wait, on every
	 * path including the early break when the buffer fills -- so the
	 * full-pipe case the explain entry describes cannot arise: a child
	 * still writing dies on EPIPE rather than blocking forever with
	 * the reader gone. In practice the output is one short line.
	 *
	 * And it is not on the reactor at all. It runs from pkg_init(),
	 * which is startup -- before the epoll loop exists, so no request
	 * handler and no reactor pass can reach it, exactly once per boot.
	 * That is a stronger position than most entries in this table,
	 * which have to argue about handlers that CAN reach them.
	 *
	 * What it is NOT: a wait after a signal. Nothing here signals the
	 * child.
	 *
	 * The re-derivations the sweep then performs add no wait of their
	 * own -- they go through pkg_recipe_rederive_identity(), whose
	 * run_cbs_explain() is the wait the entry below already counts.
	 */
	/*
	 * 8 -> 9 (ADR-0307 clause 3): cixpkg_unpack_start()'s
	 * pidfd_open-failure cleanup.
	 *
	 * This one has to be argued honestly, because it IS a wait after
	 * a signal -- the shape the comment at the top of this table
	 * names as out of budget, after it cost 366 seconds and a hand
	 * reset. The reason it is nonetheless in budget is the state of
	 * the child, not the presence of the kill: it is reached only
	 * when pidfd_open() fails on a process forked three lines
	 * earlier, which has at most just execve()d `cbs extract` and
	 * has opened nothing, taken no lock and touched no network. The
	 * case that rule was written for was a SIGKILL to a long-lived
	 * shell already in uninterruptible D-state.
	 *
	 * It is also the convention every other fork site in this file
	 * already uses for exactly this failure (start_fetch_for's own
	 * pidfd_open path is the one it was copied from), and the
	 * alternative is worse: killing without reaping leaks a zombie
	 * into a daemon that is pid 1 on an installed host.
	 *
	 * The extraction ITSELF adds no wait -- that is the whole point
	 * of the clause. The child is tracked by pidfd and reaped by
	 * handle_pkg_unpack_event() on EPOLLIN, where the process is
	 * already gone.
	 */
	{ "daemon/src/pkg.c", 10, "build helpers and fetch intermediates, plus #618's child_exit_waiter() (a waiter forked a moment ago and SIGKILLed when its pidfd cannot be opened, reaped at once as the producer beside it, image_version_fork(), already is -- ADR-0356 renamed it from buildenv_wait_for()), plus ADR-0305's `cbs explain` (a bounded parse of a local file, its pipe closed before the wait); #352 dropped one (pkg_run_capture_sha256(), no more forked sha256sum), #411 dropped another (tarball_has_common_top_dir(), no more forked tar -tf), #410 dropped three more (start_fetch_for()'s curl children, replaced by in-process curlfetch_perform())" },
	{ "daemon/src/diskpart.c", 4, "sfdisk/blkid, bounded external tools" },
	{ "daemon/src/exec.c", 2, "namespace-join intermediates" },
	{ "daemon/src/diskformat.c", 2, "mkfs intermediate" },
	{ "daemon/src/websocket.c", 0, "#351: the SHA-1+base64 handshake digest moved in-process (EVP), no forked child left to wait on" },
	{ "daemon/src/pkicrypto.c", 0, "#351: the PKI, release-key and signing-key work moved in-process to libcrypto; opensslrun.c and its one bounded wait on a forked openssl are gone" },
	{ "daemon/src/kmod.c", 1, "modprobe, bounded" },
	{ "daemon/src/storagemigrate.c", 1, "double-fork intermediate" },
	{ "src/container.c", 5,
	  "clone3 intermediates, container_wait, and the reap of a child that wrote its "
	  "diagnostic and is already exiting before exec (#622)" },
	/* 3 -> 4 (#447): container_net_apply_sysctls_running() waits for its
	 * setns() helper, which writes at most CONTAINER_MAX_SYSCTLS files under
	 * /proc/sys/net and exits -- the same bounded shape as the other three. */
	{ "src/container_net.c", 3, "netns helpers, bounded" },
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
 * via curlfetch_perform()). Then 53 -> 54 for ADR-0305's
 * run_cbs_explain() in pkg.c, the same single wait the per-file entry
 * above argues for. Then 54 -> 55 for ADR-0307 clause 7's
 * cbs_engine_version(), also in pkg.c, also a single wait and argued
 * for in the same place. Then 55 -> 56 for clause 3's
 * cixpkg_unpack_start(), again in pkg.c and again argued in the
 * per-file entry -- the extraction it starts adds none of its own,
 * being pidfd-tracked. Then 56 -> 57 for the other half of the same
 * clause: handle_pkg_unpack_event() in main.c, a pidfd callback whose
 * child is already gone. Then 57 -> 58 for #447's
 * container_net_apply_sysctls_running() in src/container_net.c, the
 * bounded setns() helper its per-file entry above argues for. Then
 * 58 -> 57 for #464: main.c's run_cmd(), an unbounded wait on a
 * whole-rootfs `cp`, removed along with the fork. Then 57 -> 56 for
 * #351: opensslrun.c's wait on a forked openssl, gone with the file.
 * Then 56 -> 52 for ADR-0328: targz.c's four waits on its tar and gzip
 * children, gone with the file and the tarball it wrote.
 * Then 52 -> 51 for #344/ADR-0335 step 2: container_net.c's wait on the
 * interface bring-up helper, which held the reactor for 1658 ms of
 * rtnl_link_set_up() on a radio. The helper still runs; the daemon
 * reaps it through its own epoll instead of standing still for it.
 * Then 51 -> 52 for #618: buildenv_wait_for() in pkg.c (child_exit_waiter() since ADR-0356), the per-file
 * entry's bounded reap of a waiter killed because it cannot be watched.
 * Then 52 -> 53 for #622: container_create()'s reap of a child that wrote
 * its diagnostic before exec. The diagnostic is read to EOF first, and
 * EOF is the child's exit, so the wait reaps a process already gone.
 * Then 53 -> 54 for ADR-0356: handle_pkg_produce_event() in main.c, a
 * pidfd callback whose child is already gone. pkg.c's two waits moved
 * with the code they belong to (image_version_fork(), child_exit_waiter())
 * and its count did not change.
 * Then 54 -> 52 for ADR-0360: containerstoragemigrate.c and its
 * double-fork intermediate are gone, and with them main.c's reap of the
 * migration job (handle_container_storage_migrate_event()). */
#define TOTAL_ALLOWED 52

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
 * Every place that walks a tree with treesize_bytes(), and
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
	{ "daemon/src/treesize.c", 1, "the definition, not a call (it was src/overlay.c's overlay_upperdir_size() until ADR-0360, cix#658)." },
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
		if (strstr(line, "treesize_bytes(") != NULL)
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
			        "FAIL: %s mentions treesize_bytes() %d times, expected %d.\n"
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

/*
 * A THIRD COUNTED SET (ADR-0340, #609): checking a credential.
 *
 * One bcrypt comparison at PWHASH_BCRYPT_COST was measured at ~350 ms on
 * 192.168.15.95 on 2026-10-18, and a login with LDAP enabled can wait out
 * three servers' timeouts before it. Both used to run on the loop, so
 * every request on the box waited behind every login. ADR-0340 moved the
 * comparison and the bind into credcheck_work(), which runs only in a
 * helper child -- and a prose rule that they stay there would not hold,
 * so they are counted.
 *
 * Unlike the two sets above this one scans EVERY daemon source file,
 * because the failure it guards is a new file growing a call: a budget
 * table that lists only the files that have one cannot see that. A file
 * not named below is budgeted at zero. Comment lines are skipped, so
 * prose about these functions moves no number.
 */
struct call_budget {
	const char *file; /* basename under daemon/src/ */
	const char *call;
	int allowed;
	const char *why;
};

static const struct call_budget g_cred_budgets[] = {
	{ "credcheck.c", "pwhash_bcrypt_check(", 2,
	  "the local password and the app passwords, in credcheck_work(), which runs in a "
	  "helper child" },
	{ "credcheck.c", "ldapclient_bind(", 1, "a login's LDAP attempt, in the same helper child" },
	{ "pwhash.c", "pwhash_bcrypt_check(", 1, "the definition, not a call" },
	{ "pwhash.c", "pwhash_bcrypt_new(", 1, "the definition, not a call" },
	{ "ldapclient.c", "ldapclient_bind(", 1, "the definition, not a call" },
	/*
	 * Phase 2 (#610) moved every request path off the loop: a password set
	 * through the API and an app password created are made here, in the
	 * helper child. 2 -> 0 in ldap.c, where those two used to be made.
	 */
	{ "credcheck.c", "pwhash_bcrypt_new(", 1,
	  "a password being set, and an app password's secret, in the helper child" },
	/*
	 * The two that remain on the loop have no request to park -- #611
	 * records both, and what would remove them. They are budgeted so a
	 * third is a number someone has to raise.
	 */
	{ "ldap.c", "pwhash_bcrypt_new(", 1,
	  "the service bind account's secret, when it is (re)provisioned -- ~350 ms on the loop "
	  "(#611)" },
	{ "main.c", "pwhash_bcrypt_new(", 1,
	  "a container's ldap_provision secret, at every start -- ~350 ms on the loop, and the "
	  "secret must reach the container before its services start (#611)" },
};

static const char *const g_cred_calls[] = {
	"pwhash_bcrypt_check(",
	"ldapclient_bind(",
	"pwhash_bcrypt_new(",
	NULL,
};

/*
 * A FOURTH COUNTED SET (ADR-0341, #608): copying or re-owning a whole
 * tree. A container's rootfs used to be copied and chowned on the loop
 * at every roll of a userns container whose image was on another
 * filesystem -- 4-5 s on every hourly roll of `jump` on 192.168.15.95,
 * measured 2026-10-18. ADR-0341 made the rootfs a snapshot beside its
 * image, so a btrfs host never copies; these count what is left, so a
 * new copy on the loop is a number someone has to raise. Same
 * mechanism as the credential set: every daemon source, comments
 * skipped, a file not named here budgeted at zero.
 */
static const struct call_budget g_copy_budgets[] = {
	/* main.c had one cix_tree_copy( and two chown_tree( for the userns
	 * copy+chown seed where rebuildable storage was not btrfs. ADR-0360
	 * removed that path, so main.c is budgeted at zero for both (#614). */
	{ "volume.c", "chown_tree(", 3,
	  "the definition, its recursion, and /volumes/{volume_name}/owner with recursive set: an "
	  "operator-initiated walk of the volume, on the loop, counted so it stays visible" },
};

static const char *const g_copy_calls[] = {
	"cix_tree_copy(",
	"chown_tree(",
	NULL,
};

/*
 * A FIFTH COUNTED SET (ADR-0342, #612): reading the log store. A filtered
 * read that finds fewer than `tail` matches reads every segment -- 1533
 * ms of loop time for a regex matching nothing on 192.168.15.95, measured
 * 2026-10-18, against a store the operator may size to 100 GB. A read
 * with no filter costs O(tail), because segments are read backwards, and
 * is the only kind answered on the loop; logread_query_bounded() is what
 * decides, in the one handler that calls logread_run() directly. Any
 * other read runs in a helper child, through logread_write_to().
 */
static const struct call_budget g_logread_budgets[] = {
	{ "logstore.c", "logstore_tail_ex(", 1, "the definition, not a call" },
	{ "logread.c", "logstore_tail_ex(", 1, "logread_run(), its one caller" },
	{ "logread.c", "logread_run(", 2,
	  "the definition, and logread_write_to(), which runs only in a helper child" },
	{ "api_logs.c", "logread_run(", 1,
	  "handle_logs_get()'s read with no filter, O(tail), after logread_query_bounded()" },
	{ "logread.c", "logread_write_to(", 1, "the definition, not a call" },
	{ "main.c", "logread_write_to(", 1, "logread_work(), the helper child of a filtered read" },
};

static const char *const g_logread_calls[] = {
	"logstore_tail_ex(",
	"logread_run(",
	"logread_write_to(",
	NULL,
};

/*
 * A SIXTH COUNTED SET (#630): making a system backup. do_system_backup()
 * walks the whole recipe store. When it read every recipe -- 1620 of
 * them, 13.2 MB of an 18.5 MB bundle on 192.168.15.95 -- it held the
 * loop 0.45-0.5 s idle and 1168 ms under a build; since ADR-0351 it
 * reads only those no source owns, a set only the operator bounds.
 * Its one caller is backup_work(), the helper
 * child that GET /v1/system/backup, the snapshot-now request and the
 * system.backup schedule all reach through backup_ask() or
 * backup_submit_daemon_job().
 */
static const struct call_budget g_backup_budgets[] = {
	{ "main.c", "do_system_backup(", 2,
	  "the definition, and backup_work(), which runs only in a helper child" },
};

static const char *const g_backup_calls[] = {
	"do_system_backup(",
	NULL,
};

/*
 * A SEVENTH COUNTED SET (#631): staging a boot slot. do_system_update()
 * copies a root image onto the inactive partition (a kernel-only update
 * copies the whole active partition), writes a kernel onto the ESP and
 * fsyncs both. Measured on 192.168.15.95, 2026-10-21, while it ran in the
 * POST /v1/system/update handler: 946 ms of event loop for a
 * 13,365,248-byte root. Its callers are update_stage(), in the helper
 * child that a request and the host roll both reach through
 * update_submit() or update_submit_daemon_job(), and main()'s
 * --test-update-image= self-test, which runs before there is a loop.
 */
/*
 * The fetch of a boot image is in the same set (#633): fetch_update_image()
 * is bounded only by the shared curl guards, two attempts of up to 20 s
 * connect plus 60 s silence. Its callers are update_stage() and
 * bootmgr_stage(), each run only in its kind's helper child; the
 * boot-manager install fetched on the loop until #633.
 */
static const struct call_budget g_update_budgets[] = {
	{ "main.c", "do_system_update(", 3,
	  "the definition, update_stage() in a helper child, and main()'s --test-update-image= "
	  "self-test, before the loop" },
	{ "main.c", "fetch_update_image(", 3,
	  "the definition, and update_stage() and bootmgr_stage(), each in a helper child" },
};

static const char *const g_update_calls[] = {
	"do_system_update(",
	"fetch_update_image(",
	NULL,
};

/*
 * AN EIGHTH COUNTED SET (ADR-0356, #617): producing an image version.
 * The snapshot, the merge or unlink of a package's files and the
 * placement cost as long as the files take: hibr's upgrade in jumpbox
 * held the loop 862 ms on 192.168.15.95 on 2026-10-23, 834 of them in
 * 203 unlinks. image_version_build() does that work, and its one caller
 * outside image_produce_new_version() is image_version_fork()'s child.
 * image_produce_new_version() is the rest: both halves in the daemon,
 * for the callers that have not moved.
 */
static const struct call_budget g_produce_budgets[] = {
	{ "pkg.c", "image_version_build(", 3,
	  "the definition, image_produce_new_version(), and image_version_fork()'s child" },
	{ "pkg.c", "image_produce_new_version(", 3,
	  "the definition; the toolchain bootstrap and the sandbox migration at start, which "
	  "produce cix-builder. The bootstrap refuses while a child is producing the same image" },
	{ "pkg.c", "image_version_fork(", 4,
	  "the definition, a build environment's composer, an install's producer and an "
	  "uninstall's (ADR-0357)" },
};

static const char *const g_produce_calls[] = {
	"image_version_build(",
	"image_produce_new_version(",
	"image_version_fork(",
	NULL,
};

/*
 * A NINTH COUNTED SET (#643): verifying the installed packages.
 * pkg_write_verify_json() stats every recorded file of every installed
 * package and reads every installed header: 8581 ms of loop on
 * 192.168.15.95 on 2026-10-25, with 310 packages. Its one caller is
 * verify_work(), the helper child GET /v1/pkg/verify reaches through
 * verify_ask().
 */
static const struct call_budget g_verify_budgets[] = {
	{ "pkg.c", "pkg_write_verify_json(", 1, "the definition, not a call" },
	{ "main.c", "pkg_write_verify_json(", 1, "verify_work(), which runs only in a helper child" },
};

static const char *const g_verify_calls[] = {
	"pkg_write_verify_json(",
	NULL,
};

/*
 * A TENTH COUNTED SET (#249): reporting the data directory.
 * datadir_write_json() walks every tree beneath it, unbounded. Its one
 * caller is datadir_work(), the helper child GET /v1/system/data-dir
 * reaches through datadir_ask().
 */
static const struct call_budget g_datadir_budgets[] = {
	{ "datadir.c", "datadir_write_json(", 1, "the definition, not a call" },
	{ "main.c", "datadir_write_json(", 1, "datadir_work(), which runs only in a helper child" },
};

static const char *const g_datadir_calls[] = {
	"datadir_write_json(",
	NULL,
};

struct call_set {
	const char *what;
	const struct call_budget *budgets;
	int count;
	const char *const *calls;
	const char *advice; /* why a new call is a stall, and what to do instead */
};

static const struct call_set g_call_sets[] = {
	{ "credential", g_cred_budgets, (int)(sizeof(g_cred_budgets) / sizeof(g_cred_budgets[0])),
	  g_cred_calls,
	  "Checking a credential costs ~350 ms of bcrypt, or up to three LDAP timeouts, and cixd "
	  "has one event loop. Make the check through credcheck_ask() (ADR-0340), which parks the "
	  "request and runs it in a helper child, rather than calling it here." },
	{ "tree copy", g_copy_budgets, (int)(sizeof(g_copy_budgets) / sizeof(g_copy_budgets[0])),
	  g_copy_calls,
	  "Copying or re-owning a tree costs as long as the tree is big, and cixd has one event "
	  "loop. A container's rootfs is a snapshot of its image (ADR-0341), never a copy; any "
	  "other tree is copied in a helper_run() child." },
	{ "log read", g_logread_budgets,
	  (int)(sizeof(g_logread_budgets) / sizeof(g_logread_budgets[0])), g_logread_calls,
	  "A filtered log read can read the whole store, which the operator may size to 100 GB, "
	  "and cixd has one event loop. Ask for it through logread_ask() (ADR-0342), which parks "
	  "the request and reads in a helper child; only a read with no filter runs here." },
	{ "system backup", g_backup_budgets,
	  (int)(sizeof(g_backup_budgets) / sizeof(g_backup_budgets[0])), g_backup_calls,
	  "A system backup walks the whole recipe store, and cixd has one event loop. Ask for one "
	  "through backup_ask() or backup_submit_daemon_job() (#630), which make it in a helper "
	  "child." },
	{ "system update", g_update_budgets,
	  (int)(sizeof(g_update_budgets) / sizeof(g_update_budgets[0])), g_update_calls,
	  "Staging a slot copies a whole root image and a kernel and fsyncs both, and fetching a "
	  "boot image can take minutes, and cixd has one event loop. Ask for a slot through "
	  "update_submit() or update_submit_daemon_job() (#631) and for a boot manager through "
	  "bootmgr_submit() (#633), which do the work in a helper child." },
	{ "image version", g_produce_budgets,
	  (int)(sizeof(g_produce_budgets) / sizeof(g_produce_budgets[0])), g_produce_calls,
	  "Producing an image version snapshots it and merges or unlinks a package's files, which "
	  "costs as long as the files take, and cixd has one event loop. Produce it through "
	  "image_version_fork() (ADR-0356), which builds it in a helper child, and record it with "
	  "image_version_commit() when the child has exited." },
	{ "package verification", g_verify_budgets,
	  (int)(sizeof(g_verify_budgets) / sizeof(g_verify_budgets[0])), g_verify_calls,
	  "Verifying the installed packages stats every file of every one of them, and cixd has "
	  "one event loop. Ask for it through verify_ask() (#643), which parks the request and "
	  "makes the report in a helper child." },
	{ "data-directory report", g_datadir_budgets,
	  (int)(sizeof(g_datadir_budgets) / sizeof(g_datadir_budgets[0])), g_datadir_calls,
	  "Reporting the data directory walks every tree beneath it, and cixd has one event loop. "
	  "Ask for it through datadir_ask() (#249), which parks the request and makes the report "
	  "in a helper child." },
};

#define CALL_SET_COUNT ((int)(sizeof(g_call_sets) / sizeof(g_call_sets[0])))

static int call_allowed(const struct call_set *s, const char *file, const char *call)
{
	int i;

	for (i = 0; i < s->count; i++) {
		if (strcmp(s->budgets[i].file, file) == 0 && strcmp(s->budgets[i].call, call) == 0)
			return i;
	}
	return -1;
}

static int count_calls(const char *path, const char *call)
{
	char line[4096];
	FILE *f = fopen(path, "r");
	int n = 0;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (!is_comment(line) && strstr(line, call) != NULL)
			n++;
	}
	fclose(f);
	return n;
}

static int check_call_set(const struct call_set *s, int *out_files)
{
	DIR *d = opendir("daemon/src");
	struct dirent *de;
	int failures = 0, seen[32];
	int i;

	*out_files = 0;
	memset(seen, 0, sizeof(seen));
	if (s->count > (int)(sizeof(seen) / sizeof(seen[0]))) {
		fprintf(stderr, "FAIL: the %s set has more rows than check_call_set() tracks\n",
		        s->what);
		return 1;
	}
	if (d == NULL) {
		fprintf(stderr, "FAIL: cannot list daemon/src\n");
		return 1;
	}
	while ((de = readdir(d)) != NULL) {
		size_t len = strlen(de->d_name);
		char path[512];
		const char *const *c;

		if (len < 3 || strcmp(de->d_name + len - 2, ".c") != 0)
			continue;
		snprintf(path, sizeof(path), "daemon/src/%s", de->d_name);
		(*out_files)++;
		for (c = s->calls; *c != NULL; c++) {
			int n = count_calls(path, *c);
			int b = call_allowed(s, de->d_name, *c);
			int allowed = b >= 0 ? s->budgets[b].allowed : 0;

			if (b >= 0)
				seen[b] = 1;
			if (n < 0) {
				fprintf(stderr, "FAIL: cannot read %s\n", path);
				failures++;
				continue;
			}
			if (n == allowed)
				continue;
			failures++;
			if (n > allowed)
				fprintf(stderr,
				        "FAIL: %s has %d lines calling %s, budget is %d.\n"
				        "      %s%s%s\n",
				        path, n, *c, allowed, s->advice, b >= 0 ? "\n      Budgeted: " : "",
				        b >= 0 ? s->budgets[b].why : "");
			else
				fprintf(stderr,
				        "FAIL: %s has %d lines calling %s, budget is %d -- fewer than "
				        "expected. Lower the budget in this same commit to lock it in.\n",
				        path, n, *c, allowed);
		}
	}
	closedir(d);
	for (i = 0; i < s->count; i++) {
		if (!seen[i]) {
			fprintf(stderr, "FAIL: %s budget names daemon/src/%s, which does not exist -- "
			                "remove the row with the file\n",
			        s->what, s->budgets[i].file);
			failures++;
		}
	}
	return failures;
}

int main(void)
{
	int i, ok = 1, total = 0, source_files = 0;

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
	for (i = 0; i < CALL_SET_COUNT; i++)
		if (check_call_set(&g_call_sets[i], &source_files) != 0)
			ok = 0;

	if (!ok)
		return 1;
	printf("test_blocking_waits: %d blocking waits across %d files, all within budget; "
	       "%d files counted for unbounded tree walks; %d daemon sources counted for "
	       "credential checks and tree copies\n",
	       total, BUDGET_COUNT, WALK_BUDGET_COUNT, source_files);
	return 0;
}
