#ifndef CHILDDIAG_H
#define CHILDDIAG_H

/*
 * Reduces a failed child process's captured output to the one line an
 * operator should be shown.
 *
 * This exists because the question keeps recurring -- mkfs.btrfs
 * (daemon/src/diskformat.c), mkinstalleriso (the ISO assembly in
 * daemon/src/main.c), artifact export, control-plane assembly -- and
 * was being answered twice, differently, with one of the two answers
 * wrong each time it mattered:
 *
 *   "the last line"   mkfs.btrfs ends a failure with
 *                     "See https://btrfs.readthedocs.io for more
 *                     information." -- a footer, not a reason.
 *   "the first line"  mkinstalleriso ends with its own summary,
 *                     "<path>/sbsign failed (status 0x100)", one line
 *                     below the actual cause, "Error reading file
 *                     /usr/lib/systemd/boot/efi/systemd-bootx64.efi:
 *                     No such file or directory". Reporting the
 *                     summary names the tool that failed and hides
 *                     what it could not find.
 *
 * The rule that serves both: the LAST line that opens with an error
 * marker, since a tool that says "error" is telling you the reason;
 * failing that, the last non-empty line. Case-insensitive, because
 * these are third-party tools and they disagree ("ERROR:", "error:",
 * "Error reading file") -- the earlier matcher was case-sensitive on
 * two of the three spellings and missed the one above.
 *
 * Rewrites `buf` in place, leaving exactly that line, NUL-terminated
 * and free of any trailing newline. A buf that is empty or all
 * whitespace is left as it is. buf must be NUL-terminated and
 * writable.
 */
void childdiag_reduce_to_error_line(char *buf);

#endif /* CHILDDIAG_H */
