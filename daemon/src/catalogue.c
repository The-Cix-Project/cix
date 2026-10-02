#include "catalogue.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CATALOGUE_HEADER                                                                           \
	"# cix catalogue index v1 (ADR-0324)\n"                                                    \
	"# excludes " CATALOGUE_INDEX_PATH " and " CATALOGUE_SIG_PATH "\n"

/* The directories a sync takes files from -- and so the ones vouched for. */
static const char *const covered[] = { "recipes/package", "recipes/image", "recipes/deployment",
	                                "docs/keys" };

struct entry {
	char path[PATH_MAX];
	char sha[CATALOGUE_SHA256_HEX];
};

struct catalogue {
	struct entry *e;
	int n;
};

static int entry_cmp(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->path, ((const struct entry *)b)->path);
}

char *catalogue_build(const char *root, catalogue_hash_fn hash, size_t *out_len)
{
	struct entry *e = NULL;
	int n = 0, cap = 0, i;
	size_t k, size, off;
	char *out;

	for (k = 0; k < sizeof(covered) / sizeof(covered[0]); k++) {
		char dir[PATH_MAX];
		DIR *d;
		struct dirent *de;

		snprintf(dir, sizeof(dir), "%s/%s", root, covered[k]);
		d = opendir(dir);
		if (d == NULL)
			continue;
		while ((de = readdir(d)) != NULL) {
			char full[PATH_MAX];
			struct stat st;

			if (de->d_name[0] == '.')
				continue;
			if ((size_t)snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= sizeof(full) ||
			    lstat(full, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			if (n == cap) {
				int ncap = cap == 0 ? 256 : cap * 2;
				struct entry *ne = realloc(e, (size_t)ncap * sizeof(*ne));

				if (ne == NULL) {
					closedir(d);
					free(e);
					return NULL;
				}
				e = ne;
				cap = ncap;
			}
			snprintf(e[n].path, sizeof(e[n].path), "%s/%s", covered[k], de->d_name);
			if (hash(full, e[n].sha, sizeof(e[n].sha)) != 0) {
				closedir(d);
				free(e);
				return NULL;
			}
			n++;
		}
		closedir(d);
	}
	if (n > 0)
		qsort(e, (size_t)n, sizeof(*e), entry_cmp);

	size = sizeof(CATALOGUE_HEADER);
	for (i = 0; i < n; i++)
		size += strlen(e[i].sha) + 2 + strlen(e[i].path) + 1;
	out = malloc(size);
	if (out == NULL) {
		free(e);
		return NULL;
	}
	off = (size_t)snprintf(out, size, "%s", CATALOGUE_HEADER);
	for (i = 0; i < n; i++)
		off += (size_t)snprintf(out + off, size - off, "%s  %s\n", e[i].sha, e[i].path);
	free(e);
	*out_len = off;
	return out;
}

int catalogue_comment(const char *index_sha, long long t, char *out, size_t out_size)
{
	return (size_t)snprintf(out, out_size, "cix catalogue sha256=%s time=%lld", index_sha, t) <
	               out_size
	           ? 0
	           : -1;
}

int catalogue_comment_parse(const char *comment, const char *index_sha, long long *out_t)
{
	static const char head[] = "cix catalogue sha256=";
	const char *p, *end;
	char *num_end;
	long long t;

	if (comment == NULL || index_sha == NULL || strncmp(comment, head, sizeof(head) - 1) != 0)
		return -1;
	p = comment + sizeof(head) - 1;
	if (strncmp(p, index_sha, strlen(index_sha)) != 0)
		return -1;
	p += strlen(index_sha);
	if (strncmp(p, " time=", 6) != 0)
		return -1;
	p += 6;
	if (*p < '0' || *p > '9')
		return -1;
	t = strtoll(p, &num_end, 10);
	end = num_end;
	if (*end != '\0')
		return -1;
	*out_t = t;
	return 0;
}

struct catalogue *catalogue_parse(const char *text, size_t len)
{
	struct catalogue *c = calloc(1, sizeof(*c));
	const char *p = text, *stop = text + len;
	int cap = 0;

	if (c == NULL)
		return NULL;
	while (p < stop) {
		const char *nl = memchr(p, '\n', (size_t)(stop - p));
		size_t ll = nl != NULL ? (size_t)(nl - p) : (size_t)(stop - p);

		/* "<64 hex>  <path>"; comment and blank lines carry nothing. */
		if (ll > 66 && p[0] != '#' && p[64] == ' ' && p[65] == ' ' &&
		    ll - 66 < sizeof(c->e[0].path)) {
			if (c->n == cap) {
				int ncap = cap == 0 ? 256 : cap * 2;
				struct entry *ne = realloc(c->e, (size_t)ncap * sizeof(*ne));

				if (ne == NULL) {
					catalogue_free(c);
					return NULL;
				}
				c->e = ne;
				cap = ncap;
			}
			memcpy(c->e[c->n].sha, p, 64);
			c->e[c->n].sha[64] = '\0';
			memcpy(c->e[c->n].path, p + 66, ll - 66);
			c->e[c->n].path[ll - 66] = '\0';
			c->n++;
		}
		p = nl != NULL ? nl + 1 : stop;
	}
	if (c->n > 0)
		qsort(c->e, (size_t)c->n, sizeof(*c->e), entry_cmp);
	return c;
}

void catalogue_free(struct catalogue *c)
{
	if (c == NULL)
		return;
	free(c->e);
	free(c);
}

int catalogue_count(const struct catalogue *c)
{
	return c != NULL ? c->n : 0;
}

int catalogue_vouches(const struct catalogue *c, const char *path, const char *sha)
{
	struct entry key;
	const struct entry *hit;

	if (c == NULL || c->n == 0 || path == NULL || sha == NULL ||
	    strlen(path) >= sizeof(key.path))
		return 0;
	snprintf(key.path, sizeof(key.path), "%s", path);
	hit = bsearch(&key, c->e, (size_t)c->n, sizeof(*c->e), entry_cmp);
	return hit != NULL && strcmp(hit->sha, sha) == 0;
}
