/*
 * ADR-0375 phase 4: the parts of a live root switch that can be proven
 * without being PID 1.
 *
 *   - rootswitch_plan_parse(): the mounts that hang off the root, read
 *     from mountinfo text, with escaped mount points, the root taken as
 *     the last "/" mount, and two mounts on one point refused;
 *   - rootswitch_check_target(): a staged tree without /bin/cixd, or
 *     without a planned mount point, is refused;
 *   - esp_entry_hold() and esp_entry_unhold(): the staged entry is held
 *     at a spent counter while the switch is on probation, exactly one
 *     counted entry is required, and the hold undoes.
 *
 * The moves and the pivot themselves need a real boot. test_boot_update
 * does them.
 */
#include "esp.h"
#include "rootswitch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, ...)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			fprintf(stderr, "FAIL: ");                                                             \
			fprintf(stderr, __VA_ARGS__);                                                          \
			fprintf(stderr, "\n");                                                                 \
			g_fail = 1;                                                                            \
		}                                                                                          \
	} while (0)

static void touch(const char *path)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0755);

	if (fd >= 0)
		close(fd);
}

static int exists(const char *dir, const char *name)
{
	char path[1024];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return stat(path, &st) == 0;
}

/* Measured shape: a Cix host's mountinfo, abridged, with the kernel's
 * own rootfs beneath the squashfs root and a stacked "/" on top. */
static const char g_mountinfo[] =
    "1 1 0:2 / / rw - rootfs rootfs rw\n"
    "20 1 254:2 / / ro,relatime - squashfs /dev/vda2 ro\n"
    "21 20 0:5 / /dev rw,nosuid - devtmpfs devtmpfs rw\n"
    "22 21 0:21 / /dev/pts rw - devpts devpts rw\n"
    "23 20 0:22 / /proc rw - proc proc rw\n"
    "24 20 0:23 / /sys rw - sysfs sysfs rw\n"
    "25 24 0:24 / /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n"
    "26 20 254:5 / /config rw - btrfs /dev/vda5 rw\n"
    "27 26 0:40 / /config/state/procfuse rw - fuse cix-procfuse rw\n"
    "28 20 0:25 / /mnt/cix rw - tmpfs tmpfs rw\n"
    "29 20 254:6 / /var/lib/cix rw - btrfs /dev/vda6 rw\n"
    "30 20 254:5 /state/resolv.conf /etc/resolv.conf rw - btrfs /dev/vda5 rw\n"
    "31 20 254:1 / /boot rw - vfat /dev/vda1 rw\n"
    "32 28 254:7 / /mnt/cix/sda\\040disk rw - btrfs /dev/sda rw\n";

static void plan_cases(void)
{
	struct rootswitch_plan plan;
	char err[256];
	char *dup;
	size_t n;
	int i, seen_resolv = 0, seen_pts = 0;

	CHECK(rootswitch_plan_parse(g_mountinfo, &plan, err, sizeof(err)) == 0, "plan: %s", err);
	/* /dev /proc /sys /config /mnt/cix /var/lib/cix /etc/resolv.conf
	 * /boot: the bind onto a file in the root hangs off the root too. */
	CHECK(plan.count == 8, "plan has %d mounts, expected 8", plan.count);
	for (i = 0; i < plan.count; i++) {
		if (strcmp(plan.points[i], "/etc/resolv.conf") == 0)
			seen_resolv = 1;
		if (strcmp(plan.points[i], "/dev/pts") == 0 ||
		    strcmp(plan.points[i], "/config/state/procfuse") == 0 ||
		    strcmp(plan.points[i], "/mnt/cix/sda disk") == 0)
			seen_pts = 1;
	}
	CHECK(!seen_pts, "a submount must move with its parent, not on its own");
	CHECK(seen_resolv, "the bind onto /etc/resolv.conf must be planned");

	/* A bind onto a file in the root, and an escaped mount point. */
	CHECK(rootswitch_plan_parse("20 1 254:2 / / ro - squashfs /dev/vda2 ro\n"
	                            "40 20 254:5 /state/resolv.conf /etc/resolv.conf rw - btrfs x rw\n"
	                            "41 20 0:30 / /mnt/a\\040b rw - tmpfs tmpfs rw\n",
	                            &plan, err, sizeof(err)) == 0 &&
	          plan.count == 2 && strcmp(plan.points[0], "/etc/resolv.conf") == 0 &&
	          strcmp(plan.points[1], "/mnt/a b") == 0,
	      "a file bind and an escaped point: count %d, %s", plan.count, err);

	/* Two mounts on one point: refused. */
	n = strlen(g_mountinfo);
	dup = malloc(n + 64);
	if (dup != NULL) {
		snprintf(dup, n + 64, "%s33 20 0:26 / /boot rw - tmpfs tmpfs rw\n", g_mountinfo);
		CHECK(rootswitch_plan_parse(dup, &plan, err, sizeof(err)) != 0 &&
		          strstr(err, "/boot") != NULL,
		      "two mounts on /boot must be refused, got: %s", err);
		free(dup);
	}

	CHECK(rootswitch_plan_parse("", &plan, err, sizeof(err)) != 0, "no root must be refused");
}

