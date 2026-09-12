#ifndef PARTLABEL_H
#define PARTLABEL_H

#include <stddef.h>

/*
 * Finding the platform's own partitions by the labels the installer
 * wrote, instead of by a device path compiled into the binary.
 *
 * cix-install.c writes a GPT whose five partitions are NAMED --
 * cix-esp, cix-root-a, cix-root-b, cix-config, cix-containers -- and
 * every consumer of that layout then ignored the names and hardcoded
 * "/dev/vda1".."/dev/vda5". That works exactly as long as the disk
 * enumerates the way the developer's VM did.
 *
 * It does not always. Move the same disk from virtio to SATA and it
 * becomes sda; the loader still says root=/dev/vda2, the kernel finds
 * sda1..sda5, and the machine panics in a reboot loop with a correct,
 * fully-installed system underneath it. Measured on 192.168.15.95 when
 * the owner deliberately changed the disk configuration to see whether
 * the platform would cope. It did not.
 *
 * The disk has always carried the answer. This reads it.
 *
 * Deliberately dependency-free -- libc and nothing else -- because the
 * callers are the daemon, cix-install and cix-recover, and the last two
 * are small static tools that must not grow a link closure to ask a
 * question this simple. It is also why the GPT is parsed here directly
 * rather than shelling out to a partition tool: this runs as pid 1
 * before any of them are reachable.
 */

/*
 * The GPT partition name for partition <partno> (1-based) of the whole
 * disk at <parent_dev_path>. out is set to "" when there is no GPT, no
 * such entry, or the device cannot be read -- never left uninitialised.
 *
 * Only the ASCII range is decoded, which every name this platform
 * produces is; a non-ASCII code unit renders as '?' rather than
 * silently truncating the name to nothing.
 */
void partlabel_read(const char *parent_dev_path, unsigned int partno, char *out, size_t out_size);

/*
 * The whole-disk name a partition's kernel name belongs to:
 * "sda4" -> "sda", "nvme0n1p2" -> "nvme0n1", "vda" -> "vda".
 */
void partlabel_parent_name(const char *part_name, char *out, size_t out_size);

/*
 * The whole-disk name the RUNNING ROOT filesystem lives on -- "vdb" --
 * or -1 with out emptied when it cannot be determined.
 *
 * This is the tie-break #427 needed. A GPT label is not unique: attach
 * a second Cix disk, or a clone of this one, and two partitions carry
 * cix-config. The label itself says nothing about which of them the
 * platform belongs to, and partlabel_find() below used to answer with
 * whichever readdir(3) happened to yield first -- an answer that can
 * differ between two boots of the same machine.
 *
 * The kernel already decided, though. It mounted one specific partition
 * as /, and cix-install writes all five platform partitions on a single
 * disk, so the disk carrying / is the disk carrying the other four.
 * This reads that decision back instead of repeating the lookup that
 * made it: stat("/") names the device the root filesystem is on, and
 * /sys/dev/block/<major>:<minor> is a symlink whose basename is that
 * device's kernel name. Measured in this sandbox, 2026-09-12:
 * /sys/dev/block/259:1 -> ../../devices/.../nvme0n1/nvme0n1p1.
 *
 * Deliberately NOT root=PARTUUID= out of /proc/cmdline. Resolving a
 * PARTUUID means scanning every disk and comparing GUIDs, which has the
 * identical first-match ambiguity one level down -- a cloned disk
 * duplicates PARTUUIDs too -- so it would narrow the problem without
 * closing it. This platform also has no initramfs at all
 * (image/src/mkinstalleriso.c:14), so / is the real root partition
 * rather than something pivoted away from one.
 *
 * Whether that device is a partition or a whole disk is the kernel's
 * answer too -- whether it has its own "partition" file -- never a
 * guess from the name. Name-stripping alone is actively wrong here:
 * this sandbox's root is the dm volume dm-71, and
 * partlabel_parent_name("dm-71") is "dm-", a disk that does not exist
 * (both measured 2026-09-12).
 */
int partlabel_root_disk(char *out, size_t out_size);

