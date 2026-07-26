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

#define RESULT_PATH "/tmp/harness_result.txt"

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
