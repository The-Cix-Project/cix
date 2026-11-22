/*
 * test_toolcapture -- diskpart keeps the END of a tool's output, and reads
 * an NVMe partition table back (#729).
 *
 * sfdisk prints the disk and its old table first and its reason last.
 * On gibsson-1 (2026-11-22) a refused grow reported the disk banner and
 * two partitions, because only the first 511 bytes were kept, and the
 * reason never reached the operator. This writes 2000 bytes whose last
 * line is the reason and checks that the captured text ends with it.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "diskpart.h"

int main(void)
{
	static const char reason[] = "sfdisk: Failed to resize partition #5.";
	char banner[2000];
	int fds[2];
	const char *got;
	size_t glen, rlen = strlen(reason);
	int ok = 1;

	memset(banner, 'x', sizeof(banner));
	if (pipe(fds) != 0) {
		fprintf(stderr, "FAIL: pipe\n");
		return 1;
	}
	if (write(fds[1], banner, sizeof(banner) - rlen - 1) < 0 ||
	    write(fds[1], "\n", 1) != 1 ||
	    write(fds[1], reason, rlen) != (ssize_t)rlen) {
		fprintf(stderr, "FAIL: write\n");
		return 1;
	}
	close(fds[1]);
	diskpart_capture_tool_output(fds[0]);
	close(fds[0]);

	got = diskpart_last_tool_error();
	glen = strlen(got);
	if (glen < rlen || strcmp(got + glen - rlen, reason) != 0) {
		fprintf(stderr, "FAIL: captured text does not end with the reason: ...%s\n",
		        glen > 60 ? got + glen - 60 : got);
		ok = 0;
	}
	if (strchr(got, '\n') != NULL) {
		fprintf(stderr, "FAIL: a newline survived the fold\n");
		ok = 0;
	}

	/* A short output is kept whole. */
	if (pipe(fds) != 0)
		return 1;
	if (write(fds[1], "short\n", 6) != 6)
		return 1;
	close(fds[1]);
	diskpart_capture_tool_output(fds[0]);
	close(fds[0]);
	if (strcmp(diskpart_last_tool_error(), "short ") != 0) {
		fprintf(stderr, "FAIL: short output: \"%s\"\n", diskpart_last_tool_error());
		ok = 0;
	}

	/*
	 * The sfdisk -d parser (#729): an NVMe partition is DEV "p" N. It
	 * read the number straight after the device name, so nvme0n1p5
	 * parsed as partition 0 and a grow on gibsson-1 could not find the
	 * entry it had just rewritten. Lines copied from sfdisk -d's form.
	 */
	{
		static const struct {
			const char *dev, *line;
			int want_ok, want_pno;
			unsigned long long want_start, want_sectors;
		} c[] = {
			{ "/dev/nvme0n1",
			  "/dev/nvme0n1p5 : start=     1837056, size=   102400000, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4",
			  1, 5, 1837056ULL, 102400000ULL },
			{ "/dev/vda", "/dev/vda5 : start= 2048, size= 4096, type=L", 1, 5, 2048ULL, 4096ULL },
			{ "/dev/mmcblk0", "/dev/mmcblk0p12 : start=10, size=20", 1, 12, 10ULL, 20ULL },
			/* Not this disk's partitions, and not partition lines. */
			{ "/dev/nvme0n1", "/dev/nvme0n15 : start=1, size=2", 0, 0, 0, 0 },
			{ "/dev/vda", "/dev/vdab1 : start=1, size=2", 0, 0, 0, 0 },
			{ "/dev/nvme0n1", "label: gpt", 0, 0, 0, 0 },
			{ "/dev/nvme0n1", "device: /dev/nvme0n1", 0, 0, 0, 0 },
		};
		size_t k;

		for (k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
			int pno = -1;
			unsigned long long start = 0, sectors = 0;
			int r = diskpart_parse_dump_line(c[k].dev, c[k].line, &pno, &start, &sectors);

			if (r != c[k].want_ok ||
			    (r && (pno != c[k].want_pno || start != c[k].want_start ||
			           sectors != c[k].want_sectors))) {
				fprintf(stderr, "FAIL: parse %s / \"%s\": got %d pno=%d start=%llu size=%llu\n",
				        c[k].dev, c[k].line, r, pno, start, sectors);
				ok = 0;
			}
		}
	}

	printf("TOOLCAPTURE: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