static void target_cases(const char *dir)
{
	struct rootswitch_plan plan;
	char root[512], path[600], err[256];

	snprintf(root, sizeof(root), "%s/nextroot", dir);
	mkdir(root, 0755);
	plan.count = 2;
	snprintf(plan.points[0], sizeof(plan.points[0]), "/proc");
	snprintf(plan.points[1], sizeof(plan.points[1]), "/etc/resolv.conf");

	CHECK(rootswitch_check_target(root, &plan, NULL, err, sizeof(err)) != 0 &&
	          strstr(err, "/bin/cixd") != NULL,
	      "a tree without /bin/cixd must be refused, got: %s", err);
	snprintf(path, sizeof(path), "%s/bin", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/bin/cixd", root);
	touch(path);
	CHECK(rootswitch_check_target(root, &plan, NULL, err, sizeof(err)) != 0 &&
	          strstr(err, "/proc") != NULL,
	      "a tree without a planned mount point must be refused, got: %s", err);
	snprintf(path, sizeof(path), "%s/proc", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/etc", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/etc/resolv.conf", root);
	touch(path);
	CHECK(rootswitch_check_target(root, &plan, NULL, err, sizeof(err)) == 0,
	      "a complete tree must be accepted: %s", err);
}

static void hold_cases(const char *dir)
{
	char entries[512], path[600], from[64], to[64], err[256];

	snprintf(entries, sizeof(entries), "%s/entries", dir);
	mkdir(entries, 0755);
	snprintf(path, sizeof(path), "%s/cix-a.conf", entries);
	touch(path);

	/* No entry for b: refused. */
	CHECK(esp_entry_hold(entries, "b", from, to, sizeof(from), err, sizeof(err)) != 0,
	      "a slot with no entry must be refused");

	/* A confirmed entry only: refused, nothing staged it. */
	snprintf(path, sizeof(path), "%s/cix-b.conf", entries);
	touch(path);
	CHECK(esp_entry_hold(entries, "b", from, to, sizeof(from), err, sizeof(err)) != 0 &&
	          strstr(err, "no boot counter") != NULL,
	      "a confirmed entry must be refused, got: %s", err);

	/* A stale confirmed entry beside a counted one: refused. */
	snprintf(path, sizeof(path), "%s/cix-b+3.conf", entries);
	touch(path);
	CHECK(esp_entry_hold(entries, "b", from, to, sizeof(from), err, sizeof(err)) != 0 &&
	          strstr(err, "2 boot entries") != NULL,
	      "two entries for one slot must be refused, got: %s", err);
	snprintf(path, sizeof(path), "%s/cix-b.conf", entries);
	unlink(path);

	/* The one counted entry: held at a spent counter, and undone. */
	CHECK(esp_entry_hold(entries, "b", from, to, sizeof(from), err, sizeof(err)) == 0 &&
	          strcmp(from, "cix-b+3.conf") == 0 && strcmp(to, "cix-b+0-3.conf") == 0 &&
	          exists(entries, "cix-b+0-3.conf") && !exists(entries, "cix-b+3.conf"),
	      "hold: %s -> %s (%s)", from, to, err);
	CHECK(esp_entry_to_confirm(entries, "b", path, sizeof(path)) &&
	          strcmp(path, "cix-b+0-3.conf") == 0,
	      "confirmation must find the held entry, found %s", path);
	CHECK(esp_entry_unhold(entries, from, to) == 0 && exists(entries, "cix-b+3.conf") &&
	          !exists(entries, "cix-b+0-3.conf"),
	      "unhold must restore cix-b+3.conf");

	/* A counter part-spent keeps its total; one with no tries left is refused. */
	snprintf(path, sizeof(path), "%s/cix-b+3.conf", entries);
	unlink(path);
	snprintf(path, sizeof(path), "%s/cix-b+2-1.conf", entries);
	touch(path);
	CHECK(esp_entry_hold(entries, "b", from, to, sizeof(from), err, sizeof(err)) == 0 &&
	          strcmp(to, "cix-b+0-3.conf") == 0,
	      "cix-b+2-1.conf must hold as cix-b+0-3.conf, got %s (%s)", to, err);
	CHECK(esp_entry_hold(entries, "b", from, to, sizeof(from), err, sizeof(err)) != 0 &&
	          strstr(err, "no tries left") != NULL,
	      "an entry with no tries left must be refused, got: %s", err);
}

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (f != NULL) {
		fputs(text, f);
		fclose(f);
	}
}

