#ifndef BOOTMODULES_H
#define BOOTMODULES_H

/*
 * The NIC drivers this platform carries as kernel modules, in one
 * place, because two programs need the identical list.
 *
 * cixd loads them at boot (load_boot_modules(), before
 * bootstrap_management_network() attaches a named interface, which the
 * kernel must already have detected). cix-install loads them before it
 * lists interfaces for the operator to choose from -- without that, a
 * real machine's built-in Ethernet is simply absent from the installer
 * screen, which is how a bare-metal install was blocked on 2026-09-12:
 * the drivers are =m, the installer ISO carried no module tree, so only
 * built-in drivers (virtio_net) ever produced an interface.
 *
 * Shared rather than copied deliberately. These are the same five
 * chipsets for the same reason in both places, and a list that had
 * drifted would fail in the worst possible way: the installer would
 * offer an interface the installed system then cannot bring up, or the
 * installed system would find a NIC the installer never let anyone
 * choose.
 *
 * Deliberately NOT the full set of modules cixd loads. ehci-hcd and
 * usb-storage were in that list and are built into the kernel as of
 * #429, so they are not module loads at all any more -- and they were
 * never needed to enumerate a NIC. This header is the NIC list, and
 * cixd keeps whatever else it wants alongside it.
 *
 * BNX2 is absent for a recorded reason rather than an oversight: the
 * kernel config excludes it because it cannot function at all without
 * a firmware blob loaded into the card first, and staging real
 * linux-firmware blobs is separate scope. A machine with only a
 * NetXtreme II is therefore still unserved here, which is an honest
 * gap, not a silent one.
 *
 * `igc` joins the list with #479, and its absence was the other kind
 * of gap -- not recorded anywhere, just missing. Intel I225/I226 has
 * been standard on consumer and small-server boards since roughly
 * 2021, and `CONFIG_IGC` was not in the kernel config at all, so a
 * machine with one had no Ethernet in the installer or afterwards.
 * Adding it here and adding the symbol there are one change: this
 * header exists precisely so the two lists cannot drift.
 */
#define CIX_NIC_MODULES                                                                            \
	{                                                                                              \
		"e1000e", "igb", "igc", "ixgbe", "r8169", "tg3"                                            \
	}

#endif
