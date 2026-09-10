#include "osrelease.h"

#include <stdio.h>
#include <string.h>

/*
 * What is here, and what is deliberately absent.
 *
 * The format is freedesktop's os-release: shell-compatible KEY=value
 * lines, so a script can source it and a C reader can parse it without
 * a library. Only NAME and ID really matter -- everything that reads
 * this file keys on ID, and a consumer that finds no ID at all falls
 * back to "linux", which is exactly the wrong answer for a platform
 * whose whole point is that it is not somebody else's distribution.
 *
 * ID is `cix`, lower-case, matching the wordmark the brand guidelines
 * name as the primary identifier. It is the one field here that is a
 * COMPATIBILITY SURFACE: once anything keys on it -- a package
 * manager, a logo table, a config-management rule -- changing it breaks
 * every one of them silently. That is why it is chosen deliberately
 * here rather than derived from a build variable.
 *
 * NO VERSION_ID. Cix is rolling-release by charter, so there is no
 * version to put in it, and the spec makes the field optional for
 * exactly this case. Writing a fabricated one -- the daemon build, say
 * -- would tell a reader that two hosts with different values are
 * running different releases, which is not true here. BUILD_ID carries
 * the build instead, which is what it is for.
 *
 * NO HOME_URL, DOCUMENTATION_URL or SUPPORT_URL. The only URLs that
 * exist today point at git.home.arpa, which nothing outside this LAN
 * can resolve. A URL that fails for every reader is worse than a
 * missing optional field: it looks like an answer.
 *
 * ANSI_COLOR is Copper (#D87945 = 216;121;69), the primary brand
 * accent, in the 24-bit form. Readers that cannot do true colour
 * ignore it; the spec allows any SGR sequence.
 */
int osrelease_render(char *out, size_t out_size, const char *build_version)
{
	int n;

	if (out == NULL || out_size == 0)
		return -1;

	if (build_version != NULL && build_version[0] != '\0') {
		n = snprintf(out, out_size,
		             "NAME=\"Cix\"\n"
		             "ID=cix\n"
		             "PRETTY_NAME=\"Cix\"\n"
		             "BUILD_ID=\"%s\"\n"
		             "ANSI_COLOR=\"0;38;2;216;121;69\"\n"
		             "LOGO=cix\n",
		             build_version);
	} else {
		n = snprintf(out, out_size,
		             "NAME=\"Cix\"\n"
		             "ID=cix\n"
		             "PRETTY_NAME=\"Cix\"\n"
		             "ANSI_COLOR=\"0;38;2;216;121;69\"\n"
		             "LOGO=cix\n");
	}

	if (n < 0 || (size_t)n >= out_size)
		return -1;
	return 0;
}
