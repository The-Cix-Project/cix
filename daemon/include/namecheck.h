#ifndef NAMECHECK_H
#define NAMECHECK_H

#include <stddef.h>

/*
 * Shared charset validator for every simple resource name in this
 * project (container, network, and package names -- NOT DNS record
 * names, which allow dots and follow RFC 1035 rules of their own via
 * dns_name_is_valid() in dns.h). Non-empty, [A-Za-z0-9_-] only,
 * strictly under max_len. A static inline header function rather than
 * a new .c/.o: pure, stateless, and small enough that three separate
 * modules each carrying their own byte-for-byte copy (differing only
 * in which length constant they checked against) was exactly the
 * duplication "No Parallel Implementations" forbids.
 */
static inline int simple_name_is_valid(const char *name, size_t max_len)
{
	size_t i;

	if (name == NULL || name[0] == '\0')
		return 0;
	for (i = 0; name[i] != '\0'; i++) {
		char c = name[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return 0;
	}
	if (i >= max_len)
		return 0;
	return 1;
}

#endif /* NAMECHECK_H */
