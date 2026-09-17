/*
 * #442: why the installer's interface list came back empty. See
 * include/nicreport.h for what this is separate for.
 */
#include "nicreport.h"
#include "bootmodules.h"

#include <stdio.h>
#include <string.h>

/*
 * The five names, rendered for a human, so the "your NIC is not one of
 * these" case names them instead of asking the operator to guess what
 * the platform supports.
 */
static void supported_list(char *out, size_t out_size)
{
	static const char *const mods[] = CIX_NIC_MODULES;
	size_t i, used = 0;

	if (out_size == 0)
		return;
	out[0] = '\0';
	for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
		used = strlen(out);
		if (used + strlen(mods[i]) + 3 >= out_size)
			return;
		snprintf(out + used, out_size - used, "%s%s", used > 0 ? ", " : "", mods[i]);
	}
}

const char *nicreport_no_nic_reason(const struct nic_load_result *r, int nics_found, char *out,
                                     size_t out_size)
{
	char mods[128];

	if (out == NULL || out_size == 0)
		return "";
	if (r == NULL) {
		snprintf(out, out_size, "no real NIC visible, and nothing recorded why -- "
		                        "this is a bug in the installer, not in this machine");
		return out;
	}
	/*
	 * A caller with a NIC in hand has nothing to explain, and saying
	 * something anyway would be the #442 failure again in the other
	 * direction: an explanation printed next to a list that contradicts
	 * it.
	 */
	if (nics_found > 0) {
		out[0] = '\0';
		return out;
	}

	/*
	 * Ordered by what the operator can do about it, most actionable
	 * first.
	 */
	if (!r->tools_present) {
		snprintf(out, out_size,
		         "no real NIC visible, and this media carries no module tools, so the NIC\n"
		         "  drivers could not be loaded at all. Only a driver built into the kernel\n"
		         "  could appear here. This is a defect in the media, not in this machine --\n"
		         "  the ISO is built with modprobe staged, so an ISO without it was built on\n"
		         "  a host with no cix-kmod image. Install on `lo` and set the real interface\n"
		         "  afterwards with `cixctl management-network set`.");
		return out;
	}
	if (r->failed > 0) {
		/*
		 * The text modprobe itself produced, verbatim. It is the only
		 * thing that distinguishes "no module tree for this kernel
		 * release" from "a tree built from another config", and
		 * discarding it is what left #442 with no cause.
		 */
		snprintf(out, out_size,
		         "no real NIC visible, and %d of %d driver loads FAILED. modprobe said:\n"
		         "  %s\n"
		         "  A tree for another kernel release reads as \"module not found\"; one built\n"
		         "  from another config reads as \"invalid module format\". Either way the media\n"
		         "  is at fault, not this machine. Install on `lo` and set the real interface\n"
		         "  afterwards with `cixctl management-network set`.",
		         r->failed, r->attempted,
		         r->first_error[0] != '\0' ? r->first_error : "(no message captured)");
		return out;
	}
	/*
	 * Every load succeeded and no interface appeared. This is the one
	 * case that really is about the machine, and it is worth stating
	 * plainly rather than leaving the operator to suspect the media:
	 * modprobe returning 0 means the driver is loaded, not that it
	 * found anything to drive.
	 */
	supported_list(mods, sizeof(mods));
	snprintf(out, out_size,
	         "no real NIC visible. All %d drivers loaded cleanly, so the media is fine and\n"
	         "  this machine's Ethernet is not one of them. Supported: %s.\n"
	         "  Known gaps: Broadcom NetXtreme II (bnx2, needs a firmware blob) and Intel\n"
	         "  I225/I226 2.5G (igc, not in the kernel config at all). Install on `lo` and\n"
	         "  set the real interface afterwards with `cixctl management-network set`.",
	         r->loaded, mods);
	return out;
}
