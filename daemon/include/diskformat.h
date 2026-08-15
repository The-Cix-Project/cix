#ifndef DISKFORMAT_H
#define DISKFORMAT_H

#include "json.h"

#include <sys/types.h>

/*
 * Multi-disk management Phase C: destructive format + mount of a disk
 * already given a role by Phase B (diskrole.h). Deliberately a
 * SEPARATE, explicit action from role assignment -- confirmed with the
 * user -- so assigning a role never has a destructive side effect, and
 * formatting always requires the operator to name the exact disk again
 * (main.c's REST handler enforces the confirm_disk_name == path name
 * check; this module enforces the domain invariants: the disk must
 * currently exist, must not be the OS disk, and must already have an
 * assigned role).
 *
 * mkfs.ext4 is a real external tool invocation that can take real time
 * on a large disk -- this daemon's reactor is single-threaded and
 * non-blocking (ADR-0009), so it's run the same fork+pidfd+epoll async
 * way every other potentially-slow host operation already is
 * (iso_build_start(), pkg.c's own start_fetch_for(), main.c's
 * spawn_thinc_bootroot_assembly()).
 *
 * Unlike every one of those precedents, though, this job is NOT a
 * single execve(): format+mount is two sequential steps (an external
 * "mkfs.ext4" invocation, then a raw mount(2) syscall), and execve()
 * irreversibly replaces its caller's entire process image -- there is
 * no way to "return" from it and run more C logic afterward. The
 * forked child this module starts does NOT execve() itself; instead it
 * fork()+execve()s mkfs.ext4 as its own grandchild (the same
 * fork-a-grandchild-per-step shape pkg.c's own start_fetch_for()
 * already uses for its sequential multi-source curl fetches), waits
 * for it, and only on success calls mount(2) directly before exiting --
 * a plain fork() (no CLONE_NEWNS) shares the parent's mount namespace,
 * so that mount(2) becomes visible daemon-wide the instant it succeeds,
 * no namespace handling needed.
 *
 * Only one format job may be in flight at a time (the same v1 single-
 * job constraint this daemon's other async jobs -- pkg install/
 * hostbuild, ISO assembly, bootstrap fetch -- already have).
 */

#define DISKFORMAT_MKFS_EXT4_BIN "/usr/sbin/mkfs.ext4"

/*
 * btrfs support (ADR-0104, closes task #732 -- the disk-*format*-time
 * half of the ext4-vs-btrfs gap; ADR-0103/task #678 already closed the
 * *quota enforcement* half). mkfs.btrfs is genuinely optional: unlike
 * mkfs.ext4 (staged into every assembled boot image unconditionally,
 * mkbootroot.c), it's only staged when a real thinc-hosttools image
 * built with btrfs-progs.recipe is available -- an install without one
 * simply can't format a disk btrfs (ENOENT at exec time, surfaced as a
 * normal DISKFORMAT_STATE_FAILED, not a crash or a silently-ignored
 * request).
 */
#define DISKFORMAT_MKFS_BTRFS_BIN "/usr/sbin/mkfs.btrfs"

enum diskformat_fs_type {
	DISKFORMAT_FS_EXT4 = 0, /* the default -- matches every fs_type-less request from before this field existed */
	DISKFORMAT_FS_BTRFS,
};

enum diskformat_error {
	DISKFORMAT_OK = 0,
	DISKFORMAT_ERR_INVALID_DISK_NAME,
	DISKFORMAT_ERR_NOT_FOUND,
	DISKFORMAT_ERR_IS_OS_DISK,
	DISKFORMAT_ERR_NO_ROLE,
	DISKFORMAT_ERR_BUSY,
	DISKFORMAT_ERR_MKDIR_FAILED,
	DISKFORMAT_ERR_SPAWN_FAILED,
};

enum diskformat_state {
	DISKFORMAT_STATE_NONE,    /* no format job has ever run this daemon lifetime */
	DISKFORMAT_STATE_RUNNING,
	DISKFORMAT_STATE_READY,
	DISKFORMAT_STATE_FAILED,
};

