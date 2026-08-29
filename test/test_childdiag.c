/*
 * Which line of a failed child's output an operator gets to see.
 *
 * This test exists because the question was answered twice, in two
 * files, with two different rules -- and each rule was wrong in the
 * case the other one handled:
 *
 *   diskformat.c took the last diagnostic line, because mkfs.btrfs
 *   ends a failure with "See https://btrfs.readthedocs.io for more
 *   information." A footer, not a reason. Its first capture reported
 *   exactly that and still said nothing about what went wrong.
 *
 *   The ISO assembly in main.c took the literal last line, and paid
 *   for it live: mkinstalleriso closes a failure with its own summary
 *   of which tool exited nonzero, one line BELOW the reason that tool
 *   printed. The operator got
 *     ".../isotools/bin/sbsign failed (status 0x100)"
 *   when the actual cause, on the line above, was
 *     "Error reading file .../systemd-bootx64.efi: No such file or
 *      directory".
 *   The summary names the tool and hides what it could not find; the
 *   real cause was only recoverable by going to the log store.
 *
 * Both real captures are asserted here verbatim, since a rule that
 * handles invented input and not the two cases that actually occurred
 * would be worth nothing. The case-insensitivity cases are not padding
 * either: the earlier matcher compared "ERROR" and "error" only, so
 * the "Error reading file" spelling above went unmatched -- that one
 * character is the whole bug.
 */
#include "childdiag.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *name, const char *input, const char *want)
{
	char buf[1024];

	snprintf(buf, sizeof(buf), "%s", input);
	childdiag_reduce_to_error_line(buf);
	if (strcmp(buf, want) != 0) {
		printf("FAIL %s\n  got  '%s'\n  want '%s'\n", name, buf, want);
		failures++;
		return;
	}
	printf("ok   %s\n", name);
}

int main(void)
{
	/* The real mkinstalleriso capture, from the failed ISO build on
	 * 192.168.15.95. Cause above, summary below. */
	check("iso: reason above the tool's own summary",
	      "Error reading file /usr/lib/systemd/boot/efi/systemd-bootx64.efi: No such file or directory\n"
	      "/var/lib/cix/disks/sda/rebuildable/artifacts/isotools/bin/sbsign failed (status 0x100)\n",
	      "Error reading file /usr/lib/systemd/boot/efi/systemd-bootx64.efi: No such file or directory");

	/* The real mkfs.btrfs shape: reason above, documentation footer
	 * below -- the case diskformat.c was written for. */
	check("btrfs: reason above the documentation footer",
	      "ERROR: unable to open /dev/sda: Device or resource busy\n"
	      "See https://btrfs.readthedocs.io for more information.\n",
	      "ERROR: unable to open /dev/sda: Device or resource busy");

	/* No line opens a diagnostic: the last non-empty line is the best
	 * available answer, which is what a bare ENOENT from a staging
	 * step looks like. */
	check("no marker: falls back to the last non-empty line",
	      "staging fdisk\nstaging sfdisk\n/usr/sbin/mkfs.fat: No such file or directory\n",
	      "/usr/sbin/mkfs.fat: No such file or directory");

	/* Three spellings, three third-party tools, one rule. */
	check("marker is case-insensitive: ERROR", "progress\nERROR: nope\n", "ERROR: nope");
	check("marker is case-insensitive: error", "progress\nerror: nope\n", "error: nope");
	check("marker is case-insensitive: Error", "progress\nError: nope\n", "Error: nope");

	/* A later diagnostic supersedes an earlier one; ordinary output
	 * after a diagnostic does not. */
	check("last diagnostic wins", "error: first\nerror: second\n", "error: second");
	check("trailing progress does not displace a diagnostic",
	      "error: the reason\ncleaning up\n", "error: the reason");

	/* "warning" is not a failure reason. Matching it would pull the
	 * wrong line out of any build log that warned and then died. */
	check("warning is not a diagnostic", "warning: deprecated\nlast line\n", "last line");

	/* Degenerate inputs must not crash or invent content. */
	check("trailing newlines are ignored", "only line\n\n\n", "only line");
	check("single line without a newline", "boom", "boom");
	check("empty input", "", "");
	check("newlines only", "\n\n\n", "");

	if (failures > 0) {
		printf("test_childdiag: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_childdiag: all checks passed\n");
	return 0;
}
