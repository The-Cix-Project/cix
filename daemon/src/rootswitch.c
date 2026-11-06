/*
 * ADR-0375 phase 4: the steps of switching the control plane's root in
 * place. See rootswitch.h for the contract and handover_exec() in
 * main.c for the order they run in.
 */
#include "rootswitch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* mountinfo escapes space, tab, newline and backslash as \ooo. */
static void unescape(const char *in, size_t len, char *out, size_t out_size)
{
	size_t i, o = 0;

	for (i = 0; i < len && o + 1 < out_size; i++) {
		if (in[i] == '\\' && i + 3 < len && in[i + 1] >= '0' && in[i + 1] <= '3' &&
		    in[i + 2] >= '0' && in[i + 2] <= '7' && in[i + 3] >= '0' && in[i + 3] <= '7') {
			out[o++] = (char)((in[i + 1] - '0') * 64 + (in[i + 2] - '0') * 8 + (in[i + 3] - '0'));
			i += 3;
			continue;
		}
		out[o++] = in[i];
	}
	out[o] = '\0';
}

/* One line's mount id, parent id and mount point. 0, or -1. */
static int parse_line(const char *line, size_t len, long *id, long *parent, char *point,
                      size_t point_size)
{
	const char *f[5];
	size_t flen[5];
	size_t i = 0;
	int n = 0;

	while (n < 5 && i < len) {
		while (i < len && line[i] == ' ')
			i++;
		if (i >= len)
			break;
		f[n] = line + i;
		while (i < len && line[i] != ' ')
			i++;
		flen[n] = (size_t)(line + i - f[n]);
		n++;
	}
	if (n < 5)
		return -1;
	*id = strtol(f[0], NULL, 10);
	*parent = strtol(f[1], NULL, 10);
	unescape(f[4], flen[4], point, point_size);
	return 0;
}

int rootswitch_plan_parse(const char *mountinfo, struct rootswitch_plan *plan, char *err,
                          size_t err_size)
{
	const char *p;
	long root_id = -1;
	int i, j;

	plan->count = 0;
	/* The root: the last "/" mount, the one on top. */
	for (p = mountinfo; *p != '\0';) {
		const char *nl = strchr(p, '\n');
		size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
		long id, parent;
		char point[256];

		if (len > 0 && parse_line(p, len, &id, &parent, point, sizeof(point)) == 0 &&
		    strcmp(point, "/") == 0)
			root_id = id;
		p += len + (nl != NULL ? 1 : 0);
	}
	if (root_id < 0) {
		snprintf(err, err_size, "no root mount in mountinfo");
		return -1;
	}
	for (p = mountinfo; *p != '\0';) {
		const char *nl = strchr(p, '\n');
		size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
		long id, parent;
		char point[256];

		if (len > 0 && parse_line(p, len, &id, &parent, point, sizeof(point)) == 0 &&
		    parent == root_id && id != root_id && strcmp(point, "/") != 0) {
			if (plan->count == ROOTSWITCH_MAX_MOUNTS) {
				snprintf(err, err_size, "more than %d mounts hang off the root",
				         ROOTSWITCH_MAX_MOUNTS);
				return -1;
			}
			snprintf(plan->points[plan->count++], sizeof(plan->points[0]), "%s", point);
		}
		p += len + (nl != NULL ? 1 : 0);
	}
	for (i = 0; i < plan->count; i++)
		for (j = i + 1; j < plan->count; j++)
			if (strcmp(plan->points[i], plan->points[j]) == 0) {
				snprintf(err, err_size,
				         "two mounts share %s, and a move would take only the top one",
				         plan->points[i]);
				return -1;
			}
	return 0;
}

