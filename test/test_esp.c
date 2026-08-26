/*
 * Issue #128: proves the ESP boot-configuration resource over real HTTP.
 *
 * The fixture is deliberately the exact state a real host was found in:
 * a loader.conf whose `default` is a glob pattern left over from an
 * earlier naming scheme, six stale entries it still matches, and one
 * correctly-staged new entry it does not. That host could not complete
 * an A/B update and nothing anywhere reported an error -- the update
 * succeeded, the entry was written correctly, and the machine rebooted
 * into what it was already running.
 *
 * So the first thing asserted here is the diagnostic, not the fix:
 * selected_entry must name the stale entry the firmware would really
 * boot, and the staged entry must be visibly not matched. Then the
 * repair, and then the two guards that stop a remote operator from
 * bricking a machine they cannot physically reach.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7689
#define PORT_ARG "--port=7689"

static char g_data_dir[PATH_MAX];
static char g_esp_dir[PATH_MAX];
static char g_entries_dir[PATH_MAX];
static char g_loader_conf[PATH_MAX];
static int g_failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	printf("FAIL: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	g_failures++;
}

static int run_cmd(const char *fmt, ...)
{
	char cmd[2048];
	va_list ap;
	int rc;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	rc = system(cmd);
	return (rc == 0) ? 0 : -1;
}

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;
	struct cix_response r;

	for (i = 0; i < max_attempts; i++) {
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[6];
	static char data_dir_arg[PATH_MAX + 12];
	static char esp_arg[PATH_MAX + 24];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	snprintf(esp_arg, sizeof(esp_arg), "--test-esp-entries-dir=%s", g_entries_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = esp_arg;
	/* A real installed host knows which slot it booted; without this the
	 * running-slot guard has nothing to protect. */
	dargv[4] = "--slot=a";
	dargv[5] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
		_exit(127);
	}
	return pid;
}

