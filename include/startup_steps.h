#ifndef CIX_STARTUP_STEPS_H
#define CIX_STARTUP_STEPS_H

/*
 * ADR-0375 phase 1: the startup steps that run once per KERNEL, not
 * once per cixd.
 *
 * cixd_main() runs every other step again when it starts with
 * --handover (a re-exec in the same kernel): the state loads are reads,
 * and the steps that touch the kernel were each checked for running
 * twice -- the disk remount skips a mounted disk, bridges tolerate
 * EEXIST, the controller write and the move into cix.daemon are
 * idempotent, and the listeners' ports are free because their sockets
 * are CLOEXEC. These do not run again: each prepares a kernel that has
 * just started, or acts once per boot. Each is called only on a
 * first_in_kernel() line, and test_startup_steps holds the code to this
 * list, which is why it is a table and not a comment.
 *
 *   boot_init                     the kernel mounts, /config, the data directory
 *   load_boot_modules             the NIC drivers the boot needs
 *   apply_configured_sysctls      sysctls persist in the kernel
 *   bootstrap_management_network  the address persists on the interface
 *   kmodscan_boot_sweep           the boot's module autoload (ADR-0348)
 *   load_configured_modules       likewise
 *   maybe_confirm_boot            the boot counter; a live switch confirms
 *                                 after its own probation (phase 4)
 *   spawn_console_shell           the shells are cixd's children and
 *                                 survive the exec
 */
#define STARTUP_ONCE_PER_KERNEL(X)                                                                 \
	X(boot_init)                                                                               \
	X(load_boot_modules)                                                                       \
	X(apply_configured_sysctls)                                                                \
	X(bootstrap_management_network)                                                            \
	X(kmodscan_boot_sweep)                                                                     \
	X(load_configured_modules)                                                                 \
	X(maybe_confirm_boot)                                                                      \
	X(spawn_console_shell)

/*
 * And the one step a handover skips because its process survives the
 * exec: procfuse's server keeps serving its mount, and starting a second
 * would mount over it. Until phase 3 hands its load page over as a
 * memfd, a handed-over cixd reports no per-container load.
 */
#define STARTUP_SURVIVES_HANDOVER(X) X(procfuse_start)

#endif
