/*
 * Exec target for test_harness.c. Runs as PID 1 inside the new
 * namespaces (post pivot_root) and reports what it observes to a
 * result file on the (shared, bind-mounted) host filesystem, since
 * its own stdout would race with the parent's.
 */
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * At the root of the container's overlay, so it lands in the upperdir
 * test_harness reads. Not /tmp: every container gets a fresh tmpfs
 * there (src/mountns.c), so a file written to /tmp never reaches the
 * upperdir -- which is how test_harness failed with its child exiting
 * 7 and the result file missing (cix-tests@v2.57.246-1, 2026-09-23).
 */
#define RESULT_PATH "/harness_result.txt"

int main(void)
{
	char hostname[HOST_NAME_MAX + 1];
	DIR *d;
	struct dirent *ent;
	FILE *out;

	out = fopen(RESULT_PATH, "w");
	if (out == NULL) {
		perror("fopen result file");
		return 126;
	}

	fprintf(out, "PID=%d\n", (int)getpid());

	if (gethostname(hostname, sizeof(hostname)) != 0) {
		perror("gethostname");
		fclose(out);
		return 126;
	}
	fprintf(out, "HOSTNAME=%s\n", hostname);

	d = opendir("/sys/class/net");
	if (d == NULL) {
		perror("opendir /sys/class/net");
		fclose(out);
		return 126;
	}
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		fprintf(out, "NETIF=%s\n", ent->d_name);
	}
	closedir(d);

	fclose(out);
	return 7;
}
