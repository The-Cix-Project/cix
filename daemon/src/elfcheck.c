/*
 * Issue #176: detect a compiler builtin that was emitted as an
 * undefined external instead of being implemented. See elfcheck.h for
 * why this class of bug needs a machine to catch it.
 *
 * The ELF structures are read field by field out of a byte buffer
 * rather than by casting onto <elf.h>'s own types. That is deliberate:
 * this project has already been bitten once by trusting a system
 * header's struct layout under TCC (`struct epoll_event`, ADR-0008 --
 * TCC ignores __attribute__((packed)) entirely, which produced
 * intermittent timing-dependent crashes rather than a compile error).
 * ELF64 structures happen to be naturally aligned and would very
 * likely be fine, but "very likely fine" is exactly the reasoning that
 * bug was hiding behind, and reading explicit little-endian fields
 * costs nothing here.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "elfcheck.h"

#define EI_NIDENT_LOCAL 16
#define ELFCLASS64_LOCAL 2
#define SHT_DYNSYM_LOCAL 11
#define SHT_DYNAMIC_LOCAL 6
#define DT_NULL_LOCAL 0
#define DT_NEEDED_LOCAL 1
#define SHN_UNDEF_LOCAL 0

/* A section header table larger than this is not something this
 * project produces; refusing is safer than trusting a bogus count from
 * a file that may not be a real ELF at all. */
#define ELFCHECK_MAX_SECTIONS 4096
/* Nothing legitimate needs a bigger string table than this to hold the
 * symbol names of one library. */
#define ELFCHECK_MAX_STRTAB (16u * 1024u * 1024u)

