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
 * machinery (sfdisk -d + GPT-name lookup): this tool's targets are
 * fixed device paths, /dev/vda4 then /dev/vda5, the exact same
 * hardcoded "these are always here once a system is actually
 * installed" convention daemon/src/main.c's own CONFIG_DEVICE and
 * CONTAINERS_DEVICE and cix-install.c's own BOOT_TIME_DISK_PREFIX
 * already rely on (a fresh install's own disk can be attached at any
 * device path while the installer runs, but the *result* is always
 * addressed this way from then on).
 *
 * Two of them because host-auth state moved from the containers
 * partition to the config partition (#250) and this tool did not move
 * with it -- see CONFIG_DEVICE below for what that cost. A recovery tool's own real security property is how
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

/*
 * Matches daemon/src/main.c's own CONFIG_DEVICE and CONTAINERS_DEVICE
 * exactly -- see this file's own header comment for why hardcoding
 * them here, rather than discovering them, is the correct choice for
 * this specific tool.
 *
 * BOTH, and in this order, because #250 moved STATE_DIR from the
 * containers partition to the config partition and this tool was not
 * moved with it. The consequence was worse than a plain failure: it
 * mounted cix-containers, found no state/hostauth_config.json there,
 * and reported "this system has never activated write-gating, so
 * there is nothing to reset" -- which reads as a benign success. The
 * operator reboots believing they are recovered and is still locked
 * out, with the tool's own output saying everything was fine.
 * Confirmed on 192.168.15.95: admin_groups was still ["cix-admins"]
 * after a completed run.
 *
 * The containers partition stays as a fallback rather than being
 * replaced, because it is where the file still lives on a system that
 * has not yet booted a daemon new enough to migrate it -- and a
 * break-glass tool that only works on current installs is no use to
 * the older box that is likelier to need it. The daemon carries the
 * same legacy path for the same reason.
 */
#define CONFIG_DEVICE "/dev/vda4"
#define CONFIG_MOUNT "/mnt/config"
#define CONTAINERS_DEVICE "/dev/vda5"
#define CONTAINERS_MOUNT "/mnt/containers"
#define HOSTAUTH_CONFIG_REL_PATH "state/hostauth_config.json"

/*
 * Where the host-auth config was actually found, so every later
 * message and the unmount name the partition this run is really
 * operating on rather than an assumed one.
 */
static const char *g_mount_point;
static const char *g_device;

/*
 * Mount each candidate in turn and keep the first that actually holds
 * the file. A partition that mounts but does not have it is unmounted
 * again before trying the next, so a successful return leaves exactly
 * one mount behind.
 */
static int mount_state_partition(char *config_path, size_t config_path_size)
{
	static const struct {
		const char *device;
		const char *mount;
	} candidates[] = {
		{ CONFIG_DEVICE, CONFIG_MOUNT },
		{ CONTAINERS_DEVICE, CONTAINERS_MOUNT },
	};
	size_t i;

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (ensure_dir(candidates[i].mount) != 0)
			return -1;
		if (mount(candidates[i].device, candidates[i].mount, "btrfs", 0, NULL) != 0 &&
		    mount(candidates[i].device, candidates[i].mount, "ext4", 0, NULL) != 0)
			continue;
		if (snprintf(config_path, config_path_size, "%s/%s", candidates[i].mount,
		             HOSTAUTH_CONFIG_REL_PATH) >= (int)config_path_size) {
			umount(candidates[i].mount);
			return -1;
		}
		if (access(config_path, R_OK) == 0) {
			g_device = candidates[i].device;
			g_mount_point = candidates[i].mount;
			dual_printf("Found host-auth state on %s.\n", candidates[i].device);
			return 0;
		}
		umount(candidates[i].mount);
	}
	return 1;
}

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

	switch (mount_state_partition(config_path, sizeof(config_path))) {
	case 0:
		break;
	case 1:
		dual_printf(
		    "No host-auth state found on %s or %s.\n"
		    "\n"
		    "Either this system has never activated write-gating -- in which case every\n"
		    "cixd API write is already open and there is nothing to reset -- or its state\n"
		    "storage was relocated to a different disk, which needs manual recovery.\n"
		    "Nothing was changed.\n",
		    CONFIG_DEVICE, CONTAINERS_DEVICE);
		return 1;
	default:
		dual_printf("Could not mount a partition to search. Nothing was changed.\n");
		return 1;
	}

	if (read_whole_file(config_path, &raw, &raw_len) != 0) {
		/* It was readable a moment ago -- mount_state_partition()
		 * selected this partition precisely because access() found it
		 * there -- so this is a real read failure, not an absent
		 * file, and must not be reported as "nothing to reset". */
		dual_perror(config_path);
		dual_printf("%s could not be read. Nothing was changed.\n", config_path);
		umount(g_mount_point);
		return 1;
	}

	root = json_parse(raw, raw_len);
	free(raw);
	if (root == NULL) {
		dual_printf("%s exists but could not be parsed as JSON -- refusing to touch it.\n",
		            config_path);
		umount(g_mount_point);
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
		umount(g_mount_point);
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
		umount(g_mount_point);
		return 1;
	}
	jw_free(&w);

	sync();
	if (umount(g_mount_point) != 0)
		dual_perror("umount");

	dual_printf("\nDone -- admin_groups reset to empty on %s.\n", g_device);
	dual_printf("Remove this recovery media and reboot into the normal installed\n");
	dual_printf("system; every cixd API write is\n");
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