static int stop_daemon(pid_t pid)
{
	int status;

	kill(pid, SIGTERM);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* One loader entry, in systemd-boot's own format. */
static int write_entry(const char *name, const char *slot, const char *init_path)
{
	char path[PATH_MAX];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s.conf", g_entries_dir, name);
	f = fopen(path, "w");
	if (f == NULL) {
		perror(path);
		return -1;
	}
	fprintf(f,
	        "title Cix (%s)\nlinux /cix-bzImage-%s\noptions console=tty0 root=/dev/vda%s rw "
	        "init=%s -- --init-mode --slot=%s\n",
	        name, slot, strcmp(slot, "a") == 0 ? "2" : "3", init_path, slot);
	fclose(f);
	return 0;
}

/*
 * The real-world fixture: an old naming scheme still winning. Note the
 * two directives this module has no opinion about -- they exist to
 * prove a loader.conf rewrite preserves what it does not understand,
 * rather than regenerating the file from a template and silently
 * dropping settings.
 */
static int seed_esp(void)
{
	FILE *f;

	if (run_cmd("mkdir -p '%s'", g_entries_dir) != 0)
		return -1;
	f = fopen(g_loader_conf, "w");
	if (f == NULL) {
		perror(g_loader_conf);
		return -1;
	}
	fprintf(f, "default thinc-*\ntimeout 3\nconsole-mode keep\neditor no\n");
	fclose(f);

	if (write_entry("thinc-a", "a", "/bin/thincd") != 0 ||
	    write_entry("thinc-b", "b", "/bin/thincd") != 0 ||
	    write_entry("thinc-a+3", "a", "/bin/thincd") != 0 ||
	    write_entry("thinc-b+3", "b", "/bin/thincd") != 0 ||
	    write_entry("cix-b+3", "b", "/bin/cixd") != 0)
		return -1;
	return 0;
}

static const struct json_value *entry_named(const struct json_value *root, const char *name)
{
	const struct json_value *entries = json_object_get(root, "entries");
	size_t i;

	if (entries == NULL || entries->type != JSON_ARRAY)
		return NULL;
	for (i = 0; i < entries->u.array.count; i++) {
		const char *n = json_as_string(json_object_get(entries->u.array.items[i], "name"));

		if (n != NULL && strcmp(n, name) == 0)
			return entries->u.array.items[i];
	}
	return NULL;
}

static int bool_field(const struct json_value *obj, const char *key)
{
	const struct json_value *v = json_object_get(obj, key);

	return (v != NULL && v->type == JSON_BOOL && v->u.boolean) ? 1 : 0;
}

int main(void)
{
	struct cix_client c;
	pid_t daemon_pid;
	struct cix_response r;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0) {
		fprintf(stderr, "could not create test data dir\n");
		return 1;
	}
	snprintf(g_esp_dir, sizeof(g_esp_dir), "%s/esp", g_data_dir);
	snprintf(g_entries_dir, sizeof(g_entries_dir), "%s/loader/entries", g_esp_dir);
	snprintf(g_loader_conf, sizeof(g_loader_conf), "%s/loader/loader.conf", g_esp_dir);
	if (seed_esp() != 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	cix_client_init(&c, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&c, 100) != 0) {
		fprintf(stderr, "daemon did not become ready\n");
		stop_daemon(daemon_pid);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* ---- the diagnostic: issue #128, made visible ---- */
	if (cix_client_request(&c, "GET", "/v1/system/esp", NULL, &r) == 0 && r.json != NULL) {
		const char *def = json_as_string(json_object_get(r.json, "default"));
		const char *selected = json_as_string(json_object_get(r.json, "selected_entry"));
		const struct json_value *staged = entry_named(r.json, "cix-b+3.conf");
		const struct json_value *stale = entry_named(r.json, "thinc-a.conf");

		if (!bool_field(r.json, "present"))
			fail("ESP reported absent with a real loader directory present");
		if (def == NULL || strcmp(def, "thinc-*") != 0)
			fail("default pattern: got %s, expected thinc-*", def != NULL ? def : "(null)");
		/*
		 * The whole bug in one field: the firmware boots a stale entry.
		 *
		 * And specifically a slot-A one. The real host this fixture
		 * copies boots slot A, so an implementation taking the LAST
		 * match in sort order instead of the FIRST names a slot-B
		 * entry here -- confidently wrong about the one thing an
		 * operator reads this field for. Not hypothetical: that is
		 * what the first version of this did, caught only by checking
		 * the answer against the real machine.
		 */
		if (selected == NULL || strncmp(selected, "thinc-a", 7) != 0)
			fail("selected_entry: got %s, expected a slot-A thinc-* entry (the slot the "
			     "real host actually boots)",
			     selected != NULL ? selected : "(null)");
		if (staged == NULL)
			fail("the staged cix-b+3.conf entry is missing from the listing");
		else if (bool_field(staged, "matches_default"))
			fail("the staged entry must NOT match a thinc-* default");
		if (stale == NULL || !bool_field(stale, "matches_default"))
			fail("the stale thinc-a.conf entry should match the thinc-* default");
		if (stale != NULL && !bool_field(stale, "is_running_slot"))
			fail("thinc-a.conf should be flagged as the running slot's entry");
		cix_response_free(&r);
	} else {
		fail("GET /v1/system/esp failed");
	}

	/* ---- a default matching nothing is refused, not warned about ---- */
	if (cix_client_request(&c, "PUT", "/v1/system/esp", "{\"default\":\"nosuch-*\"}", &r) == 0) {
		if (r.status != 409)
			fail("a default matching no entry: expected 409, got %d", r.status);
		cix_response_free(&r);
	} else {
		fail("PUT with an unmatched default failed to send");
	}

	if (cix_client_request(&c, "PUT", "/v1/system/esp", "{\"timeout\":99999}", &r) == 0) {
		if (r.status != 400)
			fail("out-of-range timeout: expected 400, got %d", r.status);
		cix_response_free(&r);
	}
	if (cix_client_request(&c, "PUT", "/v1/system/esp", "{}", &r) == 0) {
		if (r.status != 400)
			fail("empty update: expected 400, got %d", r.status);
		cix_response_free(&r);
	}

	/* ---- the repair ---- */
	if (cix_client_request(&c, "PUT", "/v1/system/esp", "{\"default\":\"cix-*\"}", &r) == 0) {
		const char *selected =
		    (r.json != NULL) ? json_as_string(json_object_get(r.json, "selected_entry")) : NULL;

		if (r.status != 200)
			fail("setting a valid default: expected 200, got %d", r.status);
		else if (selected == NULL || strcmp(selected, "cix-b+3.conf") != 0)
			fail("after the fix the machine should boot cix-b+3.conf, got %s",
			     selected != NULL ? selected : "(null)");
		cix_response_free(&r);
	} else {
		fail("PUT default=cix-* failed");
	}

	/*
	 * A pattern written WITHOUT the Automatic Boot Assessment counter
	 * must still match, because systemd-boot matches against the entry
	 * id ("cix-b"), not the filename ("cix-b+3.conf"). An API that
	 * globbed differently from the firmware it describes would be
	 * worse than no API at all.
	 */
	if (cix_client_request(&c, "PUT", "/v1/system/esp", "{\"default\":\"cix-b\"}", &r) == 0) {
		const char *sel2 =
		    (r.json != NULL) ? json_as_string(json_object_get(r.json, "selected_entry")) : NULL;

		if (r.status != 200)
			fail("a counter-less default should match by entry id (got %d)", r.status);
		else if (sel2 == NULL || strcmp(sel2, "cix-b+3.conf") != 0)
			fail("default cix-b should select cix-b+3.conf, got %s",
			     sel2 != NULL ? sel2 : "(null)");
		cix_response_free(&r);
	}
	/* Back to the glob form for the remaining checks. */
	if (cix_client_request(&c, "PUT", "/v1/system/esp", "{\"default\":\"cix-*\"}", &r) == 0)
		cix_response_free(&r);

	/* A rewrite must keep directives this module has no opinion about. */
	if (run_cmd("grep -q '^console-mode keep$' '%s'", g_loader_conf) != 0 ||
	    run_cmd("grep -q '^editor no$' '%s'", g_loader_conf) != 0)
		fail("the loader.conf rewrite dropped unrelated directives");
	if (run_cmd("grep -q '^default cix-\\*$' '%s'", g_loader_conf) != 0)
		fail("the new default was not written to loader.conf");
	if (run_cmd("grep -c '^default ' '%s' | grep -q '^1$'", g_loader_conf) != 0)
		fail("loader.conf has a duplicated default line");

	/* ---- entry removal, and the guards around it ---- */
	if (cix_client_request(&c, "DELETE", "/v1/system/esp/entries/thinc-b.conf", NULL, &r) == 0) {
		if (r.status != 204)
			fail("deleting a non-running-slot entry: expected 204, got %d", r.status);
		cix_response_free(&r);
	}
	/* A duplicate of the running slot is removable... */
	if (cix_client_request(&c, "DELETE", "/v1/system/esp/entries/thinc-a+3.conf", NULL, &r) ==
	    0) {
		if (r.status != 204)
			fail("deleting a running-slot DUPLICATE: expected 204, got %d", r.status);
		cix_response_free(&r);
	}
	/* ...but the last one standing is not: that is how a remote
	 * operator makes an unreachable machine unbootable. */
	if (cix_client_request(&c, "DELETE", "/v1/system/esp/entries/thinc-a.conf", NULL, &r) == 0) {
		if (r.status != 409)
			fail("deleting the LAST running-slot entry: expected 409, got %d", r.status);
		cix_response_free(&r);
	}
	if (run_cmd("test -f '%s/thinc-a.conf'", g_entries_dir) != 0)
		fail("the refused delete removed the file anyway");

	if (cix_client_request(&c, "DELETE", "/v1/system/esp/entries/../loader.conf", NULL, &r) ==
	    0) {
		if (r.status != 400 && r.status != 404)
			fail("path traversal: expected 400/404, got %d", r.status);
		cix_response_free(&r);
	}
	if (run_cmd("test -f '%s'", g_loader_conf) != 0)
		fail("path traversal deleted loader.conf");

	if (cix_client_request(&c, "DELETE", "/v1/system/esp/entries/nope.conf", NULL, &r) == 0) {
		if (r.status != 404)
			fail("deleting a nonexistent entry: expected 404, got %d", r.status);
		cix_response_free(&r);
	}

	/*
	 * With no --slot, nothing can be attributed to a running slot and
	 * the guard above can never fire. It must not therefore protect
	 * nothing: it degrades to refusing the last entry on the ESP at
	 * all. Checked with a second daemon, because a guard that quietly
	 * stops applying under a different startup is exactly the kind of
	 * hole that only shows up when it matters.
	 */
	{
		pid_t slotless;
		char *sargv[5];
		static char dd[PATH_MAX + 12];
		static char ea[PATH_MAX + 24];
		struct cix_client c2;

		/* Strip the ESP down to a single surviving entry. */
		run_cmd("rm -f '%s'/thinc-b+3.conf '%s'/cix-b+3.conf", g_entries_dir, g_entries_dir);

		snprintf(dd, sizeof(dd), "--data-dir=%s", g_data_dir);
		snprintf(ea, sizeof(ea), "--test-esp-entries-dir=%s", g_entries_dir);
		sargv[0] = "build/cixd";
		sargv[1] = "--port=7699";
		sargv[2] = dd;
		sargv[3] = ea;
		sargv[4] = NULL;
		slotless = fork();
		if (slotless == 0) {
			execve("build/cixd", sargv, environ);
			_exit(127);
		}
		if (slotless > 0) {
			cix_client_init(&c2, "127.0.0.1", 7699);
			if (wait_for_daemon(&c2, 100) != 0) {
				fail("slotless daemon did not become ready");
			} else if (cix_client_request(&c2, "DELETE",
			                              "/v1/system/esp/entries/thinc-a.conf", NULL,
			                              &r) == 0) {
				if (r.status != 409)
					fail("with no running slot known, deleting the LAST entry should "
					     "still be refused (got %d)",
					     r.status);
				cix_response_free(&r);
			}
			kill(slotless, SIGTERM);
			waitpid(slotless, NULL, 0);
		}
		if (run_cmd("test -f '%s/thinc-a.conf'", g_entries_dir) != 0)
			fail("the slotless guard let the last entry be deleted");
	}

	if (stop_daemon(daemon_pid) != 0)
		fail("daemon did not exit cleanly on SIGTERM");
	test_data_dir_cleanup(g_data_dir);

	if (g_failures > 0) {
		printf("test_esp: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_esp: all checks passed\n");
	return 0;
}
