#include <time.h>
#include "libdirs.h"
#include "test_image_fixture.h"

#include <elf.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

int test_mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	size_t len;
	char *p;

	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				perror(tmp);
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
		perror(tmp);
		return -1;
	}
	return 0;
}

int test_image_fixture_copy_file(const char *src_path, const char *dst_path)
{
	int src, dst;
	char buf[4096];
	ssize_t n;

	src = open(src_path, O_RDONLY);
	if (src < 0) {
		perror(src_path);
		return -1;
	}
	dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (dst < 0) {
		perror(dst_path);
		close(src);
		return -1;
	}
	while ((n = read(src, buf, sizeof(buf))) > 0) {
		if (write(dst, buf, (size_t)n) != n) {
			perror("write");
			close(src);
			close(dst);
			return -1;
		}
	}
	if (n < 0)
		perror(src_path);
	close(src);
	close(dst);
	return n < 0 ? -1 : 0;
}

int test_image_fixture_build(const char *image_root, const char *child_binary_path,
                              const char *child_basename)
{
	char path[PATH_MAX];

	if (test_mkdir_p(image_root) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/bin", image_root);
	if (test_mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/bin/%s", image_root, child_basename);
	if (test_image_fixture_copy_file(child_binary_path, path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/lib64", image_root);
	if (test_mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/lib64/ld-linux-x86-64.so.2", image_root);
	if (test_image_fixture_copy_file("/" CIX_LIB_DIR_RUNTIME "/ld-linux-x86-64.so.2", path) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/" CIX_LIB_DIR_RUNTIME, image_root);
	if (test_mkdir_p(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "%s/" CIX_LIB_DIR_RUNTIME "/libc.so.6", image_root);
	if (test_image_fixture_copy_file("/" CIX_LIB_DIR_RUNTIME "/libc.so.6", path) != 0)
		return -1;
	/*
	 * Second copy of ld-linux itself, same file as lib64/ above but at
	 * the path glibc >= 2.34's own libc.so.6 needs it reachable from --
	 * libc.so.6 carries a DT_NEEDED entry on ld-linux-x86-64.so.2
	 * itself, resolved via the ordinary runtime library search path
	 * (which does not include /lib64), not the one-time PT_INTERP
	 * lookup lib64/ld-linux-x86-64.so.2 above already satisfies.
	 *
	 * All three reads in this function (this one, libc.so.6 above, and
	 * lib64/ld-linux above) source from "/lib/x86_64-linux-gnu/", never
	 * "/usr/lib/x86_64-linux-gnu/" -- the real, final root cause of the
	 * "ld-linux-x86-64.so.2: No such file or directory" failure this
	 * function has produced on every self-hosted mkbootroot run all
	 * along, confirmed via strace against a genuinely non-merged-usr
	 * chroot: this function's own SOURCE reads used the "/usr/lib/..."
	 * form, which only resolves on a merged-usr dev sandbox (where
	 * /lib is itself a symlink to usr/lib) -- this project's own
	 * produced roots are deliberately NOT merged-usr (CLAUDE.md), so
	 * that path is simply absent when mkbootroot runs on one of its own
	 * prior builds (the self-hosted case, ADR-0057) -- the ONE case
	 * that matters, since a plain local dev-sandbox invocation never
	 * exercises this failure at all. "/lib/x86_64-linux-gnu/" resolves
	 * correctly in both environments: via the symlink here, and as the
	 * real, actual location there (matching exactly where this same
	 * function's own destination writes already place these files for
	 * the next generation). A prior attempt at this fix (superseded
	 * ADR-0083) got the destination right but left these three source
	 * reads on the wrong path -- see ADR-0085.
	 */
	snprintf(path, sizeof(path), "%s/" CIX_LIB_DIR_RUNTIME "/ld-linux-x86-64.so.2", image_root);
	if (test_image_fixture_copy_file("/" CIX_LIB_DIR_RUNTIME "/ld-linux-x86-64.so.2", path) != 0)
		return -1;

	return 0;
}

int test_image_fixture_copy_dir_files(const char *src_dir, const char *dst_dir)
{
	DIR *dir = opendir(src_dir);
	struct dirent *entry;
	char src_path[PATH_MAX], dst_path[PATH_MAX];
	struct stat st;

	if (dir == NULL) {
		perror(src_dir);
		return -1;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name) >=
		            (int)sizeof(src_path) ||
		    snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name) >=
		            (int)sizeof(dst_path)) {
			fprintf(stderr, "path too long under %s\n", src_dir);
			closedir(dir);
			return -1;
		}
		if (stat(src_path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (test_image_fixture_copy_file(src_path, dst_path) != 0) {
			closedir(dir);
			return -1;
		}
	}
	closedir(dir);
	return 0;
}

int test_image_fixture_add_lib(const char *image_root, const char *host_lib_abs_path)
{
	char dst_path[PATH_MAX];
	char dst_dir[PATH_MAX];
	char resolved[PATH_MAX];
	char *slash;

	/*
	 * Find the library by NAME if it is not at the path asked for
	 * (#224).
	 *
	 * Callers name an absolute path because that is where the library
	 * sits on a Debian-derived build host: /lib/x86_64-linux-gnu/.
	 * That is an inherited layout rather than a decision (#184), and it
	 * is not the layout of a Cix build environment, where a package
	 * like ncurses installs to usr/lib/. So staging libtinfo.so.6 by
	 * its multiarch path failed with "No such file or directory" on
	 * exactly the host that was supposed to be able to do this -- and
	 * took six boot tests with it.
	 *
	 * The destination is unchanged: whatever the caller asked for is
	 * still where it lands inside the image. Only the SOURCE is
	 * searched, and only when the given path does not exist, so a host
	 * that does have the library where the caller said is completely
	 * unaffected.
	 */
	snprintf(resolved, sizeof(resolved), "%s", host_lib_abs_path);
	if (access(resolved, R_OK) != 0) {
		static const char *const dirs[] = CIX_LIB_DIRS_SEARCH;
		const char *base = strrchr(host_lib_abs_path, '/');
		size_t i;

		base = (base != NULL) ? base + 1 : host_lib_abs_path;
		for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
			snprintf(resolved, sizeof(resolved), "/%s/%s", dirs[i], base);
			if (access(resolved, R_OK) == 0)
				break;
			resolved[0] = '\0';
		}
	}

	if (resolved[0] == '\0')
		snprintf(resolved, sizeof(resolved), "%s", host_lib_abs_path); /* let the copy report it */

	/* Destination is always what the caller asked for -- the loader
	 * inside the image looks where the caller said, not where this
	 * host happens to keep its copy. */
	if (snprintf(dst_path, sizeof(dst_path), "%s%s", image_root, host_lib_abs_path) >=
	    (int)sizeof(dst_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	snprintf(dst_dir, sizeof(dst_dir), "%s", dst_path);
	slash = strrchr(dst_dir, '/');
	if (slash != NULL)
		*slash = '\0';

	if (test_mkdir_p(dst_dir) != 0)
		return -1;
	return test_image_fixture_copy_file(resolved, dst_path);
}

int test_pkg_config_seed_cleared(const char *pkg_state_dir)
{
	/*
	 * ADR-0315: a host with no saved repo or artifact config starts
	 * pointed at the public catalogue and cache. A test daemon must
	 * not reach the internet, and in a build container it cannot (no
	 * egress), so every test daemon starts as an operator who has
	 * CLEARED both -- written as the same files a clearing PUT
	 * persists, not through a test-only switch.
	 */
	static const char *const cleared[][2] = {
		/* ADR-0324: a saved empty source list -- cleared, and so never
		 * replaced by the public default. */
		{"sources.json", "{\"sources\":[],\"choices\":[]}\n"},
		/* ADR-0324 step B: a saved empty repository list, cleared the same way. */
		{"repositories.json", "{\"repositories\":[]}\n"},
	};
	char cfg[PATH_MAX];
	size_t i;
	FILE *f;

	if (test_mkdir_p(pkg_state_dir) != 0)
		return -1;
	for (i = 0; i < sizeof(cleared) / sizeof(cleared[0]); i++) {
		if (snprintf(cfg, sizeof(cfg), "%s/%s", pkg_state_dir, cleared[i][0]) >= (int)sizeof(cfg))
			return -1;
		f = fopen(cfg, "w");
		if (f == NULL)
			return -1;
		fputs(cleared[i][1], f);
		if (fclose(f) != 0)
			return -1;
	}
	return 0;
}

int test_data_dir_create(char *out_path, size_t out_size)
{
	/*
	 * /run first, and it is not a preference (#224).
	 *
	 * A container's rootfs is an overlay mount, so when the suite runs
	 * inside a build container -- which is how it runs on a Cix host at
	 * all -- a data directory under /tmp sits ON that overlay. Every
	 * container a test daemon then creates asks the kernel to stack
	 * overlayfs on overlayfs, and the kernel refuses, in as many words:
	 *
	 *   overlay: filesystem on /tmp/cix_test_data_XXXXXX/containers/
	 *            c1/upper not supported as upperdir
	 *
	 * /run is a fresh tmpfs in every container (src/mountns.c mounts it
	 * precisely so it is not overlay-backed storage), and tmpfs is a
	 * supported upperdir. On a host with no such constraint /run is an
	 * ordinary tmpfs too, so this is not a container-only special case
	 * -- it is simply a better place for a scratch directory that has
	 * filesystems built on top of it.
	 *
	 * /tmp remains the fallback for any environment where /run is not
	 * writable, which is where every one of these tests ran before.
	 */
	char tmpl[] = "/run/cix_test_data_XXXXXX";
	char fallback[] = "/tmp/cix_test_data_XXXXXX";

	if (mkdtemp(tmpl) == NULL) {
		if (mkdtemp(fallback) == NULL) {
			perror("mkdtemp");
			return -1;
		}
		snprintf(tmpl, sizeof(tmpl), "%s", fallback);
	}
	if (snprintf(out_path, out_size, "%s", tmpl) >= (int)out_size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	/*
	 * ADR-0207 phase 3: the platform default is userns ON, but this dev
	 * sandbox's own LSM forbids the uid_map write outright (documented
	 * in CLAUDE.md -- even a "0 0 1" self-map is EPERM here), so every
	 * container a test daemon created would die in its handshake.
	 * Seeded through the ordinary daemon-config channel -- the same
	 * knob an operator has -- not a test-only code path. A test that
	 * wants the secure default exercises it on the .95 VM, the one
	 * environment whose kernel actually permits it.
	 */
	{
		char cfg_dir[PATH_MAX];
		char cfg[PATH_MAX];
		FILE *f;

		snprintf(cfg_dir, sizeof(cfg_dir), "%s/state", tmpl);
		if (mkdir(cfg_dir, 0755) != 0 && errno != EEXIST)
			return -1;
		snprintf(cfg, sizeof(cfg), "%s/daemon_config.json", cfg_dir);
		f = fopen(cfg, "w");
		if (f == NULL)
			return -1;
		fputs("{\"userns_default\": false}\n", f);
		fclose(f);
		/*
		 * ADR-0316: a host with no saved schedules gets a recipe sync
		 * that fires at startup. A test daemon starts as a host whose
		 * schedules were saved empty, so nothing runs on a clock
		 * unless the test asks for it.
		 */
		snprintf(cfg, sizeof(cfg), "%s/schedules.json", cfg_dir);
		f = fopen(cfg, "w");
		if (f == NULL)
			return -1;
		fputs("{\"schedules\":[]}\n", f);
		fclose(f);
		/*
		 * ADR-0317 section 7 (#542): a host with no record of having
		 * provisioned them is given cix-admins, cix-operators and
		 * cix-readers at startup. A test daemon starts as a host that
		 * already has, so its directory and permission mapping hold
		 * only what the test puts there. test_hostauth removes this to
		 * test the provisioning itself.
		 */
		snprintf(cfg, sizeof(cfg), "%s/hostauth_config.json", cfg_dir);
		f = fopen(cfg, "w");
		if (f == NULL)
			return -1;
		fputs("{\"standard_groups_provisioned\":true}\n", f);
		fclose(f);
	}
	{
		char pkg_dir[PATH_MAX];

		snprintf(pkg_dir, sizeof(pkg_dir), "%s/rebuildable/pkg", tmpl);
		if (test_pkg_config_seed_cleared(pkg_dir) != 0)
			return -1;
	}
	return 0;
}

void test_data_dir_cleanup(const char *path)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: cleanup of %s failed\n", path);
}

int test_image_fixture_write_manifest(const char *image_dir, const char *version)
{
	char manifest_path[PATH_MAX];
	char content[512];
	int n;
	FILE *f;

	if (test_mkdir_p(image_dir) != 0) {
		perror("mkdir_p (image_dir)");
		return -1;
	}
	snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", image_dir);
	n = snprintf(content, sizeof(content),
	             "{\"packages\":[],\"current_version\":\"%s\","
	             "\"versions\":[{\"version\":\"%s\",\"created_at\":0}]}",
	             version, version);
	f = fopen(manifest_path, "w");
	if (f == NULL) {
		perror("fopen (manifest.json)");
		return -1;
	}
	if (fwrite(content, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

int test_image_fixture_read_current_version(const char *image_dir, char *out_version,
                                             size_t out_size)
{
	char manifest_path[PATH_MAX];
	char buf[4096];
	FILE *f;
	size_t n;
	const char *key = "\"current_version\":\"";
	const char *p, *end;
	size_t len;

	snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", image_dir);
	f = fopen(manifest_path, "r");
	if (f == NULL)
		return -1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	p = strstr(buf, key);
	if (p == NULL)
		return -1;
	p += strlen(key);
	end = strchr(p, '"');
	if (end == NULL)
		return -1;
	len = (size_t)(end - p);
	if (len >= out_size)
		return -1;
	memcpy(out_version, p, len);
	out_version[len] = '\0';
	return 0;
}

/*
 * #505: see test_image_fixture.h. The corpus moved to its own
 * repository, so a test that reads a real recipe has to be told where
 * it is instead of assuming a directory in this tree.
 */
const char *test_recipes_root(void)
{
	const char *env = getenv("CIX_RECIPES_DIR");

	if (env != NULL && env[0] != '\0')
		return env;
	return "../cix-recipes/recipes";
}

int test_recipe_path(const char *kind, const char *name, const char *version,
                     const char *ext, char *out, size_t out_size)
{
	int n = snprintf(out, out_size, "%s/%s/%s@%s.%s", test_recipes_root(), kind, name,
	                 version, ext);

	if (n < 0 || (size_t)n >= out_size)
		return -1;
	return 0;
}

/* ---- real shared-library closure derivation (see the header) ---- */

#define CLOSURE_MAX_SEEN 128
#define CLOSURE_NAME_MAX 96

struct closure_seen {
	char name[CLOSURE_MAX_SEEN][CLOSURE_NAME_MAX];
	int n;
};

/* The runtime test_image_fixture_build() already stages. Re-staging
 * either from a different tree would replace a working loader. */
static int closure_is_runtime(const char *soname)
{
	return strcmp(soname, "libc.so.6") == 0 || strcmp(soname, "ld-linux-x86-64.so.2") == 0;
}

static int closure_seen_add(struct closure_seen *seen, const char *name)
{
	int i;

	for (i = 0; i < seen->n; i++) {
		if (strcmp(seen->name[i], name) == 0)
			return 1; /* already handled */
	}
	if (seen->n >= CLOSURE_MAX_SEEN) {
		fprintf(stderr, "shared-library closure exceeded %d entries at %s\n",
		        CLOSURE_MAX_SEEN, name);
		return -1;
	}
	snprintf(seen->name[seen->n], CLOSURE_NAME_MAX, "%s", name);
	seen->n++;
	return 0;
}

/*
 * Reads an ELF64 file's DT_NEEDED entries. Returns 0 with *out_n set
 * (0 for a static binary, or one with no PT_DYNAMIC), or -1.
 *
 * The whole file is read into memory rather than seeked around: these
 * are binaries and libraries of a few hundred KB, the parse touches the
 * program headers, one PT_DYNAMIC segment and one string table, and a
 * single read is both simpler and harder to get wrong than a series of
 * pread()s whose offsets all come from the file's own untrusted fields.
 */
static int elf_needed(const char *path, char out[][CLOSURE_NAME_MAX], int max, int *out_n)
{
	unsigned char *buf = NULL;
	off_t size;
	int fd, i, rc = -1;
	ssize_t got, off = 0;
	Elf64_Ehdr *eh;
	Elf64_Phdr *ph;
	Elf64_Dyn *dyn = NULL;
	size_t dyn_count = 0;
	Elf64_Addr strtab_vaddr = 0;
	const char *strtab = NULL;

	*out_n = 0;
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		return -1;
	}
	size = lseek(fd, 0, SEEK_END);
	if (size <= 0 || lseek(fd, 0, SEEK_SET) != 0) {
		fprintf(stderr, "%s: cannot determine size\n", path);
		close(fd);
		return -1;
	}
	buf = malloc((size_t)size);
	if (buf == NULL) {
		perror("malloc");
		close(fd);
		return -1;
	}
	while (off < (ssize_t)size) {
		got = read(fd, buf + off, (size_t)size - (size_t)off);
		if (got <= 0) {
			perror(path);
			goto out;
		}
		off += got;
	}
	close(fd);
	fd = -1;

	if ((size_t)size < sizeof(Elf64_Ehdr) || memcmp(buf, ELFMAG, SELFMAG) != 0 ||
	    buf[EI_CLASS] != ELFCLASS64) {
		/* Not an ELF64 object at all -- a script, or something else
		 * entirely. It has no closure to stage, which is not an error. */
		rc = 0;
		goto out;
	}
	eh = (Elf64_Ehdr *)buf;
	if (eh->e_phoff == 0 || eh->e_phentsize != sizeof(Elf64_Phdr) ||
	    eh->e_phoff + (off_t)eh->e_phnum * eh->e_phentsize > size) {
		fprintf(stderr, "%s: malformed program headers\n", path);
		goto out;
	}

	/* PT_DYNAMIC holds the entries; its DT_STRTAB is a virtual address,
	 * so a PT_LOAD segment is needed to map it back to a file offset. */
	for (i = 0; i < eh->e_phnum; i++) {
		ph = (Elf64_Phdr *)(buf + eh->e_phoff + (size_t)i * sizeof(Elf64_Phdr));
		if (ph->p_type != PT_DYNAMIC)
			continue;
		if (ph->p_offset + ph->p_filesz > (Elf64_Xword)size) {
			fprintf(stderr, "%s: PT_DYNAMIC out of range\n", path);
			goto out;
		}
		dyn = (Elf64_Dyn *)(buf + ph->p_offset);
		dyn_count = ph->p_filesz / sizeof(Elf64_Dyn);
		break;
	}
	if (dyn == NULL) {
		rc = 0; /* static, or no dynamic section */
		goto out;
	}
	for (i = 0; (size_t)i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
		if (dyn[i].d_tag == DT_STRTAB)
			strtab_vaddr = dyn[i].d_un.d_ptr;
	}
	if (strtab_vaddr == 0) {
		fprintf(stderr, "%s: dynamic section has no DT_STRTAB\n", path);
		goto out;
	}
	for (i = 0; i < eh->e_phnum; i++) {
		ph = (Elf64_Phdr *)(buf + eh->e_phoff + (size_t)i * sizeof(Elf64_Phdr));
		if (ph->p_type != PT_LOAD)
			continue;
		if (strtab_vaddr < ph->p_vaddr || strtab_vaddr >= ph->p_vaddr + ph->p_filesz)
			continue;
		strtab = (const char *)(buf + ph->p_offset + (strtab_vaddr - ph->p_vaddr));
		break;
	}
	if (strtab == NULL) {
		fprintf(stderr, "%s: DT_STRTAB is in no PT_LOAD segment\n", path);
		goto out;
	}
	for (i = 0; (size_t)i < dyn_count && dyn[i].d_tag != DT_NULL; i++) {
		const char *name;

		if (dyn[i].d_tag != DT_NEEDED)
			continue;
		name = strtab + dyn[i].d_un.d_val;
		if (name < (const char *)buf || name >= (const char *)buf + size) {
			fprintf(stderr, "%s: DT_NEEDED name out of range\n", path);
			goto out;
		}
		if (*out_n >= max) {
			fprintf(stderr, "%s: more than %d DT_NEEDED entries\n", path, max);
			goto out;
		}
		snprintf(out[*out_n], CLOSURE_NAME_MAX, "%s", name);
		(*out_n)++;
	}
	rc = 0;
out:
	if (fd >= 0)
		close(fd);
	free(buf);
	return rc;
}

static int stage_closure_rec(const char *image_root, const char *binary_path,
                             const char *const *search_dirs, struct closure_seen *seen)
{
	char needed[32][CLOSURE_NAME_MAX];
	int n = 0, i, d;

	if (elf_needed(binary_path, needed, 32, &n) != 0)
		return -1;

	for (i = 0; i < n; i++) {
		char resolved[PATH_MAX];
		char dst[PATH_MAX];
		int added, found = 0;

		if (closure_is_runtime(needed[i]))
			continue;
		added = closure_seen_add(seen, needed[i]);
		if (added < 0)
			return -1;
		if (added == 1)
			continue;

		for (d = 0; search_dirs[d] != NULL; d++) {
			struct stat st;

			snprintf(resolved, sizeof(resolved), "%s/%s", search_dirs[d], needed[i]);
			if (stat(resolved, &st) == 0 && S_ISREG(st.st_mode)) {
				found = 1;
				break;
			}
		}
		if (!found) {
			fprintf(stderr, "%s needs %s, which is in none of the search directories:\n",
			        binary_path, needed[i]);
			for (d = 0; search_dirs[d] != NULL; d++)
				fprintf(stderr, "  %s\n", search_dirs[d]);
			return -1;
		}

		{
			char libdir[PATH_MAX];

			snprintf(libdir, sizeof(libdir), "%s/" CIX_LIB_DIR_RUNTIME, image_root);
			if (test_mkdir_p(libdir) != 0)
				return -1;
		}
		snprintf(dst, sizeof(dst), "%s/" CIX_LIB_DIR_RUNTIME "/%s", image_root, needed[i]);
		if (test_image_fixture_copy_file(resolved, dst) != 0)
			return -1;

		/* A library has its own DT_NEEDED entries, resolved from the
		 * same tree it was found in. */
		if (stage_closure_rec(image_root, resolved, search_dirs, seen) != 0)
			return -1;
	}
	return 0;
}

int test_image_fixture_stage_closure(const char *image_root, const char *binary_path,
                                     const char *const *search_dirs)
{
	struct closure_seen seen;

	seen.n = 0;
	return stage_closure_rec(image_root, binary_path, search_dirs, &seen);
}
/*
 * The loopback HTTP file server behind test_http_server_start() and
 * test_http_source_url(). See test_image_fixture.h for why it exists.
 */

/*
 * The request being answered, "<METHOD> <path>", for the one line every
 * reply logs. A test that fails on a fetch sees only the daemon's
 * summary of it ("The requested URL returned error: 404"); this line is
 * what says which path was asked for and what this server made of it.
 * One per server process, which serves one request at a time.
 */
static char g_http_serving[PATH_MAX + 16] = "(unparsed request)";

static void http_serve_reply(int fd, const char *status, long long length)
{
	char head[160];
	int n = snprintf(head, sizeof(head),
	                 "HTTP/1.0 %s\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",
	                 status, length);

	fprintf(stderr, "    fixture http: %s -> %s\n", g_http_serving, status);

	if (n > 0 && write(fd, head, (size_t)n) != n)
		return;
}

/* Copies the value of header `name` (case-insensitive) out of req, or "". */
static void http_header_value(const char *req, const char *name, char *out, size_t out_size)
{
	size_t nlen = strlen(name);
	const char *line = strstr(req, "\r\n");

	out[0] = '\0';
	while (line != NULL && line[2] != '\r' && line[2] != '\0') {
		const char *h = line + 2;
		const char *end = strstr(h, "\r\n");

		if (strncasecmp(h, name, nlen) == 0 && h[nlen] == ':') {
			const char *v = h + nlen + 1;
			size_t vlen;

			while (*v == ' ')
				v++;
			vlen = end != NULL ? (size_t)(end - v) : strlen(v);
			if (vlen >= out_size)
				vlen = out_size - 1;
			memcpy(out, v, vlen);
			out[vlen] = '\0';
			return;
		}
		line = end;
	}
}

/*
 * Receives a request body of exactly Content-Length bytes into `part`,
 * starting with what the header read already took past "\r\n\r\n".
 * 0 when it all arrived, -1 when `part` cannot be written, -2 when the
 * body came up short -- see http_serve_put() for why a short body is a
 * refusal and never a truncated file.
 */
static int http_receive_body(int fd, const char *req, size_t used, const char *body,
                             const char *part)
{
	char value[64];
	long long want, have;
	FILE *out;

	http_header_value(req, "Content-Length", value, sizeof(value));
	want = atoll(value);
	out = fopen(part, "wb");
	if (out == NULL)
		return -1;
	have = (long long)(used - (size_t)(body - req));
	if (have > want)
		have = want;
	if (have > 0)
		fwrite(body, 1, (size_t)have, out);
	while (have < want) {
		char buf[65536];
		size_t chunk = (size_t)(want - have) < sizeof(buf) ? (size_t)(want - have) : sizeof(buf);
		ssize_t got = read(fd, buf, chunk);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		fwrite(buf, 1, (size_t)got, out);
		have += got;
	}
	if (fclose(out) != 0 || have != want)
		return -2;
	return 0;
}

/*
 * A PUT, for a server started with accept_put: the body is written to
 * root/<basename of the path>, and root/<basename>.headers records the
 * Authorization and X-Cix-Sha256 headers, one per line, so a test can
 * assert on exactly what arrived. It verifies nothing itself.
 *
 * A PUT IS ATOMIC HERE: the body goes to `<path>.part` and is renamed
 * into place only once Content-Length bytes have actually arrived. An
 * incomplete one answers 400 and leaves whatever was already at that
 * path untouched.
 *
 * That is not defensive habit, it is cix#533. This function used to
 * `fopen(path, "wb")` -- truncating immediately -- read until the
 * first `read()` that returned <= 0, and then reply "201 Created"
 * whatever had arrived. Two things followed, and the second is what
 * made it expensive:
 *
 *   - a short read left a TRUNCATED file, and said 201 about it, so
 *     the corruption was silent at the only point that could have
 *     caught it;
 *   - because the open truncates before any body is read, a second
 *     PUT of the same name DESTROYS a good file already there. The
 *     hbpush case does push the same artifact name twice, so a single
 *     short read on the second one turns a correct first upload into
 *     an empty file.
 *
 * It presented as `FAIL: the published hostbuild artifact is a valid
 * gzip`, intermittently, in a FLOOR_SELFTEST -- so it failed a
 * RELEASE at random, and looked exactly like a regression in whatever
 * change happened to be in flight. It cost a full investigation of an
 * unrelated artifact-naming change before an unchanged re-run passed.
 *
 * `read()` is also now retried on EINTR rather than treated as
 * end-of-body, which is the most likely way the short read happened:
 * nothing here blocks signals.
 */
static void http_serve_put(int fd, const char *root, const char *upath, const char *req,
                           size_t used)
{
	const char *base = strrchr(upath, '/');
	const char *body = strstr(req, "\r\n\r\n");
	char path[PATH_MAX], part[PATH_MAX + 8], hpath[PATH_MAX + 16], value[512];
	FILE *out;

	base = base != NULL ? base + 1 : upath;
	if (body == NULL || base[0] == '\0' ||
	    snprintf(path, sizeof(path), "%s/%s", root, base) >= (int)sizeof(path) ||
	    snprintf(part, sizeof(part), "%s.part", path) >= (int)sizeof(part)) {
		http_serve_reply(fd, "400 Bad Request", 0);
		return;
	}
	body += 4;
	switch (http_receive_body(fd, req, used, body, part)) {
	case 0:
		break;
	case -1:
		http_serve_reply(fd, "500 Internal Server Error", 0);
		return;
	default:
		/* Say so, and do not disturb what is already at `path`. A
		 * test asserting on a previous good upload keeps passing;
		 * one asserting on this upload fails with a status rather
		 * than with mysterious bytes. */
		unlink(part);
		http_serve_reply(fd, "400 Bad Request", 0);
		return;
	}
	if (rename(part, path) != 0) {
		unlink(part);
		http_serve_reply(fd, "500 Internal Server Error", 0);
		return;
	}
	snprintf(hpath, sizeof(hpath), "%s.headers", path);
	out = fopen(hpath, "w");
	if (out != NULL) {
		http_header_value(req, "Authorization", value, sizeof(value));
		fprintf(out, "%s\n", value);
		http_header_value(req, "X-Cix-Sha256", value, sizeof(value));
		fprintf(out, "%s\n", value);
		fclose(out);
	}
	http_serve_reply(fd, "201 Created", 0);
}

/*
 * The commit SHA this fixture's fake forge answers every create with.
 * Exported so a test can assert the daemon carried it through.
 */
const char *const TEST_FORGE_COMMIT_SHA = "c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1c1";

/*
 * A minimal stand-in for Gitea's create-file call (ADR-0323), shaped by
 * its own spec (Gitea 1.25.4, git.home.arpa /swagger.v1.json,
 * 2026-10-02): POST .../contents/{filepath} creates the file and answers
 * 201 with a FileResponse carrying `commit`. A path that already exists
 * is refused, which is the forge-side half of a published version being
 * immutable. The request body is kept beside root as
 * <basename>.request.json, so a test reads exactly what was sent.
 */
static void http_serve_forge_create(int fd, const char *root, const char *upath, const char *req,
                                    size_t used)
{
	const char *base = strrchr(upath, '/');
	const char *body = strstr(req, "\r\n\r\n");
	char path[PATH_MAX], part[PATH_MAX + 8], reply[160];
	struct stat st;
	int n;

	base = base != NULL ? base + 1 : upath;
	if (body == NULL || base[0] == '\0' ||
	    snprintf(path, sizeof(path), "%s/%s.request.json", root, base) >= (int)sizeof(path) ||
	    snprintf(part, sizeof(part), "%s.part", path) >= (int)sizeof(part)) {
		http_serve_reply(fd, "400 Bad Request", 0);
		return;
	}
	body += 4;
	if (stat(path, &st) == 0) {
		n = snprintf(reply, sizeof(reply), "{\"message\":\"repository file already exists\"}");
		http_serve_reply(fd, "422 Unprocessable Entity", n);
		if (write(fd, reply, (size_t)n) != n)
			return;
		return;
	}
	if (http_receive_body(fd, req, used, body, part) != 0 || rename(part, path) != 0) {
		unlink(part);
		http_serve_reply(fd, "400 Bad Request", 0);
		return;
	}
	n = snprintf(reply, sizeof(reply), "{\"content\":{\"sha\":\"b10b\"},\"commit\":{\"sha\":\"%s\"}}",
	             TEST_FORGE_COMMIT_SHA);
	http_serve_reply(fd, "201 Created", n);
	if (write(fd, reply, (size_t)n) != n)
		return;
}

/* Serves one request on fd: GET or HEAD of root + the request path, and
 * PUT -- plus POST to a forge's .../contents/ path (ADR-0323) -- when
 * accept_put is set. */
static void http_serve_one(int fd, const char *root, int accept_put)
{
	char req[4096];
	char method[8];
	char upath[PATH_MAX];
	char path[PATH_MAX];
	size_t used = 0;
	ssize_t got;
	struct stat st;
	char *query;
	int file;

	/* Read until the end of the request headers, or the buffer is full. */
	while (used < sizeof(req) - 1) {
		got = read(fd, req + used, sizeof(req) - 1 - used);
		if (got <= 0)
			break;
		used += (size_t)got;
		req[used] = '\0';
		if (strstr(req, "\r\n\r\n") != NULL)
			break;
	}
	req[used] = '\0';
	snprintf(g_http_serving, sizeof(g_http_serving), "(unparsed request)");
	if (sscanf(req, "%7s %4095s", method, upath) == 2)
		snprintf(g_http_serving, sizeof(g_http_serving), "%s %s", method, upath);
	if (sscanf(req, "%7s %4095s", method, upath) != 2 || upath[0] != '/' ||
	    strstr(upath, "..") != NULL) {
		http_serve_reply(fd, "400 Bad Request", 0);
		return;
	}
	if (accept_put && strcmp(method, "POST") == 0 &&
	    (strstr(upath, "/contents/") != NULL ||
	     (strlen(upath) >= 9 && strcmp(upath + strlen(upath) - 9, "/contents") == 0))) {
		http_serve_forge_create(fd, root, upath, req, used);
		return;
	}
	if (accept_put && strcmp(method, "PUT") == 0) {
		http_serve_put(fd, root, upath, req, used);
		return;
	}
	if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
		http_serve_reply(fd, "405 Method Not Allowed", 0);
		return;
	}
	/* A query string names the same file, as python's http.server treated it. */
	query = strchr(upath, '?');
	if (query != NULL)
		*query = '\0';
	/*
	 * A path under /private/<credential>/ is a private repository's
	 * file: served only to a request carrying exactly
	 * "Authorization: Basic <credential>", and 404 otherwise, as Gitea
	 * answers a private repository. test_pkg uses it to prove a fetch
	 * carries its owning source's token and no other (ADR-0324). The
	 * rest of the path names the file.
	 */
	if (strncmp(upath, "/private/", 9) == 0) {
		char want[sizeof(upath) + 40];
		char *rest = strchr(upath + 9, '/');

		if (rest == NULL) {
			http_serve_reply(fd, "404 Not Found", 0);
			return;
		}
		snprintf(want, sizeof(want), "\r\nAuthorization: Basic %.*s\r\n",
		         (int)(rest - (upath + 9)), upath + 9);
		if (strstr(req, want) == NULL) {
			fprintf(stderr, "    fixture http: %s asked for without its credential\n", upath);
			http_serve_reply(fd, "404 Not Found", 0);
			return;
		}
		memmove(upath, rest, strlen(rest) + 1);
	}
	if (snprintf(path, sizeof(path), "%s%s", strcmp(root, "/") == 0 ? "" : root, upath) >=
	        (int)sizeof(path) ||
	    stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		fprintf(stderr, "    fixture http: %s is not a regular file here\n", path);
		http_serve_reply(fd, "404 Not Found", 0);
		return;
	}
	file = open(path, O_RDONLY | O_CLOEXEC);
	if (file < 0) {
		http_serve_reply(fd, "404 Not Found", 0);
		return;
	}
	http_serve_reply(fd, "200 OK", (long long)st.st_size);
	if (strcmp(method, "GET") == 0) {
		char buf[65536];

		while ((got = read(file, buf, sizeof(buf))) > 0) {
			ssize_t off = 0;

			while (off < got) {
				ssize_t w = write(fd, buf + off, (size_t)(got - off));

				if (w <= 0)
					break;
				off += w;
			}
			if (off < got)
				break;
		}
	}
	close(file);
}

int test_http_server_start(const char *root, int accept_put, int *out_port, pid_t *out_pid)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	int lfd;
	pid_t pid;

	if (root == NULL || root[0] != '/')
		return -1;
	lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (lfd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0; /* kernel-chosen, so parallel tests never collide */
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(lfd, 16) != 0 ||
	    getsockname(lfd, (struct sockaddr *)&addr, &len) != 0) {
		close(lfd);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(lfd);
		return -1;
	}
	if (pid == 0) {
		signal(SIGPIPE, SIG_IGN);
		for (;;) {
			int cfd = accept(lfd, NULL, NULL);

			if (cfd < 0) {
				if (errno == EINTR)
					continue;
				_exit(1);
			}
			http_serve_one(cfd, root, accept_put);
			close(cfd);
		}
	}
	close(lfd);
	*out_port = ntohs(addr.sin_port);
	*out_pid = pid;
	return 0;
}

int test_http_server_stop(pid_t pid)
{
	int status;

	if (pid <= 0)
		return -1;
	kill(pid, SIGTERM);
	return waitpid(pid, &status, 0) == pid ? 0 : -1;
}

/*
 * test_http_source_url()'s own server, rooted at "/": one per test
 * process, started on first use and stopped at exit.
 */
static pid_t g_http_source_pid = -1;
static int g_http_source_port;
/* The process that started it. A test that forks and later exit()s in
 * the child runs atexit handlers there too; only the owner stops it. */
static pid_t g_http_source_owner = -1;

static void http_source_stop(void)
{
	if (g_http_source_pid > 0 && getpid() == g_http_source_owner) {
		test_http_server_stop(g_http_source_pid);
		g_http_source_pid = -1;
	}
}

int test_http_source_url(const char *abs_path, char *out, size_t out_size)
{
	int n;

	if (abs_path == NULL || abs_path[0] != '/')
		return -1;
	if (g_http_source_pid <= 0) {
		if (test_http_server_start("/", 0, &g_http_source_port, &g_http_source_pid) != 0)
			return -1;
		g_http_source_owner = getpid();
		atexit(http_source_stop);
	}
	n = snprintf(out, out_size, "http://127.0.0.1:%d%s", g_http_source_port, abs_path);
	if (n < 0 || (size_t)n >= out_size)
		return -1;
	return 0;
}

/*
 * test_http_source_url() for use inside one printf: returns the URL
 * from a ring of buffers, so up to eight can appear in one call (a
 * multi-source recipe names three). A server that cannot start ends
 * the test rather than letting a fixture silently fetch nothing.
 */
const char *test_http_src(const char *abs_path)
{
	static char ring[8][PATH_MAX + 64];
	static unsigned next;
	char *out = ring[next++ % 8];

	if (test_http_source_url(abs_path, out, sizeof(ring[0])) != 0) {
		fprintf(stderr, "test_http_src: cannot serve %s over loopback HTTP\n",
		        abs_path != NULL ? abs_path : "(null)");
		exit(1);
	}
	return out;
}


/*
 * A shell recipe written into the recipe store rather than published.
 * See test_image_fixture.h for why that distinction is the point.
 *
 * Each level is created separately because the daemon may not have
 * made them yet -- a test that seeds before its first install finds
 * <pkg_state_dir> itself absent -- and mkdir() creates one level at a
 * time. EEXIST is the ordinary case for the outer two and is not an
 * error; the fopen() is what actually reports a bad path.
 */
int test_seed_shell_recipe(const char *pkg_state_dir, const char *name, const char *version,
                           const char *content)
{
	char dir[PATH_MAX];
	char path[PATH_MAX];
	FILE *f;

	if (snprintf(dir, sizeof(dir), "%s/recipes", pkg_state_dir) >= (int)sizeof(dir))
		return -1;
	mkdir(pkg_state_dir, 0755);
	mkdir(dir, 0755);
	if (snprintf(dir, sizeof(dir), "%s/recipes/%s", pkg_state_dir, name) >= (int)sizeof(dir))
		return -1;
	mkdir(dir, 0755);
	if (snprintf(dir, sizeof(dir), "%s/recipes/%s/%s", pkg_state_dir, name, version) >=
	    (int)sizeof(dir))
		return -1;
	mkdir(dir, 0755);
	if (snprintf(path, sizeof(path), "%s/build.sh", dir) >= (int)sizeof(path))
		return -1;
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	if (fputs(content, f) == EOF) {
		fclose(f);
		return -1;
	}
	return fclose(f) == 0 ? 0 : -1;
}
