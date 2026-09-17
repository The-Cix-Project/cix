/*
 * test_nsswitch -- the two nsswitch.conf variants agree about hosts,
 * and the baseline converges (#478, ADR-0296).
 *
 * This gates the thing that actually went wrong. The baseline said
 * "hosts: files" and the ldap_client replacement said nothing about
 * hosts at all, so a container given the explicit, pinned file could
 * not resolve names while a container given the incomplete one could
 * -- via the glibc compiled-in default ADR-0111 wrote the file to
 * avoid depending on. Two hand-written copies of one rule.
 *
 * Pure logic, no daemon and no filesystem, so it runs in a build
 * container and is in SELFTESTS. The convergence half matters most and
 * is the half nothing tested: the write used to be conditional on the
 * file being ABSENT, which is why every image already built kept the
 * old content and would have kept it forever.
 */
#include <stdio.h>
#include <string.h>

#include "nsswitch.h"

static int g_fail;

static void fail(const char *msg)
{
	fprintf(stderr, "FAIL: %s\n", msg);
	g_fail = 1;
}

/* The value of a line in an nsswitch database, or NULL if absent. */
static const char *db_line(const char *content, const char *db)
{
	const char *p = content;
	size_t dblen = strlen(db);

	while (p != NULL && *p != '\0') {
		if (strncmp(p, db, dblen) == 0)
			return p;
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	return NULL;
}

static void check_names_dns(const char *content, const char *what)
{
	const char *line = db_line(content, "hosts:");
	const char *nl;
	char buf[256];
	size_t n;

	if (line == NULL) {
		fail("a variant has no hosts: line at all -- that is exactly #478, where "
		     "glibc's own compiled-in default silently took over");
		fprintf(stderr, "      variant: %s\n", what);
		return;
	}
	nl = strchr(line, '\n');
	n = nl != NULL ? (size_t)(nl - line) : strlen(line);
	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	memcpy(buf, line, n);
	buf[n] = '\0';
	if (strstr(buf, "dns") == NULL) {
		fail("a variant's hosts: line does not name the dns backend, so a staged "
		     "/etc/resolv.conf would be inert");
		fprintf(stderr, "      variant: %s, line: %s\n", what, buf);
	}
	if (strstr(buf, "files") == NULL) {
		fail("a variant's hosts: line does not name files, so a staged /etc/hosts "
		     "would be ignored");
		fprintf(stderr, "      variant: %s, line: %s\n", what, buf);
	}
	printf("  %-12s %s\n", what, buf);
}

int main(void)
{
	const char *base = nsswitch_baseline_content();
	const char *ldap = nsswitch_ldap_client_content();
	const char *bh = db_line(base, "hosts:");
	const char *lh = db_line(ldap, "hosts:");

	check_names_dns(base, "baseline");
	check_names_dns(ldap, "ldap_client");

	/*
	 * The two must say the SAME thing about hosts. Whether a container
	 * joins the directory has nothing to do with how it resolves a
	 * hostname, and the moment those are two separate literals they
	 * drift -- which is the whole of #478.
	 */
	if (bh == NULL || lh == NULL || strcmp(bh, lh) != 0)
		fail("the baseline and ldap_client variants disagree about hosts -- they must "
		     "come from the one shared line, since ldap_client REPLACES the baseline");

	/* The account databases are where the two legitimately differ. */
	if (strstr(base, "passwd:         files\n") == NULL)
		fail("the baseline's passwd database should be files only (ADR-0111)");
	if (strstr(ldap, "passwd:         files ldap\n") == NULL)
		fail("the ldap_client variant's passwd database should be files ldap");

	/*
	 * Convergence. An absent file and a stale file must both be
	 * written; identical content must not be rewritten, so an install
	 * does not churn every image's rootfs.
	 */
	if (!nsswitch_needs_write(NULL, 0, base))
		fail("an absent nsswitch.conf must be written");
	if (!nsswitch_needs_write("hosts: files\n", strlen("hosts: files\n"), base))
		fail("a STALE nsswitch.conf must be rewritten -- writing only when the file is "
		     "absent is what left every already-built image on the old content (#478)");
	if (nsswitch_needs_write(base, strlen(base), base))
		fail("identical content must not be rewritten");
	if (!nsswitch_needs_write(base, strlen(base) - 1, base))
		fail("a truncated file must be rewritten");

	if (g_fail) {
		fprintf(stderr, "test_nsswitch: FAILED\n");
		return 1;
	}
	printf("test_nsswitch: both variants name the dns backend and agree\n");
	return 0;
}