/* A newly staged entry outranks every entry already on the ESP, even one
 * staged while the clock ran fast (gibsson-1, 2026-11-21: slot b staged
 * 8 hours ahead beat the slot a 0.2.57-711 switched into, and a reboot
 * came back on 0.2.57-710). */
static void version_cases(const char *dir)
{
	char loader[512], entries[512], path[600];

	snprintf(loader, sizeof(loader), "%s/vloader", dir);
	snprintf(entries, sizeof(entries), "%s/ventries", dir);
	mkdir(loader, 0755);
	mkdir(entries, 0755);
	esp_init(loader, entries);
	snprintf(path, sizeof(path), "%s/cix-a.conf", entries);
	write_file(path, "title Cix (A)\nsort-key cix\nversion 100\nlinux /cix-bzImage-a\n");
	snprintf(path, sizeof(path), "%s/cix-b.conf", entries);
	write_file(path, "title Cix (B)\nsort-key cix\nversion 5000\nlinux /cix-bzImage-b\n");
	CHECK(esp_next_entry_version(200) == 5001,
	      "an entry from a fast clock (5000) must be outranked: got %ld",
	      esp_next_entry_version(200));
	CHECK(esp_next_entry_version(9000) == 9000,
	      "a clock later than every entry is used as it is: got %ld",
	      esp_next_entry_version(9000));
	snprintf(path, sizeof(path), "%s/cix-b.conf", entries);
	write_file(path, "title Cix (B)\nsort-key cix\nlinux /cix-bzImage-b\n");
	CHECK(esp_next_entry_version(200) == 200,
	      "an entry with no version does not move it: got %ld", esp_next_entry_version(200));

	/* Confirming makes the confirmed entry outrank the other, rewriting
	 * only its version line; a second call writes nothing. */
	snprintf(path, sizeof(path), "%s/cix-b.conf", entries);
	write_file(path, "title Cix (B)\nsort-key cix\nversion 5000\nlinux /cix-bzImage-b\n");
	CHECK(esp_entry_make_newest(entries, "cix-a.conf") == 0, "make_newest failed");
	{
		struct esp_entry e[8];
		int n = esp_entries_list(e, 8), i;
		long va = -1, vb = -1;
		char buf[256];
		FILE *f;
		size_t r = 0;

		for (i = 0; i < n; i++) {
			if (strcmp(e[i].name, "cix-a.conf") == 0)
				va = strtol(e[i].version, NULL, 10);
			if (strcmp(e[i].name, "cix-b.conf") == 0)
				vb = strtol(e[i].version, NULL, 10);
		}
		CHECK(n == 2, "only the two entries remain, no leftover file: %d", n);
		CHECK(va == 5001 && vb == 5000, "cix-a must outrank cix-b: a=%ld b=%ld", va, vb);
		snprintf(path, sizeof(path), "%s/cix-a.conf", entries);
		f = fopen(path, "r");
		if (f != NULL) {
			r = fread(buf, 1, sizeof(buf) - 1, f);
			fclose(f);
		}
		buf[r] = '\0';
		CHECK(strcmp(buf, "title Cix (A)\nsort-key cix\nversion 5001\nlinux /cix-bzImage-a\n") == 0,
		      "only the version line changes, got:\n%s", buf);
	}
	CHECK(esp_entry_make_newest(entries, "cix-a.conf") == 0, "a second make_newest failed");
}

int main(void)
{
	char tmpl[512];
	const char *tmp = getenv("TMPDIR");

	snprintf(tmpl, sizeof(tmpl), "%s/cix_rootswitch_XXXXXX",
	         tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	plan_cases();
	target_cases(tmpl);
	hold_cases(tmpl);
	version_cases(tmpl);
	printf("ROOTSWITCH: %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