/*
 * Resolve a GPT partition label to the device path holding it, by
 * walking /sys/class/block and reading each partition's own GPT entry.
 *
 * Returns 0 with "/dev/<name>" in out, or -1 when no partition on any
 * disk carries that label. A -1 is a real answer -- the caller reports
 * it rather than falling back to a guess, because a guess is what this
 * module exists to remove.
 *
 * Two passes, in this order (#427):
 *
 *   1. the disk the running root is on, from partlabel_root_disk()
 *      above -- the platform's own five partitions are all on it by
 *      construction, so a label found there is the right one even when
 *      another disk carries the same label;
 *   2. every disk, which is what this did before the first pass
 *      existed.
 *
 * The second pass is not a stop-gap, it is load-bearing in two real
 * cases. cix-recover runs as pid 1 from ISO media, where / is on the
 * optical device and no platform label is on that disk at all; and a
 * root that is not on a block device has no disk to scope to. Both
 * must keep resolving, so this never returns -1 where the all-disks
 * scan alone would have returned 0. Reaching the second pass with two
 * disks matching is still first-match -- by then there is nothing left
 * to prefer.
 *
 * Requires /sys mounted; boot_init() mounts it well before the first
 * call. Reads only, and opens each disk O_RDONLY.
 */
int partlabel_find(const char *label, char *out, size_t out_size);

/*
 * partlabel_find()'s scan with its three implicit inputs made explicit:
 * <sys_class_block> is the directory enumerated, <dev_dir> the
 * directory the returned device path is built in, and <only_disk> the
 * whole-disk name to restrict the search to -- NULL searches every
 * disk.
 *
 * It exists so the scoping rule can be tested at all. The real scan
 * walks /sys/class/block and this sandbox has no block device nodes, so
 * a test against the real paths can only ever assert refusals -- which
 * is all test_partlabel.c could assert before this existed. With the
 * paths as parameters, two crafted GPTs carrying the SAME label can be
 * put in front of it and the tie-break actually checked.
 *
 * Production code calls partlabel_find(), which owns the policy of
 * which disk to prefer; this is for that function and for the test.
 * Same shape and same reason as disk.h's disk_fill_mount_status_from().
 */
int partlabel_find_in(const char *sys_class_block, const char *dev_dir, const char *only_disk,
                      const char *label, char *out, size_t out_size);

/*
 * The unique partition GUID (PARTUUID) for partition <partno> of the
 * disk at <parent_dev_path>, formatted the way the kernel prints and
 * parses it: 8-4-4-4-12 lowercase hex.
 *
 * This exists because a GPT LABEL cannot answer the one question that
 * matters most. The kernel's own root= parser understands /dev paths
 * and PARTUUID= and does NOT understand PARTLABEL=, so the loader
 * entry -- the single line that decides whether the machine boots at
 * all -- has to name the partition by UUID. cix-install wrote
 * root=/dev/vda2 from a compiled-in prefix, and a disk that came back
 * as sda turned a healthy install into a panic loop (#305).
 *
 * The first three fields are little-endian in the binary and big-endian
 * in the text, per the GPT/EFI spec; the last two are byte order as
 * stored. Getting that backwards produces a plausible-looking UUID that
 * matches nothing, so it is asserted in test_partlabel.c against a
 * known entry rather than eyeballed.
 *
 * out is set to "" on any failure -- never left holding a partial UUID
 * that a caller might write into a boot entry.
 */
void partlabel_read_uuid(const char *parent_dev_path, unsigned int partno, char *out,
                         size_t out_size);

/*
 * The PARTUUID of a partition named by its own device path
 * ("/dev/sda2"), which is the form callers actually hold: the
 * installer has already resolved cix-root-a to a device, and the
 * daemon knows which slot it is writing an entry for.
 *
 * Splits the parent disk and partition number out of the name and
 * reads that entry's GUID. Returns 0 on success, -1 on any failure
 * with out emptied -- a half-answer here would be written into a boot
 * entry, so there is no partial success.
 */
int partlabel_uuid_for_device(const char *part_dev_path, char *out, size_t out_size);

#endif
