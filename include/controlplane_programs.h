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
 * This list is what closes that gap, from THREE sides (cix#350):
 *   - mkbootroot STAGES every CP_HOSTTOOLS program from this list, so
 *     the root's program set is derived from it rather than from a
 *     table kept beside it (stage_controlplane_programs());
 *   - mkbootroot refuses to seal a root missing any program listed here
 *     (verify_controlplane_programs()), naming it;
 *   - test_controlplane_programs, in SELFTESTS, fails a release whose
 *     control-plane sources execute a program this list does not name:
 *     every `*_BIN` define and every literal `exec*("/...")`.
 * A program is added here in the same change that first executes it.
 *
 * The staging used to be two hand-maintained tables in mkbootroot.c
 * plus two one-off blocks, and the drift was always in the same
 * direction: a program declared and not staged, which is a feature that
 * can only fail at execve() on an installed host. mkfs.btrfs did it
 * once (found by an operator asking for btrfs), `cp` did it in reverse,
 * and those tables also carried a BUILD-HOST fallback path for every
 * entry -- which on an installed host is the control-plane root itself,
 * so each assembly copied the previous root's copy forward and whatever
 * a dev machine first put there stayed forever (cix#589). There is one
 * list now and one source, so neither is expressible.
 *
 * X(id, path, source, pkg):
 *   source -- which assembly input supplies it.
 *     CP_BUILD      this build tree, beside cixd (not a package).
 *     CP_HOSTTOOLS  the cix-hosttools image's installed rootfs
 *                   (ADR-0078), at this same absolute path.
 *     CP_KMOD       the kmod image's usr/bin, which is optional (#429).
 *   pkg -- the package that provides it, so a root missing a program
 *     can say which install fixes it. Measured on 192.168.15.95,
 *     2026-10-10, from the daemon's own per-package file lists for the
 *     21 packages installed in cix-hosttools; every path below is
 *     present there, which is why CP_HOSTTOOLS needs no fallback.
 *     "" where no package supplies it.
 */
#define CONTROLPLANE_PROGRAMS(X)                                                                   \
	/* the console's shell (spawn_console_shell() in cixd) */                                   \
	X(CIXCTL, "/bin/cixctl", CP_BUILD, "")                                                      \
	/* image content, and mkbootroot's own seal when it runs on this root */                   \
	X(UNSQUASHFS, "/usr/bin/unsquashfs", CP_HOSTTOOLS, "squashfs-tools")                        \
	X(MKSQUASHFS, "/usr/bin/mksquashfs", CP_HOSTTOOLS, "squashfs-tools")                        \
	/* the CPDL engine: publish, package, extract (ADR-0307 clause 6) */                        \
	X(CBS, "/usr/bin/cbs", CP_HOSTTOOLS, "cbs")                                                 \
	/* disks and partitions */                                                                  \
	X(SFDISK, "/usr/sbin/sfdisk", CP_HOSTTOOLS, "util-linux")                                   \
	X(E2FSCK, "/usr/sbin/e2fsck", CP_HOSTTOOLS, "e2fsprogs")                                    \
	X(RESIZE2FS, "/usr/sbin/resize2fs", CP_HOSTTOOLS, "e2fsprogs")                              \
	X(BTRFS, "/usr/sbin/btrfs", CP_HOSTTOOLS, "btrfs-progs")                                    \
	X(MKFS_EXT4, "/usr/sbin/mkfs.ext4", CP_HOSTTOOLS, "e2fsprogs")                              \
	X(MKFS_BTRFS, "/usr/sbin/mkfs.btrfs", CP_HOSTTOOLS, "btrfs-progs")                          \
	/* kernel modules */                                                                        \
	X(MODPROBE, "/usr/bin/modprobe", CP_KMOD, "kmod")                                           \
	X(MODINFO, "/usr/bin/modinfo", CP_KMOD, "kmod")

enum controlplane_program_source { CP_BUILD, CP_HOSTTOOLS, CP_KMOD };

#endif