static unsigned long long rd(const unsigned char *p, int width)
{
	unsigned long long v = 0;
	int i;

	/* ELF little-endian only -- this platform is x86_64, and a
	 * big-endian file here would be a different problem entirely. */
	for (i = width - 1; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static int read_at(int fd, void *buf, size_t len, off_t off)
{
	ssize_t n = pread(fd, buf, len, off);

	return (n == (ssize_t)len) ? 0 : -1;
}

/*
 * Section-name lookup, needed only by the .comment reader below: the
 * undefined-builtin check finds its section by TYPE, which needs no
 * name table at all.
 */
static int section_name_matches(int fd, unsigned long long shstr_off, unsigned long long shstr_size,
                                 unsigned int name_off, const char *want)
{
	char buf[64];
	size_t want_len = strlen(want);

	if ((unsigned long long)name_off + want_len + 1 > shstr_size)
		return 0;
	if (want_len + 1 > sizeof(buf))
		return 0;
	if (read_at(fd, buf, want_len + 1, (off_t)(shstr_off + name_off)) != 0)
		return 0;
	buf[want_len] = '\0';
	return strcmp(buf, want) == 0;
}

int elfcheck_built_by_gcc(const char *path, char *out_version, size_t version_size)
{
	int fd;
	unsigned char ehdr[64];
	unsigned long long e_shoff, shstr_off = 0, shstr_size = 0;
	unsigned int e_shentsize, e_shnum, e_shstrndx, i;
	struct stat st;
	int found = 0;

	if (out_version != NULL && version_size > 0)
		out_version[0] = '\0';

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}
	if (read_at(fd, ehdr, sizeof(ehdr), 0) != 0) {
		close(fd);
		return 0;
	}
	if (memcmp(ehdr, "\177ELF", 4) != 0 || ehdr[4] != ELFCLASS64_LOCAL) {
		close(fd);
		return 0;
	}
	/*
	 * Refuse to answer for an executable.
	 *
	 * Cix's own glibc ships crt1.o/Scrt1.o carrying
	 * "GCC: (GNU) 16.2.0" -- correctly, because glibc is built with
	 * Cix's GCC. Every executable TCC links therefore inherits that
	 * marker regardless of what compiled the package's own code, so
	 * "GCC touched this executable" is true of every binary on the
	 * platform and distinguishes nothing.
	 *
	 * A shared library links crti.o/crtn.o (no marker) and not crt1.o,
	 * so a marker there really does come from compiled code.
	 *
	 * This was found the hard way: an earlier version of this check
	 * flagged seven current recipes as GCC-built on the strength of
	 * their executables, and a local rebuild of one of them from the
	 * identical recipe came out fully TCC-clean. The first version
	 * looked right only because DEBIAN's crt1.o happens to carry no
	 * marker -- a property of one dev sandbox, generalised to the
	 * platform. Hence ELFCHECK_GCC_INCONCLUSIVE rather than a quiet 0:
	 * "cannot tell" and "no" are different answers and collapsing them
	 * is how the wrong conclusion got drawn in the first place.
	 */
	{
		unsigned int e_type = (unsigned int)rd(ehdr + 0x10, 2);
		unsigned long long e_phoff = rd(ehdr + 0x20, 8);
		unsigned int e_phentsize = (unsigned int)rd(ehdr + 0x36, 2);
		unsigned int e_phnum = (unsigned int)rd(ehdr + 0x38, 2);
		unsigned int p;
		int is_executable = (e_type == 2); /* ET_EXEC */

		/* ET_DYN is both a library and a PIE executable; only the
		 * latter has a PT_INTERP. */
		if (!is_executable && e_type == 3 && e_phoff != 0 && e_phentsize >= 56 &&
		    e_phnum > 0 && e_phnum < 256) {
			for (p = 0; p < e_phnum; p++) {
				unsigned char ph[56];

				if (read_at(fd, ph, sizeof(ph),
				            (off_t)(e_phoff + (unsigned long long)p * e_phentsize)) != 0)
					break;
				if (rd(ph + 0x00, 4) == 3) { /* PT_INTERP */
					is_executable = 1;
					break;
				}
			}
		}
		if (is_executable) {
			close(fd);
			return ELFCHECK_GCC_INCONCLUSIVE;
		}
	}

	e_shoff = rd(ehdr + 0x28, 8);
	e_shentsize = (unsigned int)rd(ehdr + 0x3a, 2);
	e_shnum = (unsigned int)rd(ehdr + 0x3c, 2);
	e_shstrndx = (unsigned int)rd(ehdr + 0x3e, 2);
	if (e_shoff == 0 || e_shentsize < 64 || e_shnum == 0 || e_shnum > ELFCHECK_MAX_SECTIONS ||
	    e_shstrndx >= e_shnum) {
		close(fd);
		return 0;
	}
	{
		unsigned char sh[64];

		if (read_at(fd, sh, sizeof(sh),
		            (off_t)(e_shoff + (unsigned long long)e_shstrndx * e_shentsize)) != 0) {
			close(fd);
			return 0;
		}
		shstr_off = rd(sh + 0x18, 8);
		shstr_size = rd(sh + 0x20, 8);
	}

	for (i = 0; i < e_shnum && !found; i++) {
		unsigned char sh[64];
		unsigned long long off, size;
		char *body;
		unsigned int name_off;

		if (read_at(fd, sh, sizeof(sh), (off_t)(e_shoff + (unsigned long long)i * e_shentsize)) != 0)
			break;
		name_off = (unsigned int)rd(sh + 0x00, 4);
		if (!section_name_matches(fd, shstr_off, shstr_size, name_off, ".comment"))
			continue;
		off = rd(sh + 0x18, 8);
		size = rd(sh + 0x20, 8);
		if (size == 0 || size > 64 * 1024)
			continue;
		body = malloc((size_t)size + 1);
		if (body == NULL)
			continue;
		if (read_at(fd, body, (size_t)size, (off_t)off) == 0) {
			size_t j;

			body[size] = '\0';
			/* .comment is a run of NUL-separated strings; a producer
			 * that is not first must still be found. */
			for (j = 0; j < (size_t)size; j++) {
				if (strncmp(body + j, "GCC: ", 5) == 0) {
					if (out_version != NULL && version_size > 0)
						snprintf(out_version, version_size, "%s", body + j);
					found = 1;
					break;
				}
				while (j < (size_t)size && body[j] != '\0')
					j++;
			}
		}
		free(body);
	}
	close(fd);
	return found;
}

/*
 * The libraries a binary actually declares, read from its own
 * .dynamic section (#224).
 *
 * Exists because test_dns carried a hardcoded twenty-entry list of
 * shared libraries to stage beside dnsmasq -- the dependency closure of
 * DEBIAN's dnsmasq, from a comment that says so ("a real, unmodified
 * Debian package binary"). Cix builds its own dnsmasq now, and it needs
 * exactly two: libc.so.6 and the loader. So the test could not run on a
 * Cix host at all, failing on a libdbus that this platform's dnsmasq
 * has never linked against.
 *
 * A list of libraries kept beside a binary is the same hand-maintained
 * pair that drifts everywhere else in this project. The binary already
 * states its own answer; this reads it.
 *
 * Names only (DT_NEEDED holds a soname, not a path) -- resolving them
 * to files is the caller's job, since where a library lives is a
 * property of the system, not of the binary.
 */
int elfcheck_needed_libs(const char *path, char out[][ELFCHECK_SONAME_MAX], int max)
{
	int fd;
	unsigned char ehdr[64];
	unsigned long long e_shoff;
	unsigned int e_shentsize, e_shnum, i;
	struct stat st;
	int count = 0;

	if (out == NULL || max <= 0)
		return -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}
	if (read_at(fd, ehdr, sizeof(ehdr), 0) != 0 || memcmp(ehdr, "\177ELF", 4) != 0 ||
	    ehdr[4] != ELFCLASS64_LOCAL) {
		close(fd);
		return -1;
	}

	e_shoff = rd(ehdr + 0x28, 8);
	e_shentsize = (unsigned int)rd(ehdr + 0x3a, 2);
	e_shnum = (unsigned int)rd(ehdr + 0x3c, 2);
	if (e_shoff == 0 || e_shentsize < 64 || e_shnum == 0 || e_shnum > ELFCHECK_MAX_SECTIONS) {
		close(fd);
		return -1;
	}

	for (i = 0; i < e_shnum; i++) {
		unsigned char sh[64], strsh[64];
		unsigned int sh_type, sh_link;
		unsigned long long sh_off, sh_size, str_off, str_size, off;
		char *strtab;

		if (read_at(fd, sh, sizeof(sh), (off_t)(e_shoff + (unsigned long long)i * e_shentsize)) != 0)
			break;
		sh_type = (unsigned int)rd(sh + 0x04, 4);
		if (sh_type != SHT_DYNAMIC_LOCAL)
			continue;

		sh_off = rd(sh + 0x18, 8);
		sh_size = rd(sh + 0x20, 8);
		sh_link = (unsigned int)rd(sh + 0x28, 4);
		if (sh_size == 0 || sh_link >= e_shnum)
			continue;

		/* .dynamic's linked section is the string table its
		 * DT_NEEDED offsets index into. */
		if (read_at(fd, strsh, sizeof(strsh),
		            (off_t)(e_shoff + (unsigned long long)sh_link * e_shentsize)) != 0)
			continue;
		str_off = rd(strsh + 0x18, 8);
		str_size = rd(strsh + 0x20, 8);
		if (str_size == 0 || str_size > ELFCHECK_MAX_STRTAB)
			continue;
		strtab = malloc((size_t)str_size + 1);
		if (strtab == NULL)
			continue;
		if (read_at(fd, strtab, (size_t)str_size, (off_t)str_off) != 0) {
			free(strtab);
			continue;
		}
		strtab[str_size] = '\0';

		/* Each entry is a 16-byte (tag, value) pair; DT_NULL ends it. */
		for (off = 0; off + 16 <= sh_size && count < max; off += 16) {
			unsigned char ent[16];
			unsigned long long tag, val;

			if (read_at(fd, ent, sizeof(ent), (off_t)(sh_off + off)) != 0)
				break;
			tag = rd(ent, 8);
			val = rd(ent + 8, 8);
			if (tag == DT_NULL_LOCAL)
				break;
			if (tag != DT_NEEDED_LOCAL || val >= str_size)
				continue;
			snprintf(out[count], ELFCHECK_SONAME_MAX, "%s", strtab + val);
			count++;
		}
		free(strtab);
		break;
	}
	close(fd);
	return count;
}

