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
 * This list is what closes that gap, from two sides (cix#350):
 *   - mkbootroot refuses to seal a root missing any program listed here
 *     (verify_controlplane_programs()), naming the package that fixes it;
 *   - test_controlplane_programs, in SELFTESTS, fails a release whose
 *     control-plane sources execute a program this list does not name:
 *     every `*_BIN` define and every literal `exec*("/...")`.
 * A program is added here in the same change that first executes it, and
 * its package is added to the cix-root image's recipe (ADR-0371): the
 * root is that image's tree, so nothing else can put a program in it.
 *
 * Before ADR-0371 phase 3 the list was also the SUPPLY: mkbootroot
 * staged each program from the cix-hosttools image, and before cix#350
 * from two hand-maintained tables in mkbootroot.c with a BUILD-HOST
 * fallback path for every entry -- which on an installed host is the
 * control-plane root itself, so each assembly copied the previous root's
 * copy forward and whatever a dev machine first put there stayed forever
 * (cix#589). The tests' fixture tree (test/test_root_tree.c) still
 * stages from this list, from a build container's "/".
 *
 * X(id, path, source, pkg):
 *   source -- what kind of program it is.
 *     CP_CIX   a Cix build's own (an output of the cix recipe).
 *     CP_TOOL  a third-party tool, at this same absolute path in its
 *              package.
 *     CP_KMOD  a module tool, required only in a root that carries a
 *              kernel module tree (#429).
 *   pkg -- the package of the cix-root image that provides it, so a root
 *     missing a program can say which package fixes it.
 */
#define CONTROLPLANE_PROGRAMS(X)                                                                   \
	/* the console's shell (spawn_console_shell() in cixd) */                                   \
	X(CIXCTL, "/usr/bin/cixctl", CP_CIX, "cixctl") /* ADR-0371 C: its package's path */             \
	/* image content, and mkbootroot's own seal when it runs on this root */                   \
	X(UNSQUASHFS, "/usr/bin/unsquashfs", CP_TOOL, "squashfs-tools")                        \
	X(MKSQUASHFS, "/usr/bin/mksquashfs", CP_TOOL, "squashfs-tools")                        \
	/* the CPDL engine: publish, package, extract (ADR-0307 clause 6) */                        \
	X(CBS, "/usr/bin/cbs", CP_TOOL, "cbs")                                                 \
	/* disks and partitions */                                                                  \
	X(SFDISK, "/usr/sbin/sfdisk", CP_TOOL, "util-linux")                                   \
	X(E2FSCK, "/usr/sbin/e2fsck", CP_TOOL, "e2fsprogs")                                    \
	X(RESIZE2FS, "/usr/sbin/resize2fs", CP_TOOL, "e2fsprogs")                              \
	X(BTRFS, "/usr/sbin/btrfs", CP_TOOL, "btrfs-progs")                                    \
	X(MKFS_EXT4, "/usr/sbin/mkfs.ext4", CP_TOOL, "e2fsprogs")                              \
	X(MKFS_BTRFS, "/usr/sbin/mkfs.btrfs", CP_TOOL, "btrfs-progs")                          \
	/* the ESP's check at every boot (#638) */                                                  \
	X(FSCK_FAT, "/usr/sbin/fsck.fat", CP_TOOL, "dosfstools")                               \
	/* kernel modules */                                                                        \
	X(MODPROBE, "/usr/bin/modprobe", CP_KMOD, "kmod")                                           \
	X(MODINFO, "/usr/bin/modinfo", CP_KMOD, "kmod")

enum controlplane_program_source { CP_CIX, CP_TOOL, CP_KMOD };

#endif
