#include "internal.h"

#include <errno.h>
#include <string.h>
#include <sys/prctl.h>

/*
 * Real capability bit numbers (Linux ABI, stable since capabilities were
 * introduced -- not expected to change): self-declared as named constants
 * here rather than pulling in <linux/capability.h>, same "named constant,
 * not a bare header dependency" posture container_dev.c's own BPF opcode
 * constants already take. Only the ones this file actually references are
 * named; see capabilities(7) for the full canonical list.
 */
#define CIX_CAP_SYS_MODULE       16
#define CIX_CAP_SYS_RAWIO        17
#define CIX_CAP_SYS_PTRACE       19
#define CIX_CAP_SYS_PACCT        20
#define CIX_CAP_SYS_ADMIN        21
#define CIX_CAP_SYS_BOOT         22
#define CIX_CAP_SYS_RESOURCE     24
#define CIX_CAP_SYS_TIME         25
#define CIX_CAP_SYS_TTY_CONFIG   26
#define CIX_CAP_LINUX_IMMUTABLE  9
#define CIX_CAP_AUDIT_CONTROL    30
#define CIX_CAP_MAC_OVERRIDE     32
#define CIX_CAP_MAC_ADMIN        33
#define CIX_CAP_SYSLOG           34
#define CIX_CAP_WAKE_ALARM       35
#define CIX_CAP_BLOCK_SUSPEND    36
#define CIX_CAP_AUDIT_READ       37
#define CIX_CAP_PERFMON          38
#define CIX_CAP_BPF              39
#define CIX_CAP_CHECKPOINT_RESTORE 40

/*
 * Default deny-list: real, actively dangerous capabilities no ordinary
 * service (dns/ldap/syslog/jumpbox, and every future recipe unless it
 * opts in) genuinely needs. Deliberately a deny-list, not an allow-list --
 * an allow-list would need an exhaustive per-service audit before this
 * could ship at all; a short, named deny-list of the capabilities that
 * actually matter (issue #29's own CAP_SYS_MODULE finding chief among
 * them) ships today and only grows the handful of real opt-ins as they're
 * found, matching "keep it simple, ship it, harden by exception" over
 * "block on a perfect audit."
 *
 * CAP_SYS_MODULE: kernel module loading -- issue #29's own finding.
 * Modules aren't namespaced, so this alone is a straight line from any
 * container's root process to arbitrary host-kernel code, no exploit
 * needed. The single most important entry here.
 * CAP_SYS_ADMIN: the historical "everything else" grab-bag -- a wide
 * range of mount/namespace/admin operations, several of them well-known
 * container-breakout vectors on their own.
 * CAP_SYS_PTRACE, CAP_SYS_RAWIO, CAP_SYS_BOOT: process introspection,
 * raw I/O/memory access, reboot/kexec -- none legitimate for a service
 * container.
 * CAP_SYS_TIME: clock_settime() -- genuinely needed by chrony/NTP
 * containers specifically, hence the cap_add opt-in exists.
 * CAP_SYS_RESOURCE, CAP_SYS_TTY_CONFIG, CAP_LINUX_IMMUTABLE: resource-
 * limit/quota bypass, host tty reconfiguration, immutable-bit bypass --
 * narrow, rarely-if-ever legitimate for a service container.
 * CAP_MAC_OVERRIDE/CAP_MAC_ADMIN: LSM policy override -- this project
 * runs no LSM (no seccomp/AppArmor/SELinux, confirmed directly), so these
 * bits could only ever matter on a future host that adds one; dropped now
 * so that addition doesn't silently do nothing.
 * CAP_SYSLOG: kernel ring buffer/dmesg read -- minor host information
 * leak, no service here needs it.
 * CAP_WAKE_ALARM/CAP_BLOCK_SUSPEND/CAP_AUDIT_CONTROL/CAP_AUDIT_READ:
 * power-management and kernel audit-subsystem control -- host-wide
 * concerns, never a per-service one.
 * CAP_PERFMON/CAP_BPF: modern kernel-introspection/BPF-program-loading
 * capabilities -- CAP_BPF in particular can be as powerful as
 * CAP_SYS_ADMIN for certain BPF program types on a newer kernel.
 * CAP_CHECKPOINT_RESTORE: CRIU-class process checkpoint/restore --
 * no legitimate use inside a service container.
 *
 * Deliberately NOT on this list (kept by default): CAP_NET_ADMIN/
 * CAP_NET_RAW (routing/interface config and raw sockets, e.g. jumpbox's
 * own `ping`) and CAP_SETUID/CAP_SETGID (sshd's own privilege-separation
 * and per-session user drop, confirmed live via jumpbox1's real cmd) --
 * both real, in-use needs on this project's own containers today,
 * confirmed by inspecting each live container's actual cmd/config before
 * writing this list, not assumed.
 */