/*
 * Validates disk_name (must currently resolve to a real disk via
 * disk_enumerate(), must not be that enumeration's is_os_disk, must
 * have an assigned role per diskrole_lookup()) and, on success, forks
 * the two-step job described above targeting "<mount_base_dir>/
 * <disk_name>" (created here, synchronously, before forking -- a
 * plain single-level mkdir, DISKFORMAT_ERR_MKDIR_FAILED if it fails).
 *
 * On DISKFORMAT_OK, *out_pid/*out_pidfd identify the forked child for
 * the caller (main.c) to register with its own epoll reactor and
 * later waitpid() -- mirrors pkg.c's own start_fetch_for() convention;
 * this module has no epoll/conn knowledge of its own. State is now
 * DISKFORMAT_STATE_RUNNING (diskformat_write_status_json() reports
 * it); every other error is a synchronous rejection, no job started.
 *
 * fs_type selects which mkfs tool the forked child execve()s and which
 * fstype string the final mount(2) uses -- DISKFORMAT_FS_EXT4 (the
 * default) is byte-for-byte the same job this function has always
 * started; DISKFORMAT_FS_BTRFS is new (ADR-0104).
 */
enum diskformat_error diskformat_start(const char *disk_name, const char *os_containers_dir,
                                        const char *mount_base_dir, enum diskformat_fs_type fs_type,
                                        pid_t *out_pid, int *out_pidfd);

/*
 * Called by main.c after waitpid() on the child diskformat_start()
 * returned; exit_status is the raw WEXITSTATUS() (or -1 if the child
 * could not be reaped / did not exit normally). Updates internal
 * state to DISKFORMAT_STATE_READY or DISKFORMAT_STATE_FAILED for
 * diskformat_write_status_json() to report; -1 and every nonzero
 * status is FAILED, with a distinct message per the child's own exit
 * code convention (1 = mkfs.ext4 failed, 2 = mount(2) failed, other =
 * the child process itself could not be set up).
 */
void diskformat_completed(int exit_status);

/*
 * Reports the most recent (or currently running) job: disk_name,
 * state, mount_path (once RUNNING or READY), and error (once FAILED).
 * Writes {"state":"none"} if no job has ever run. want_disk_name, if
 * non-NULL, must match the tracked job's own disk_name or this writes
 * {"state":"none"} instead -- callers asking about a specific disk (a
 * GET on that disk's own format endpoint) should never see another
 * disk's unrelated job status.
 */
void diskformat_write_status_json(struct json_writer *w, const char *want_disk_name);

/*
 * ADR-0142: called once at daemon startup (main.c), after diskrole_init()
 * but before this daemon starts serving requests. Walks every disk
 * disk_enumerate() currently reports and, for each one that is present,
 * not the OS disk, not already mounted, has a role assigned, and has a
 * remembered fs_type (diskrole_lookup_fs_type() -- i.e. it was
 * successfully formatted at some point, possibly a prior daemon
 * lifetime), mounts it directly with that same fs_type at
 * "<mount_base_dir>/<disk_name>" -- a plain mount(2), never a second
 * mkfs, since the filesystem already exists. Closes the real gap found
 * live: this job's own DISKFORMAT_STATE_READY is purely in-memory and
 * forgotten across every restart, even though the real mount (and the
 * filesystem on disk) both survive fine -- without this, a formatted
 * disk stays unmounted forever after the first reboot with no operator
 * action able to fix it short of a destructive re-format.
 *
 * A disk that fails to remount (corrupted filesystem, physically
 * failing) is logged and left unmounted -- never a second automatic
 * format attempt, which would be destructive. Best-effort and
 * non-fatal to daemon startup either way; this never returns an error
 * a caller needs to act on.
 */
void diskformat_remount_present_role_disks(const char *os_containers_dir, const char *mount_base_dir);

#endif /* DISKFORMAT_H */
