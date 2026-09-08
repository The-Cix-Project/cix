#ifndef CIX_NL80211_H
#define CIX_NL80211_H

#include <sys/types.h>

/*
 * Moving a radio into a container (#341).
 *
 * A wireless interface cannot be moved between network namespaces the
 * way every other netdev can. `rtnl_link_set_netns_pid()` on wlan0
 * returns EINVAL: a wireless netdev belongs to a *wiphy*, one wiphy can
 * own several netdevs, and the kernel will not let one of them leave on
 * its own. The whole PHY moves or nothing does, and the operation that
 * moves it is NL80211_CMD_SET_WIPHY_NETNS -- which is what
 * `iw phy <phy> set netns <pid>` issues.
 *
 * That command lives in the nl80211 family, which is GENERIC netlink:
 * a different protocol (NETLINK_GENERIC) with a family id that is not
 * a compile-time constant and has to be resolved at runtime by asking
 * the controller for it by name. Everything else this platform does
 * with the kernel is rtnetlink, where the ids are fixed. That is the
 * whole reason this is a separate file rather than three more
 * functions in rtnetlink.c.
 *
 * The semantics are worth stating plainly because they are not the
 * same as a netdev move: EVERY interface on the wiphy goes with it, and
 * the host loses the radio entirely until the container is deleted.
 * For a dedicated access-point adapter that is exactly what is wanted.
 * For a radio the host is also using, it is not, and this platform
 * offers no way to share one -- because the kernel does not.
 */

/*
 * Is this interface backed by a wiphy?
 *
 * Answered from sysfs (/sys/class/net/<ifname>/phy80211) rather than by
 * asking nl80211, because the caller needs to know which of two
 * mechanisms to use BEFORE opening a socket for either, and a missing
 * directory is a cheaper and less ambiguous answer than a netlink
 * round trip that could fail for unrelated reasons.
 *
 * Returns 1 for wireless, 0 for anything else including a name that
 * does not exist -- "not wireless" is the right answer for a missing
 * interface here, since the rtnetlink path will then fail on it and
 * report the real problem.
 */
int nl80211_is_wireless(const char *ifname);


/*
 * Moves the wiphy that owns ifname into the namespace named by an open
 * fd (as from /proc/<pid>/ns/net).
 *
 * An fd and not a pid, in both directions, deliberately. nl80211 accepts
 * either -- NL80211_ATTR_PID resolves the pid to a namespace kernel-side
 * -- and the pid form asks the kernel to re-derive something the caller
 * already holds exactly. Every caller here has, or can trivially open,
 * the fd: going in, container_net_host_attach_interfaces() opens
 * /proc/<pid>/ns/net before it does anything else; coming back out, the
 * teardown helper holds an fd for the host's namespace and has no pid it
 * could name without assuming something about its own parent. One
 * spelling, no ambiguity about whose pid namespace a number is read in.
 *
 * Opens its own socket, resolves the nl80211 family, and closes
 * everything before returning -- unlike the rtnetlink calls, which take
 * a caller-owned fd. This is a once-per-container operation on a
 * lightly-used family, so keeping the socket's lifetime inside the one
 * call it serves is worth more than saving the open.
 *
 * Returns 0, or -1 with errno set from the kernel's own netlink error.
 */
int nl80211_move_phy_to_netns_fd(const char *ifname, int netns_fd);

#endif /* CIX_NL80211_H */