static const int cix_default_deny[] = {
	CIX_CAP_SYS_MODULE,
	CIX_CAP_SYS_ADMIN,
	CIX_CAP_SYS_PTRACE,
	CIX_CAP_SYS_RAWIO,
	CIX_CAP_SYS_BOOT,
	CIX_CAP_SYS_TIME,
	CIX_CAP_SYS_RESOURCE,
	CIX_CAP_SYS_TTY_CONFIG,
	CIX_CAP_LINUX_IMMUTABLE,
	CIX_CAP_SYS_PACCT,
	CIX_CAP_MAC_OVERRIDE,
	CIX_CAP_MAC_ADMIN,
	CIX_CAP_SYSLOG,
	CIX_CAP_WAKE_ALARM,
	CIX_CAP_BLOCK_SUSPEND,
	CIX_CAP_AUDIT_CONTROL,
	CIX_CAP_AUDIT_READ,
	CIX_CAP_PERFMON,
	CIX_CAP_BPF,
	CIX_CAP_CHECKPOINT_RESTORE,
};
#define CIX_DEFAULT_DENY_COUNT (int)(sizeof(cix_default_deny) / sizeof(cix_default_deny[0]))

/* Only capabilities that are ever actually on cix_default_deny above are
 * meaningful cap_add targets -- CAP_NET_ADMIN etc. are already kept by
 * default and would be a silent no-op here, not a real exception. */
struct cix_cap_name {
	const char *name;
	int value;
};

static const struct cix_cap_name cix_cap_names[] = {
	{ "CAP_SYS_MODULE", CIX_CAP_SYS_MODULE },
	{ "CAP_SYS_ADMIN", CIX_CAP_SYS_ADMIN },
	{ "CAP_SYS_PTRACE", CIX_CAP_SYS_PTRACE },
	{ "CAP_SYS_RAWIO", CIX_CAP_SYS_RAWIO },
	{ "CAP_SYS_BOOT", CIX_CAP_SYS_BOOT },
	{ "CAP_SYS_TIME", CIX_CAP_SYS_TIME },
	{ "CAP_SYS_RESOURCE", CIX_CAP_SYS_RESOURCE },
	{ "CAP_SYS_TTY_CONFIG", CIX_CAP_SYS_TTY_CONFIG },
	{ "CAP_LINUX_IMMUTABLE", CIX_CAP_LINUX_IMMUTABLE },
	{ "CAP_SYS_PACCT", CIX_CAP_SYS_PACCT },
	{ "CAP_MAC_OVERRIDE", CIX_CAP_MAC_OVERRIDE },
	{ "CAP_MAC_ADMIN", CIX_CAP_MAC_ADMIN },
	{ "CAP_SYSLOG", CIX_CAP_SYSLOG },
	{ "CAP_WAKE_ALARM", CIX_CAP_WAKE_ALARM },
	{ "CAP_BLOCK_SUSPEND", CIX_CAP_BLOCK_SUSPEND },
	{ "CAP_AUDIT_CONTROL", CIX_CAP_AUDIT_CONTROL },
	{ "CAP_AUDIT_READ", CIX_CAP_AUDIT_READ },
	{ "CAP_PERFMON", CIX_CAP_PERFMON },
	{ "CAP_BPF", CIX_CAP_BPF },
	{ "CAP_CHECKPOINT_RESTORE", CIX_CAP_CHECKPOINT_RESTORE },
};
#define CIX_CAP_NAMES_COUNT (int)(sizeof(cix_cap_names) / sizeof(cix_cap_names[0]))

/* -1 if name isn't a recognized, droppable capability name. */
static int cix_cap_by_name(const char *name)
{
	int i;

	for (i = 0; i < CIX_CAP_NAMES_COUNT; i++)
		if (strcmp(cix_cap_names[i].name, name) == 0)
			return cix_cap_names[i].value;
	return -1;
}

int container_cap_name_valid(const char *name)
{
	return cix_cap_by_name(name) >= 0;
}

int container_caps_drop(const char cap_add[][CONTAINER_CAP_NAME_MAX], int cap_add_count)
{
	int i, j;

	/* Reject an unrecognized cap_add entry outright, before dropping
	 * anything -- a typo should fail cleanly, not leave an operator
	 * believing a capability was restored when it silently wasn't. */
	for (j = 0; j < cap_add_count; j++) {
		if (cix_cap_by_name(cap_add[j]) < 0) {
			errno = EINVAL;
			return -1;
		}
	}

	for (i = 0; i < CIX_DEFAULT_DENY_COUNT; i++) {
		int cap = cix_default_deny[i];
		int keep = 0;

		for (j = 0; j < cap_add_count; j++) {
			if (cix_cap_by_name(cap_add[j]) == cap) {
				keep = 1;
				break;
			}
		}
		if (keep)
			continue;
		if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) != 0 && errno != EINVAL)
			return -1; /* EINVAL: kernel too old to know this bit --
			            * already absent, not a real failure. */
	}

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
		return -1;

	return 0;
}
