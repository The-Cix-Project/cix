#ifndef NICREPORT_H
#define NICREPORT_H

#include <stddef.h>

/*
 * What the installer tells an operator when its network-interface list
 * comes back with no real NIC on it (#442).
 *
 * This exists as its own translation unit for one reason: the sentence
 * chosen here is the whole diagnosis, and it was wrong in a way nobody
 * could see. cix-install printed, on screen, above the list:
 *
 *   "a built-in Ethernet port may not be listed below even though this
 *    machine has one -- its driver is a kernel module and this
 *    installer carries no module tree."
 *
 * True when written, false since the ISO started staging the five NIC
 * drivers and modprobe (2026-09-12, #429). So a real bare-metal
 * install that listed only `lo` came with a printed explanation of a
 * mechanism that no longer existed, the reporter read the code, found
 * the staging present, and could not reconcile the two -- which is
 * exactly how #442 came to say "cause not established".
 *
 * The classification is therefore the part worth gating, and
 * cix-install cannot be run by a test (it is pid 1 on real media and
 * formats a disk). test_installer is not in SELFTESTS either. So the
 * decision lives here, where test_nicreport exercises every state, and
 * the caller only prints what it returns.
 *
 * MEASURED, and the whole classification rests on it: on
 * 192.168.15.95 (a virtio VM with no Broadcom hardware),
 * `cixctl kmod load tg3` returns 0 and the module goes Live with
 * used_by=0 -- no interface appears. So **modprobe succeeds on a
 * machine that does not have the chipset**, and with a correct module
 * tree all five loads return 0 everywhere. A non-zero load is
 * therefore never "ordinary": it means the tree is missing, is for
 * another kernel release, or was built from another config. The code
 * that discarded that text called those failures ordinary and expected.
 */

/* What load_nic_modules() actually managed to do. */
struct nic_load_result {
	int tools_present; /* modprobe was on the media at all */
	int attempted;     /* how many modules it tried */
	int loaded;        /* how many returned success */
	int failed;        /* how many returned an error */
	char first_error[256]; /* the first real error text, "" if none */
};

/*
 * The line(s) to print when the interface list found no real NIC.
 * Writes into out and returns it; never NULL, and ends without a
 * trailing newline so the caller controls layout.
 *
 * nics_found is what the /sys/class/net scan turned up, excluding lo.
 * It is meant to be called only when that is 0; called with a NIC in
 * hand it returns the EMPTY string deliberately, because printing an
 * explanation next to a list that contradicts it is the #442 failure
 * over again in the other direction.
 */
const char *nicreport_no_nic_reason(const struct nic_load_result *r, int nics_found, char *out,
                                     size_t out_size);

#endif /* NICREPORT_H */
