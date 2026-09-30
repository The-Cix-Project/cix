#ifndef CONTROLPLANE_PROGRAMS_H
#define CONTROLPLANE_PROGRAMS_H

/*
 * Every program that code running on the control-plane root executes,
 * by the absolute path it is executed at (#554).
 *
 * The root carries only what mkbootroot stages into it, and nothing
 * used to say what that had to be: cixd declared each program as a
 * `*_BIN` define, and mkbootroot staged them from tables kept in step
 * by eye. So when 0.2.57-412 took `cp` out of the root, mkbootroot
 * itself still forked it, and the first assembly run on a 412 root
 * (0.2.57-414's) failed with `execve cp: No such file or directory`.
 *
 * This list is what closes that gap, from both sides:
 *   - mkbootroot refuses to seal a root missing any program listed here
 *     (verify_controlplane_programs()), naming it;
 *   - test_controlplane_programs, in SELFTESTS, fails a release whose
 *     control-plane sources execute a program this list does not name:
 *     every `*_BIN` define and every literal `exec*("/...")`.
 * A program is added here in the same change that first executes it.
 *
 * X(id, path, source): source says which assembly input supplies it.
 * CP_ALWAYS is staged by every assembly; CP_KMOD only when mkbootroot is
 * given the kmod image's usr/bin, which is optional (#429).
 */
#define CONTROLPLANE_PROGRAMS(X)                                                                   \
	/* the console's shell (spawn_console_shell() in cixd) */                                   \
	X(CIXCTL, "/bin/cixctl", CP_ALWAYS)                                                         \
	/* PKI, release and artifact signing */                                                     \
	X(OPENSSL, "/usr/bin/openssl", CP_ALWAYS)                                                   \
	/* legacy .tar.gz artifacts; tar runs xz and bzip2 itself by name */                        \
	X(TAR, "/usr/bin/tar", CP_ALWAYS)                                                           \
	X(GZIP, "/usr/bin/gzip", CP_ALWAYS)                                                         \
	X(XZ, "/usr/bin/xz", CP_ALWAYS)                                                             \
	X(BZIP2, "/usr/bin/bzip2", CP_ALWAYS)                                                       \
	/* image content, and mkbootroot's own seal when it runs on this root */                   \
	X(UNSQUASHFS, "/usr/bin/unsquashfs", CP_ALWAYS)                                             \
	X(MKSQUASHFS, "/usr/bin/mksquashfs", CP_ALWAYS)                                             \
	/* the CPDL engine: publish, package, extract (ADR-0307 clause 6) */                        \
	X(CBS, "/usr/bin/cbs", CP_ALWAYS)                                                           \
	/* disks and partitions */                                                                  \
	X(SFDISK, "/usr/sbin/sfdisk", CP_ALWAYS)                                                    \
	X(E2FSCK, "/usr/sbin/e2fsck", CP_ALWAYS)                                                    \
	X(RESIZE2FS, "/usr/sbin/resize2fs", CP_ALWAYS)                                              \
	X(BTRFS, "/usr/sbin/btrfs", CP_ALWAYS)                                                      \
	X(MKFS_EXT4, "/usr/sbin/mkfs.ext4", CP_ALWAYS)                                              \
	X(MKFS_BTRFS, "/usr/sbin/mkfs.btrfs", CP_ALWAYS)                                            \
	/* kernel modules */                                                                        \
	X(MODPROBE, "/usr/bin/modprobe", CP_KMOD)                                                   \
	X(MODINFO, "/usr/bin/modinfo", CP_KMOD)

enum controlplane_program_source { CP_ALWAYS, CP_KMOD };

#endif
