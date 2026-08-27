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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "esp.h"

extern char **environ;

#define TEST_PORT 7689
#define PORT_ARG "--port=7689"

static char g_data_dir[PATH_MAX];
static char g_esp_dir[PATH_MAX];
static char g_efivars_dir[PATH_MAX];
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
/*
 * sort-key and version are written here because the real thing writes
 * them -- cix-install.c emits "sort-key cix / version 1", and every
 * POST /system/update emits "sort-key cix / version <unix timestamp>".
 * They are not decoration: they are what systemd-boot actually orders
 * by, so a fixture omitting them cannot model selection at all. An
 * earlier version of this test omitted both and therefore "verified"
 * an ordering rule the bootloader does not use.
 */
static int write_entry(const char *name, const char *slot, const char *init_path,
                        const char *version)
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
	        "title Cix (%s)\nsort-key cix\nversion %s\nlinux /cix-bzImage-%s\noptions "
	        "console=tty0 root=/dev/vda%s rw init=%s -- --init-mode --slot=%s\n",
	        name, version, slot, strcmp(slot, "a") == 0 ? "2" : "3", init_path, slot);
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

	/*
	 * Versions mirror how these entries really come to exist: "1" from
	 * the installer, and an increasing timestamp from each subsequent
	 * update. Expected selections below were taken from real
	 * `bootctl list` output against this exact layout, not derived
	 * from reading the specification.
	 */
	if (write_entry("thinc-a", "a", "/bin/thincd", "1") != 0 ||
	    write_entry("thinc-b", "b", "/bin/thincd", "100") != 0 ||
	    write_entry("thinc-a+3", "a", "/bin/thincd", "200") != 0 ||
	    write_entry("thinc-b+3", "b", "/bin/thincd", "300") != 0 ||
	    write_entry("cix-b+3", "b", "/bin/cixd", "400") != 0)
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
	snprintf(g_efivars_dir, sizeof(g_efivars_dir), "%s/efivars", g_data_dir);
	mkdir(g_efivars_dir, 0755);
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
		const struct json_value *stale = entry_named(r.json, "thinc-b+3.conf");

		if (!bool_field(r.json, "present"))
			fail("ESP reported absent with a real loader directory present");
		if (def == NULL || strcmp(def, "thinc-*") != 0)
			fail("default pattern: got %s, expected thinc-*", def != NULL ? def : "(null)");
		/*
		 * The whole bug in one field: the firmware boots a stale entry
		 * and the correctly staged one is never even a candidate.
		 *
		 * Which stale entry is decided by version, descending. Real
		 * `bootctl list` on this exact layout orders them
		 * cix-b+3 (400), thinc-b+3 (300), thinc-a+3 (200),
		 * thinc-b (100), thinc-a (1) -- so thinc-* selects
		 * thinc-b+3.conf. This field exists to be
		 * believed, so being confidently wrong here is worse than not
		 * reporting it: an earlier implementation ordered by filename
		 * ascending and answered "thinc-a", which would send an
		 * operator looking at the wrong slot entirely.
		 */
		if (selected == NULL || strcmp(selected, "thinc-b+3.conf") != 0)
			fail("selected_entry: got %s, expected thinc-b+3.conf (highest version among "
			     "thinc-*, per real bootctl)",
			     selected != NULL ? selected : "(null)");
		if (staged == NULL)
			fail("the staged cix-b+3.conf entry is missing from the listing");
		else if (bool_field(staged, "matches_default"))
			fail("the staged entry must NOT match a thinc-* default");
		if (stale == NULL || !bool_field(stale, "matches_default"))
			fail("the stale thinc-b+3.conf entry should match the thinc-* default");
		cix_response_free(&r);
	} else {
		fail("GET /v1/system/esp failed");
	}

	/*
	 * ---- the guarantee an A/B update depends on ----
	 *
	 * With the correct default, a freshly staged entry must be the one
	 * that boots. This is the property that makes POST /system/update
	 * work at all, and it rests entirely on version-descending order:
	 * the staged entry carries a unix timestamp, every older entry
	 * carries something smaller, so the new one sorts first regardless
	 * of which slot letter it happens to be.
	 *
	 * Asserted explicitly because an earlier ordering model got this
	 * backwards and reported that a correctly staged slot-B update
	 * would never boot. That reading is worse than a missing field: it
	 * invites pinning the default to one slot, which really does break
	 * the next update in the other direction.
	 */
	if (cix_client_request(&c, "PUT", "/v1/system/esp", "{\"default\":\"cix-*\"}", &r) == 0) {
		if (r.status != 200)
			fail("setting default to cix-*: expected 200, got %d", r.status);
		cix_response_free(&r);
	} else {
		fail("PUT default=cix-* failed to send");
	}
	if (cix_client_request(&c, "GET", "/v1/system/esp", NULL, &r) == 0 && r.json != NULL) {
		const char *selected = json_as_string(json_object_get(r.json, "selected_entry"));

		if (selected == NULL || strcmp(selected, "cix-b+3.conf") != 0)
			fail("with default cix-*, the staged entry must be selected; got %s",
			     selected != NULL ? selected : "(null)");
		cix_response_free(&r);
	} else {
		fail("GET /v1/system/esp after setting cix-* failed");
	}
	/* restore the stale-pattern fixture for the checks that follow */
	if (cix_client_request(&c, "PUT", "/v1/system/esp", "{\"default\":\"thinc-*\"}", &r) == 0)
		cix_response_free(&r);

	/*
	 * ---- which entry a successful boot confirms ----
	 *
	 * After an update the ESP legitimately holds both `cix-a.conf`
	 * (confirmed, from an earlier cycle) and `cix-a+3.conf` (just
	 * booted, awaiting confirmation). The counter-bearing one must win.
	 *
	 * Getting this wrong is a silent revert rather than a visible
	 * failure: an implementation taking whichever entry readdir()
	 * returned first concludes "already confirmed" against the bare one
	 * and leaves the real counter ticking down, so systemd-boot gives
	 * up on the new slot two boots later and falls back. That happened
	 * on a real host after a successful cutover -- the update booted,
	 * ran, passed every check, and was quietly undone.
	 *
	 * Asserted on the rank rather than through a directory ON PURPOSE.
	 * A directory-driven test cannot catch this: readdir() order is
	 * unspecified, and this sandbox's filesystem happens to return
	 * "cix-a+3.conf" first, so the broken implementation passes here
	 * and fails only on the real ESP's vfat. Verified directly before
	 * writing this, by reintroducing the bug and watching the
	 * directory-driven version still report success.
	 */
	if (esp_confirm_rank("cix-a+3.conf", "a") != 2)
		fail("a counter-bearing entry must outrank a confirmed one");
	if (esp_confirm_rank("cix-a+2-1.conf", "a") != 2)
		fail("a partially-consumed counter still needs confirming");
	if (esp_confirm_rank("cix-a.conf", "a") != 1)
		fail("an already-confirmed entry should rank as a fallback, not as absent");
	if (esp_confirm_rank("cix-abc.conf", "a") != 0)
		fail("cix-abc.conf is not slot a's entry -- the prefix needs a '.' or '+' after it");
	if (esp_confirm_rank("cix-b+3.conf", "a") != 0)
		fail("another slot's entry must never be confirmed for this one");
	if (esp_confirm_rank("thinc-a.conf", "a") != 0)
		fail("a foreign-prefix entry must not be confirmed");

	/* And the scan itself, over a real directory holding both. */
	{
		char picked[ESP_ENTRY_NAME_MAX];
		char dir[PATH_MAX];
		char path[PATH_MAX];
		FILE *f;

		snprintf(dir, sizeof(dir), "%s/confirmscan", g_esp_dir);
		if (mkdir(dir, 0755) != 0 && errno != EEXIST)
			fail("could not create %s", dir);
		snprintf(path, sizeof(path), "%s/cix-a.conf", dir);
		if ((f = fopen(path, "w")) != NULL) {
			fputs("title x\n", f);
			fclose(f);
		}
		snprintf(path, sizeof(path), "%s/cix-a+3.conf", dir);
		if ((f = fopen(path, "w")) != NULL) {
			fputs("title x\n", f);
			fclose(f);
		}
		if (!esp_entry_to_confirm(dir, "a", picked, sizeof(picked)) ||
		    strcmp(picked, "cix-a+3.conf") != 0)
			fail("scan picked %s, expected cix-a+3.conf", picked);
		if (esp_entry_to_confirm(dir, "b", picked, sizeof(picked)))
			fail("slot b has no entry here, but one was reported: %s", picked);
	}

	/*
	 * ---- boot-next: the one-shot (issue #154) ----
	 *
	 * The rollback tool. Until it existed, "this update is bad, go
	 * back" meant waiting for a boot counter to exhaust over three
	 * reboots, or reinstalling -- and the apparent workaround, pinning
	 * the loader default to a slot, is worse than the problem because
	 * `default` is sticky and the NEXT update stages the other slot.
	 *
	 * Exercised against a temp directory standing in for efivarfs: a
	 * dev sandbox mounts /sys read-only and cannot write a real EFI
	 * variable at all, so without the override none of this could be
	 * tested anywhere but on hardware.
	 */
	{
		char armed[ESP_ENTRY_NAME_MAX];

		/* This process links esp.c directly, so the module needs the
		 * same directories the daemon under test was given -- without
		 * esp_init() it has no entry list and every lookup below would
		 * fail for the wrong reason. */
		esp_init(g_esp_dir, g_entries_dir);
		esp_set_efivars_dir(g_efivars_dir);

		if (esp_boot_next_get(armed, sizeof(armed)))
			fail("nothing should be armed before anything arms it, got %s", armed);

		/*
		 * The fixture's slot-b file is "cix-b+3.conf", but the id
		 * systemd-boot matches on is "cix-b.conf" -- ".conf" kept, the
		 * boot counter stripped. Taken from real `bootctl list`
		 * output, because writing the wrong spelling produces a
		 * variable the bootloader silently ignores, which would look
		 * exactly like the feature not working.
		 */
		if (esp_boot_next_set("b") != ESP_OK)
			fail("arming slot b should succeed -- cix-b+3.conf is present");
		if (!esp_boot_next_get(armed, sizeof(armed)) || strcmp(armed, "cix-b.conf") != 0)
			fail("armed entry should be cix-b.conf, got %s", armed);

		/* The raw variable is what the firmware reads, so its exact
		 * bytes matter more than the round-trip: a 4-byte attribute
		 * word (NV|BS|RT) then NUL-terminated UTF-16LE. */
		{
			char path[PATH_MAX];
			unsigned char raw[64];
			FILE *f;
			size_t got;

			snprintf(path, sizeof(path),
			         "%s/LoaderEntryOneShot-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f",
			         g_efivars_dir);
			f = fopen(path, "rb");
			if (f == NULL) {
				fail("the EFI variable file was not created at %s", path);
			} else {
				got = fread(raw, 1, sizeof(raw), f);
				fclose(f);
				if (got < 4 || raw[0] != 0x07 || raw[1] != 0 || raw[2] != 0 || raw[3] != 0)
					fail("attribute word should be NV|BS|RT (0x07), got %02x%02x%02x%02x",
					     raw[0], raw[1], raw[2], raw[3]);
				else if (got != 4 + (strlen("cix-b.conf") + 1) * 2)
					fail("variable is %zu bytes, expected %zu for NUL-terminated UTF-16LE",
					     got, (size_t)(4 + (strlen("cix-b.conf") + 1) * 2));
				else if (raw[4] != 'c' || raw[5] != 0 || raw[6] != 'i' || raw[7] != 0)
					fail("payload is not UTF-16LE");
			}
		}

		/* Arming a slot with no entry would need a console to recover
		 * from -- exactly what this feature exists to avoid. */
		if (esp_boot_next_set("zz") != ESP_ERR_INVALID &&
		    esp_boot_next_set("zz") != ESP_ERR_NOT_FOUND)
			fail("a slot with no loader entry must be refused, not armed");

		if (esp_boot_next_clear() != ESP_OK)
			fail("disarming should succeed");
		if (esp_boot_next_get(armed, sizeof(armed)))
			fail("nothing should be armed after clearing, got %s", armed);
		/* Disarming an already-disarmed machine is the desired state,
		 * not an error -- it must be safe to call blindly. */
		if (esp_boot_next_clear() != ESP_OK)
			fail("clearing twice should be idempotent");
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
