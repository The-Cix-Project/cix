/*
 * One source of truth for /etc/nsswitch.conf. See nsswitch.h for why
 * this is its own file rather than two literals at the call sites.
 */
#include "nsswitch.h"

#include <string.h>

const char *nsswitch_baseline_content(void)
{
	/*
	 * "files" for the account databases: this platform renders real
	 * /etc/passwd content into a container rather than having its NSS
	 * talk to a directory (ADR-0111), and that half is unchanged.
	 * Hosts are different in kind -- there is no file for Cix to
	 * render a container's view of DNS into, so the resolver it is
	 * given has to be consulted.
	 */
	return "passwd:         files\n"
	       "group:          files\n"
	       "shadow:         files\n" NSSWITCH_HOSTS_LINE;
}

const char *nsswitch_ldap_client_content(void)
{
	return "passwd:         files ldap\n"
	       "group:          files ldap\n"
	       "shadow:         files ldap\n" NSSWITCH_HOSTS_LINE;
}

int nsswitch_needs_write(const char *have, size_t have_len, const char *want)
{
	size_t want_len;

	if (have == NULL || want == NULL)
		return 1;
	want_len = strlen(want);
	if (have_len != want_len)
		return 1;
	return memcmp(have, want, want_len) != 0;
}
