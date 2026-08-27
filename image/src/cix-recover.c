/*
 * cix-recover: a deliberately tiny, independent break-glass tool
 * (ADR-0146) -- boots as its own init= target from the SAME installer
 * media as cix-install (a second GRUB menu entry, see
 * mkinstalleriso.c), but never reformats or reinstalls anything. It
 * mounts the ALREADY-INSTALLED system's own real containers partition
 * read-write, resets exactly one field (admin_groups, back to "no one
 * is an admin yet") in the persisted host-auth config, and leaves
 * everything else -- every container, every LDAP record, the LDAP
 * backend settings themselves -- completely untouched.
 *
 * Deliberately NOT sharing cix-install.c's own partition-discovery
 * machinery (sfdisk -d + GPT-name lookup): this tool's own real,
 * fixed target is always /dev/vda5, the exact same hardcoded
 * "cix-containers is always here once a system is actually
 * installed" convention daemon/src/main.c's own CONTAINERS_DEVICE and
 * cix-install.c's own BOOT_TIME_DISK_PREFIX already rely on (a
 * fresh install's own disk can be attached at any device path while
 * the installer runs, but the *result* is always addressed this way
 * from then on). A recovery tool's own real security property is how
 * small and independently-auditable its own code is -- reusing the
 * full installer's much larger disk-partitioning surface for a task
 * that needs none of it would work against that, not for it.
 *
 * The real, deliberate friction this tool has, matching a genuine
 * "not trivial" bar: reaching this code at all already requires
 * hypervisor/physical console access to attach different boot media
 * and force a reboot -- something no network-side attacker
 * manipulating cixd's own REST API could ever do, gated or not.
 * On top of that, this tool also requires a real typed confirmation
 * at the console before touching anything, so a stray or accidental
 * boot into this entry can't silently disable write-gating.
 */
#include "dual_console.h"

#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

/* Matches daemon/src/main.c's own CONTAINERS_DEVICE exactly -- see
 * this file's own header comment for why hardcoding it here, rather
 * than discovering it, is the correct choice for this specific tool. */
#define CONTAINERS_DEVICE "/dev/vda5"
#define CONTAINERS_MOUNT "/mnt/containers"
#define HOSTAUTH_CONFIG_REL_PATH "state/hostauth_config.json"

static int early_mounts(void)
{
	if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		dual_perror("/proc");
		return -1;
	}
	if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
		dual_perror("/sys");
		return -1;
	}
	return 0;
}

static int ensure_dir(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		dual_perror(path);
		return -1;
	}
	return 0;
}

/* Reads a whole file into a malloc'd, NUL-terminated buffer. Returns
 * 0 (buf/len set) on success, -1 (buf untouched) if the file doesn't
 * exist or can't be read -- both real, reportable outcomes here, not
 * silently tolerated the way a few daemon-side callers treat "doesn't
 * exist yet" (this tool has nothing sensible to do with a system that
 * was never actually installed). */
static int read_whole_file(const char *path, char **out_buf, size_t *out_len)
{
	FILE *f;
	long size;
	char *buf;

	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -1;
	}
	buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		return -1;
	}
	if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return -1;
	}
	buf[size] = '\0';
	fclose(f);
	*out_buf = buf;
	*out_len = (size_t)size;
	return 0;
}

/* Atomic write: temp file in the same directory, fsync, rename --
 * the exact same "never leave a half-written config on disk" pattern
 * persist_atomic_write() (daemon/src/persist.c) already establishes,
 * reimplemented here directly rather than linking that module, to
 * keep this tool's own real dependency list as small as the rest of
 * it deliberately is. */
static int write_whole_file_atomic(const char *path, const char *content, size_t len)
{
	char tmp_path[600];
	int fd;

	if (snprintf(tmp_path, sizeof(tmp_path), "%s.recover-tmp", path) >= (int)sizeof(tmp_path))
		return -1;

	fd = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd < 0) {
		dual_perror(tmp_path);
		return -1;
	}
	if (len > 0 && write(fd, content, len) != (ssize_t)len) {
		dual_perror("write");
		close(fd);
		unlink(tmp_path);
		return -1;
	}
	if (fsync(fd) != 0) {
		dual_perror("fsync");
		close(fd);
		unlink(tmp_path);
		return -1;
	}
	close(fd);
	if (rename(tmp_path, path) != 0) {
		dual_perror("rename");
		unlink(tmp_path);
		return -1;
	}
	return 0;
}

/*
 * Issue #131's purest instance, observed on a real serial capture: this
 * tool runs as PID 1, so RETURNING from main -- even with exit status 0,
 * after doing its job perfectly -- makes the kernel panic with
 * "Attempted to kill init!" and a stack trace. A successful recovery
 * presented as a crash. So main() no longer returns: every path funnels
 * into a parked end state that says what happened and waits for the
 * operator to power off or reset, which is what "done" honestly means
 * for a single-purpose boot medium.
 */