int elfcheck_undefined_builtin(const char *path, char *out_sym, size_t sym_size)
{
	int fd;
	unsigned char ehdr[64];
	unsigned long long e_shoff;
	unsigned int e_shentsize, e_shnum, i;
	struct stat st;
	int found = 0;

	if (out_sym != NULL && sym_size > 0)
		out_sym[0] = '\0';

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}
	if (read_at(fd, ehdr, sizeof(ehdr), 0) != 0) {
		/* Too small to be an ELF64 file -- not an error, just not
		 * something this check applies to. */
		close(fd);
		return 0;
	}
	if (memcmp(ehdr, "\177ELF", 4) != 0 || ehdr[4] != ELFCLASS64_LOCAL) {
		close(fd);
		return 0;
	}

	e_shoff = rd(ehdr + 0x28, 8);
	e_shentsize = (unsigned int)rd(ehdr + 0x3a, 2);
	e_shnum = (unsigned int)rd(ehdr + 0x3c, 2);
	if (e_shoff == 0 || e_shentsize < 64 || e_shnum == 0 || e_shnum > ELFCHECK_MAX_SECTIONS) {
		close(fd);
		return 0;
	}

	for (i = 0; i < e_shnum && !found; i++) {
		unsigned char sh[64];
		unsigned int sh_type, sh_link, sh_entsize;
		unsigned long long sh_off, sh_size;
		unsigned char strsh[64];
		unsigned long long str_off, str_size;
		char *strtab;
		unsigned long long nsyms, s;

		if (read_at(fd, sh, sizeof(sh), (off_t)(e_shoff + (unsigned long long)i * e_shentsize)) != 0)
			break;
		sh_type = (unsigned int)rd(sh + 0x04, 4);
		if (sh_type != SHT_DYNSYM_LOCAL)
			continue;

		sh_off = rd(sh + 0x18, 8);
		sh_size = rd(sh + 0x20, 8);
		sh_link = (unsigned int)rd(sh + 0x28, 4);
		sh_entsize = (unsigned int)rd(sh + 0x38, 8);
		if (sh_entsize < 24 || sh_size == 0 || sh_link >= e_shnum)
			continue;

		/* The linked section is this symbol table's own string table. */
		if (read_at(fd, strsh, sizeof(strsh),
		            (off_t)(e_shoff + (unsigned long long)sh_link * e_shentsize)) != 0)
			continue;
		str_off = rd(strsh + 0x18, 8);
		str_size = rd(strsh + 0x20, 8);
		if (str_size == 0 || str_size > ELFCHECK_MAX_STRTAB)
			continue;

		strtab = malloc((size_t)str_size + 1);
		if (strtab == NULL)
			continue;
		if (read_at(fd, strtab, (size_t)str_size, (off_t)str_off) != 0) {
			free(strtab);
			continue;
		}
		strtab[str_size] = '\0'; /* so a malformed unterminated entry cannot run off the end */

		nsyms = sh_size / sh_entsize;
		for (s = 0; s < nsyms; s++) {
			unsigned char sym[24];
			unsigned int st_name;
			unsigned int st_shndx;
			const char *name;

			if (read_at(fd, sym, sizeof(sym), (off_t)(sh_off + s * sh_entsize)) != 0)
				break;
			st_name = (unsigned int)rd(sym + 0x00, 4);
			st_shndx = (unsigned int)rd(sym + 0x06, 2);
			if (st_shndx != SHN_UNDEF_LOCAL)
				continue; /* defined here -- not our case */
			if (st_name == 0 || (unsigned long long)st_name >= str_size)
				continue;
			name = strtab + st_name;
			if (strncmp(name, "__builtin_", 10) != 0)
				continue;

			if (out_sym != NULL && sym_size > 0)
				snprintf(out_sym, sym_size, "%s", name);
			found = 1;
			break;
		}
		free(strtab);
	}

	close(fd);
	return found;
}
