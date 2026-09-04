#ifndef CIX_CONTAINERPATH_H
#define CIX_CONTAINERPATH_H

#include <stddef.h>

/*
 * Where the DAEMON writes a file that belongs to a container (#269).
 *
 * Every subsystem that renders state into a running container -- DNS
 * hosts, DHCP leases, glauth users, an issued certificate, a hot-plugged
 * device node -- used to build its own "/proc/<pid>/root<path>" and write
 * through the container's own view of its filesystem. That worked while
 * every container's rootfs carried ordinary ownership, and stopped
 * working the moment one did not.
 *
 * A btrfs-backed userns container's rootfs is presented through an
 * ID-MAPPED MOUNT (ADR-0207 phase 3): the tree stays owned by host uid 0
 * on disk and the mount translates it to the container's own root. The
 * daemon writing through that same mount has no mapped identity there,
 * so the write is refused -- and every one of those call sites discarded
 * the result, so the failure was silent. Measured: both DNS servers held
 * 0-byte hosts files while four records were registered and forwarding
 * worked perfectly, so nothing looked wrong.
 *
 * The fix is to write to the container's tree on the HOST side, which is
 * the natural path rather than a way around one: under the id-mapped
 * presentation on-disk uid 0 is exactly what the container sees as its
 * own root, so a file the daemon writes as root arrives owned by root
 * inside. It is also the same path creation-time staging already uses,
 * which is why staging never had this problem.
 *
 * rel_path is absolute in the CONTAINER's namespace ("/etc/hosts").
 * disk_name may be empty for a container on the default OS disk.
 */
void container_file_host_path(const char *container_name, const char *disk_name,
                              const char *rel_path, char *out, size_t out_size);

#endif /* CIX_CONTAINERPATH_H */