int rootswitch_plan_read(struct rootswitch_plan *plan, char *err, size_t err_size)
{
	char *buf;
	size_t cap = 65536, len = 0;
	ssize_t n;
	int fd, rc;

	fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, err_size, "cannot read /proc/self/mountinfo: %s", strerror(errno));
		return -1;
	}
	buf = malloc(cap);
	if (buf == NULL) {
		close(fd);
		snprintf(err, err_size, "out of memory reading mountinfo");
		return -1;
	}
	for (;;) {
		if (len + 1 >= cap) {
			char *nb = realloc(buf, cap * 2);

			if (nb == NULL) {
				free(buf);
				close(fd);
				snprintf(err, err_size, "out of memory reading mountinfo");
				return -1;
			}
			buf = nb;
			cap *= 2;
		}
		n = read(fd, buf + len, cap - len - 1);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		len += (size_t)n;
	}
	close(fd);
	buf[len] = '\0';
	rc = rootswitch_plan_parse(buf, plan, err, err_size);
	free(buf);
	return rc;
}

int rootswitch_check_target(const char *dir, const struct rootswitch_plan *plan,
                            const char *release, char *err, size_t err_size)
{
	char path[PATH_MAX];
	struct stat st;
	int i;

	snprintf(path, sizeof(path), "%s/bin/cixd", dir);
	if (access(path, X_OK) != 0) {
		snprintf(err, err_size, "the staged root has no executable /bin/cixd");
		return -1;
	}
	for (i = 0; i < plan->count; i++) {
		snprintf(path, sizeof(path), "%s%s", dir, plan->points[i]);
		if (lstat(path, &st) != 0) {
			snprintf(err, err_size, "the staged root has no %s to move a mount onto",
			         plan->points[i]);
			return -1;
		}
	}
	if (release != NULL) {
		snprintf(path, sizeof(path), "/lib/modules/%s", release);
		if (stat(path, &st) == 0) {
			snprintf(path, sizeof(path), "%s/lib/modules/%s", dir, release);
			if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
				snprintf(err, err_size,
				         "the staged root has no modules for the running kernel %s", release);
				return -1;
			}
		}
	}
	return 0;
}

int rootswitch_move(const char *dir, const struct rootswitch_plan *plan, char *err,
                    size_t err_size)
{
	char target[PATH_MAX];
	int i;

	for (i = 0; i < plan->count; i++) {
		snprintf(target, sizeof(target), "%s%s", dir, plan->points[i]);
		if (mount(plan->points[i], target, NULL, MS_MOVE, NULL) != 0) {
			int saved = errno;

			snprintf(err, err_size, "cannot move %s into the staged root: %s",
			         plan->points[i], strerror(saved));
			while (--i >= 0) {
				snprintf(target, sizeof(target), "%s%s", dir, plan->points[i]);
				mount(target, plan->points[i], NULL, MS_MOVE, NULL);
			}
			return -1;
		}
	}
	return 0;
}

void rootswitch_move_back(const char *dir, const struct rootswitch_plan *plan)
{
	char target[PATH_MAX];
	int i;

	for (i = plan->count - 1; i >= 0; i--) {
		snprintf(target, sizeof(target), "%s%s", dir, plan->points[i]);
		mount(target, plan->points[i], NULL, MS_MOVE, NULL);
	}
}

int rootswitch_pivot(const char *dir, char *err, size_t err_size)
{
	if (chdir(dir) != 0) {
		snprintf(err, err_size, "cannot enter %s: %s", dir, strerror(errno));
		return -1;
	}
	if (syscall(SYS_pivot_root, ".", ".") != 0) {
		snprintf(err, err_size, "pivot_root into %s: %s", dir, strerror(errno));
		chdir("/");
		return -1;
	}
	/*
	 * The old root is now stacked beneath the new one at "/": detach it.
	 * It stays alive while anything still maps a binary from it. A
	 * detach that fails leaves it mounted and hidden, which costs memory
	 * and nothing else, so it does not fail the switch.
	 */
	umount2(".", MNT_DETACH);
	chdir("/");
	return 0;
}