static int recover_main(void)
{
	char *raw = NULL;
	size_t raw_len = 0;
	struct json_value *root;
	const struct json_value *jidle, *jldapen, *jservers, *jport;
	const char *base_dn;
	char config_path[600];
	char line[64];
	struct json_writer w;

	dual_console_open("/dev/tty0", "/dev/ttyS0");

	if (early_mounts() != 0)
		return 1;

	dual_printf("\n");
	dual_printf("=====================================================================\n");
	dual_printf("  Cix Recovery: reset host-auth admin_groups (ADR-0146)\n");
	dual_printf("=====================================================================\n");
	dual_printf("This resets ONLY the admin_groups list in the already-installed\n");
	dual_printf("system's own persisted host-auth config back to empty -- the same\n");
	dual_printf("state a fresh install starts in, where every cixd API write is\n");
	dual_printf("open with no login required. Nothing else is touched: no container,\n");
	dual_printf("no LDAP user/group record, no LDAP backend setting, no data of any\n");
	dual_printf("kind is modified or deleted.\n");
	dual_printf("\n");
	dual_printf("Use this only if you are genuinely locked out of cixd's own API\n");
	dual_printf("(every login attempt failing) and have no other way back in.\n");
	dual_printf("\n");

	if (ensure_dir(CONTAINERS_MOUNT) != 0)
		return 1;
	if (mount(CONTAINERS_DEVICE, CONTAINERS_MOUNT, "ext4", 0, NULL) != 0) {
		dual_perror("mount " CONTAINERS_DEVICE);
		dual_printf(
		    "Could not mount %s -- this recovery tool only supports the standard layout\n"
		    "(host-auth state on the primary OS disk's own cix-containers partition).\n"
		    "A system whose state storage was relocated to a different disk needs manual\n"
		    "recovery instead.\n",
		    CONTAINERS_DEVICE);
		return 1;
	}

	if (snprintf(config_path, sizeof(config_path), "%s/%s", CONTAINERS_MOUNT,
	             HOSTAUTH_CONFIG_REL_PATH) >= (int)sizeof(config_path)) {
		umount(CONTAINERS_MOUNT);
		return 1;
	}

	if (read_whole_file(config_path, &raw, &raw_len) != 0) {
		dual_printf("%s does not exist -- this system has never activated write-gating,\n"
		            "so there is nothing to reset. Nothing was changed.\n",
		            config_path);
		umount(CONTAINERS_MOUNT);
		return 0;
	}

	root = json_parse(raw, raw_len);
	free(raw);
	if (root == NULL) {
		dual_printf("%s exists but could not be parsed as JSON -- refusing to touch it.\n",
		            config_path);
		umount(CONTAINERS_MOUNT);
		return 1;
	}

	jidle = json_object_get(root, "idle_timeout_seconds");
	jldapen = json_object_get(root, "ldap_enabled");
	jservers = json_object_get(root, "ldap_servers");
	jport = json_object_get(root, "ldap_port");
	base_dn = json_as_string(json_object_get(root, "ldap_base_dn"));

	dual_printf("Current config found. admin_groups will be reset to empty; every\n");
	dual_printf("other field (idle_timeout_seconds=%ld, ldap_enabled=%s, ldap_port=%ld,\n"
	            "ldap_base_dn=%s) is preserved exactly as-is.\n",
	            (long)json_as_number(jidle),
	            (jldapen != NULL && jldapen->type == JSON_BOOL && jldapen->u.boolean) ? "true"
	                                                                                  : "false",
	            (long)json_as_number(jport), base_dn != NULL && base_dn[0] != '\0' ? base_dn : "-");
	dual_printf("\n");
	dual_printf("Type RESET (all capitals) and press Enter to proceed, anything else to\n");
	dual_printf("abort and reboot with nothing changed: ");

	if (fgets(line, sizeof(line), stdin) == NULL || strncmp(line, "RESET", 5) != 0 ||
	    (line[5] != '\n' && line[5] != '\0')) {
		dual_printf("\nAborted -- nothing was changed.\n");
		json_free(root);
		umount(CONTAINERS_MOUNT);
		return 0;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "admin_groups");
	jw_arr_open(&w);
	jw_arr_close(&w);
	jw_key(&w, "idle_timeout_seconds");
	jw_int(&w, (long long)json_as_number(jidle));
	jw_key(&w, "ldap_enabled");
	jw_bool(&w, jldapen != NULL && jldapen->type == JSON_BOOL && jldapen->u.boolean);
	jw_key(&w, "ldap_servers");
	jw_arr_open(&w);
	if (jservers != NULL && jservers->type == JSON_ARRAY) {
		size_t i;

		for (i = 0; i < jservers->u.array.count; i++)
			jw_str(&w, json_as_string(jservers->u.array.items[i]));
	}
	jw_arr_close(&w);
	jw_key(&w, "ldap_port");
	jw_int(&w, (long long)json_as_number(jport));
	jw_key(&w, "ldap_base_dn");
	jw_str(&w, base_dn != NULL ? base_dn : "");
	jw_obj_close(&w);
	json_free(root);

	if (write_whole_file_atomic(config_path, w.buf, w.len) != 0) {
		dual_printf("\nFailed to write %s -- nothing was confirmed changed.\n", config_path);
		jw_free(&w);
		umount(CONTAINERS_MOUNT);
		return 1;
	}
	jw_free(&w);

	sync();
	if (umount(CONTAINERS_MOUNT) != 0)
		dual_perror("umount " CONTAINERS_MOUNT);

	dual_printf("\nDone -- admin_groups reset to empty. Remove this recovery media and\n");
	dual_printf("reboot into the normal installed system; every cixd API write is\n");
	dual_printf("open again until you configure a real admin group (cixctl\n");
	dual_printf("hostauth-config set --admin-group=...).\n");
	return 0;
}

int main(void)
{
	int rc = recover_main();

	dual_printf("\n=====================================================================\n");
	if (rc == 0)
		dual_printf("  cix-recover finished successfully.\n");
	else
		dual_printf("  cix-recover finished with an ERROR (see above) -- nothing partial\n"
		            "  was left behind.\n");
	dual_printf("  It is now safe to power off or reset this machine.\n");
	dual_printf("=====================================================================\n");
	sync();
	for (;;)
		pause();
}
