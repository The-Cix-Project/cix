#include "ldaptoken.h"

#include <stdlib.h>
#include <string.h>

#define LT_PREFIX "{{LDAP:PROVISIONED_"

/* Appends n bytes of s to the growing buffer. 0 on success. */
static int lt_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
	if (*len + n + 1 > *cap) {
		size_t want = (*cap == 0) ? 256 : *cap;
		char *nb;

		while (want < *len + n + 1)
			want *= 2;
		nb = realloc(*buf, want);
		if (nb == NULL)
			return -1;
		*buf = nb;
		*cap = want;
	}
	memcpy(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = '\0';
	return 0;
}

int ldaptoken_render(const char *in, const char *dn, const char *secret, char **out)
{
	const char *p = in;
	const char *tok;
	char *buf = NULL;
	size_t len = 0, cap = 0;

	*out = NULL;
	if (in == NULL || strstr(in, LT_PREFIX) == NULL)
		return 0;
	while ((tok = strstr(p, LT_PREFIX)) != NULL) {
		const char *name = tok + strlen(LT_PREFIX);
		const char *end = strstr(name, "}}");
		const char *val;

		if (lt_append(&buf, &len, &cap, p, (size_t)(tok - p)) != 0)
			goto fail;
		if (end == NULL)
			goto fail;
		if ((size_t)(end - name) == 2 && strncmp(name, "DN", 2) == 0)
			val = dn;
		else if ((size_t)(end - name) == 6 && strncmp(name, "SECRET", 6) == 0)
			val = secret;
		else
			goto fail;
		if (val == NULL)
			goto fail;
		if (lt_append(&buf, &len, &cap, val, strlen(val)) != 0)
			goto fail;
		p = end + 2;
	}
	if (lt_append(&buf, &len, &cap, p, strlen(p)) != 0)
		goto fail;
	*out = buf;
	return 1;
fail:
	free(buf);
	return -1;
}
