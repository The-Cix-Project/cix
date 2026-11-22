/*
 * test_ldaptoken -- a container's provisioned LDAP account is filled into
 * its files (ADR-0389, #733).
 *
 * The first consumer is nas's lighttpd.conf, whose bind DN and password
 * were the platform's own shared LDAP client credential until this:
 * auth.backend.ldap.bind-pw = "{{LDAP:PROVISIONED_SECRET}}".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldaptoken.h"

static int ok = 1;

static void expect(const char *in, const char *dn, const char *secret, int want_rc,
                   const char *want_out)
{
	char *out = NULL;
	int rc = ldaptoken_render(in, dn, secret, &out);

	if (rc != want_rc || (want_out == NULL) != (out == NULL) ||
	    (want_out != NULL && strcmp(out, want_out) != 0)) {
		fprintf(stderr, "FAIL: \"%s\": rc=%d out=\"%s\", want rc=%d out=\"%s\"\n",
		        in ? in : "(null)", rc, out ? out : "(null)", want_rc,
		        want_out ? want_out : "(null)");
		ok = 0;
	}
	free(out);
}

int main(void)
{
	const char *dn = "cn=nas,ou=svc-accounts,ou=users,dc=uk,dc=home,dc=arpa";
	const char *sec = "0123456789abcdef0123456789abcdef";

	/* No token: nothing to do, and no allocation. */
	expect("server.port = 80\n", dn, sec, 0, NULL);
	expect("", dn, sec, 0, NULL);
	/* The other {{LDAP:...}} family belongs to deployment apply. */
	expect("uri = \"{{LDAP:URI}}\"\n", dn, sec, 0, NULL);

	/* Both tokens, as nas's lighttpd.conf uses them. */
	expect("bind-dn = \"{{LDAP:PROVISIONED_DN}}\"\nbind-pw = \"{{LDAP:PROVISIONED_SECRET}}\"\n",
	       dn, sec, 1,
	       "bind-dn = \"cn=nas,ou=svc-accounts,ou=users,dc=uk,dc=home,dc=arpa\"\n"
	       "bind-pw = \"0123456789abcdef0123456789abcdef\"\n");
	/* Repeated, adjacent, at the very start and end. */
	expect("{{LDAP:PROVISIONED_SECRET}}{{LDAP:PROVISIONED_SECRET}}", dn, sec, 1,
	       "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
	/* Mixed with an apply token, which is left for apply. */
	expect("{{LDAP:URI}} {{LDAP:PROVISIONED_DN}}", dn, sec, 1,
	       "{{LDAP:URI}} cn=nas,ou=svc-accounts,ou=users,dc=uk,dc=home,dc=arpa");

	/* Refusals: never a partial render. */
	expect("x {{LDAP:PROVISIONED_PASSWORD}} y", dn, sec, -1, NULL);
	expect("x {{LDAP:PROVISIONED_SECRET y", dn, sec, -1, NULL);
	/* A token in a container with no provisioned account. */
	expect("{{LDAP:PROVISIONED_SECRET}}", NULL, NULL, -1, NULL);
	expect("{{LDAP:PROVISIONED_DN}}", NULL, sec, -1, NULL);

	/* A large file: the buffer grows. */
	{
		size_t n = 70000;
		char *big = malloc(n + 64);
		char *want = malloc(n + 64);

		if (big == NULL || want == NULL)
			return 1;
		memset(big, 'a', n);
		strcpy(big + n, "{{LDAP:PROVISIONED_DN}}");
		memset(want, 'a', n);
		strcpy(want + n, "cn=x");
		expect(big, "cn=x", sec, 1, want);
		free(big);
		free(want);
	}

	printf("LDAPTOKEN: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
