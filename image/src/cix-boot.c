/*
 * cix-boot -- the UEFI boot manager an installed Cix host boots through
 * (ADR-0215).
 *
 * It replaces systemd-boot, and does only what this platform actually
 * used systemd-boot for:
 *
 *   1. read the /loader/entries files off the ESP it was itself loaded
 *      from, and loader.conf's own "default" pattern (#467) -- but not
 *      its "timeout": that bounds how long an interactive menu waits
 *      before auto-booting, and this program has no menu at all, so
 *      there is nothing for a timeout to bound. Two earlier versions
 *      of this comment got "default" wrong in both directions: one
 *      claimed it worked when nothing here read loader.conf at all,
 *      the next correctly said so but then said PUT /v1/system/esp
 *      {"default": ...} was dead code for the SAME reason boot-next
 *      was (next bullet) -- it wasn't the same reason, and #467 is
 *      what actually closes this one.
 *   2. honor an operator's one-shot override (LoaderEntryOneShot, #469)
 *      when one is armed and names a real entry, overriding steps 3-4
 *      for exactly one boot; consumed (deleted) the instant it is
 *      read, whether or not it matched, so a stale value can never
 *      stick. Before #469 this program read no EFI variable at all, so
 *      POST /v1/system/boot-next armed a real NVRAM variable that
 *      nothing here ever looked at -- confirmed dead end to end on
 *      192.168.15.95, 2026-09-14.
 *   3. failing that, narrow to whatever loader.conf's "default"
 *      pattern matches (or the full set, if it names nothing that
 *      exists any more -- a stale pattern must not mean the machine
 *      refuses to boot) and pick the best entry within it -- highest
 *      version, and an entry that has run out of boot attempts only
 *      ever as a last resort
 *   4. decrement the chosen entry's Automatic Boot Assessment counter
 *      by renaming its file (cix-a+3.conf -> cix-a+2-1.conf)
 *   5. load the kernel it names and start it, with the entry's own
 *      "options" line as the kernel command line
 *
 * The entry format is the Boot Loader Specification, unchanged --
 * ADR-0014's counted A/B slots, cix-install's ESP layout and the
 * daemon's own esp.c all keep working, because only the reader changed.
 *
 * The kernel is itself a PE32+ EFI application (the Linux EFI stub), so
 * starting it is LoadImage()+StartImage() with LoadOptions set. There is
 * no filesystem driver, no scripting language and no module loader here,
 * because none of that is needed to do the four things above.
 *
 * No libc: this runs before any operating system exists. The handful of
 * string operations used are written out below rather than pulled in.
 */
#include "uefi.h"

#define MAX_ENTRIES 32
#define ENTRY_TEXT_MAX 4096
#define CMDLINE_MAX 1024
#define NAME_MAX_CHARS 128

/*
 * SBAT metadata (Secure Boot Advanced Targeting). REQUIRED: shim
 * generation 4 and later refuses to load any image without a .sbat
 * section, and reports it as
 *
 *   Verification failed: (0x1A) Security Violation
 *
 * -- the same message a bad signature produces, which is what made this
 * cost three QEMU rounds to find. The image was signed correctly the
 * whole time; sbsign and sbverify both accepted it, because they check
 * signatures and shim additionally checks this.
 *
 * The format is CSV: component, generation, vendor name, package name,
 * version, URL. The generation number is the revocation lever -- if a
 * security hole is ever found in this program, shipping a shim with
 * "cix-boot,2" refuses every copy still declaring 1. That is the entire
 * point of the mechanism, so the number is meaningful and must only be
 * incremented for a real security fix, never routinely.
 *
 * The first line declares which SBAT revision this follows and is
 * required verbatim.
 */
static const char sbat_metadata[] __attribute__((used, section(".sbat"), aligned(512))) =
	"sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
	"cix-boot,1,Cix,cix-boot,1,https://git.home.arpa/itdlabs/cix\n";

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

struct entry {
	CHAR16 filename[NAME_MAX_CHARS]; /* as it appears in /loader/entries */
	char id[NAME_MAX_CHARS];         /* filename minus ".conf" and minus the counter */
	char linux_path[256];            /* the "linux" key, e.g. /cix-bzImage-a */
	char options[1024];              /* the "options" key -- the kernel command line */
	int version;                     /* the "version" key; 0 when absent */
	int tries_left;                  /* -1 when the entry carries no counter */
	int tries_done;
};

/* ---- freestanding string helpers ---- */

static UINTN str16_len(const CHAR16 *s)
{
	UINTN n = 0;

	while (s[n] != 0)
		n++;
	return n;
}

static void str_copy(char *dst, UINTN dst_size, const char *src, UINTN n)
{
	UINTN i;

	for (i = 0; i < n && i + 1 < dst_size && src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static void print(const CHAR16 *s)
{
	ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s);
}

/* ASCII to UCS-2, for handing a command line to the kernel. */
static void ascii_to_utf16(const char *src, CHAR16 *dst, UINTN dst_chars)
{
	UINTN i;

	for (i = 0; i + 1 < dst_chars && src[i] != '\0'; i++)
		dst[i] = (CHAR16)(unsigned char)src[i];
	dst[i] = 0;
}

static int digit_value(CHAR16 c)
{
	return (c >= '0' && c <= '9') ? (int)(c - '0') : -1;
}

/* ---- entry filename parsing ----
 *
 * "cix-a+3.conf"   -> id "cix-a", tries_left 3, tries_done 0
 * "cix-a+2-1.conf" -> id "cix-a", tries_left 2, tries_done 1
 * "cix-a.conf"     -> id "cix-a", tries_left -1 (a confirmed entry)
 *
 * The id excludes the counter deliberately: an entry keeps its identity
 * as its counter changes, which is what makes "this is the same entry I
 * booted last time" answerable at all.
 */
static int parse_entry_name(const CHAR16 *name, struct entry *e)
{
	UINTN len = str16_len(name);
	UINTN stem_len, i;
	int seen_plus = -1;

	if (len < 6)
		return -1;
	/* Must end in ".conf". */
	if (name[len - 5] != '.' || name[len - 4] != 'c' || name[len - 3] != 'o' ||
	    name[len - 2] != 'n' || name[len - 1] != 'f')
		return -1;
	stem_len = len - 5;

	for (i = 0; i < stem_len; i++) {
		if (name[i] == '+')
			seen_plus = (int)i;
	}

	e->tries_left = -1;
	e->tries_done = 0;

	if (seen_plus < 0) {
		/* No counter: the whole stem is the id. */
		for (i = 0; i < stem_len && i + 1 < NAME_MAX_CHARS; i++)
			e->id[i] = (char)name[i];
		e->id[i] = '\0';
		return 0;
	}

	for (i = 0; (int)i < seen_plus && i + 1 < NAME_MAX_CHARS; i++)
		e->id[i] = (char)name[i];
	e->id[i] = '\0';

	{
		int left = 0, done = 0, have_left = 0, in_done = 0;

		for (i = (UINTN)seen_plus + 1; i < stem_len; i++) {
			int d = digit_value(name[i]);

			if (name[i] == '-' && have_left && !in_done) {
				in_done = 1;
				continue;
			}
			if (d < 0)
				return -1;
			if (in_done)
				done = done * 10 + d;
			else {
				left = left * 10 + d;
				have_left = 1;
			}
		}
		if (!have_left)
			return -1;
		e->tries_left = left;
		e->tries_done = done;
	}
	return 0;
}

/* ---- entry file parsing ---- */

static void parse_entry_text(char *text, UINTN len, struct entry *e)
{
	UINTN i = 0;

	e->linux_path[0] = '\0';
	e->options[0] = '\0';
	e->version = 0;

	while (i < len) {
		UINTN start = i, key_end, val_start;

		while (i < len && text[i] != '\n')
			i++;
		/* [start, i) is one line. */
		key_end = start;
		while (key_end < i && text[key_end] != ' ' && text[key_end] != '\t')
			key_end++;
		val_start = key_end;
		while (val_start < i && (text[val_start] == ' ' || text[val_start] == '\t'))
			val_start++;

		{
			char key[32];
			UINTN klen = key_end - start;
			UINTN vlen = i - val_start;

			/* Trim a trailing CR, so a file written on any host parses. */
			if (vlen > 0 && text[val_start + vlen - 1] == '\r')
				vlen--;

			if (klen > 0 && klen < sizeof(key)) {
				str_copy(key, sizeof(key), text + start, klen);
				if (str_eq(key, "linux"))
					str_copy(e->linux_path, sizeof(e->linux_path),
					         text + val_start, vlen);
				else if (str_eq(key, "options"))
					str_copy(e->options, sizeof(e->options),
					         text + val_start, vlen);
				else if (str_eq(key, "version")) {
					UINTN k;
					int v = 0;

					for (k = 0; k < vlen; k++) {
						char c = text[val_start + k];

						if (c < '0' || c > '9')
							break;
						v = v * 10 + (c - '0');
					}
					e->version = v;
				}
			}
		}
		i++; /* step over the newline */
	}
}

/*
 * ---- LoaderEntryOneShot: an operator-selected slot, once (#469) ----
 *
 * cixd's esp_boot_next_set() (POST /v1/system/boot-next) already wrote
 * a real UEFI variable -- Linux's efivarfs is just the kernel's own
 * file view onto this same NVRAM store, so what it persisted is
 * exactly what GetVariable() below reads back. What was missing was a
 * reader: this program used to imitate systemd-boot's on-disk entry
 * format without imitating this half of its interface, so the REST
 * endpoint documented "boot the given slot once" while nothing here
 * ever looked at the variable it wrote. Confirmed dead end to end on
 * 192.168.15.95, 2026-09-14: boot_next read back armed for slot b
 * after a real reboot (kmsg showed a fresh boot), and the machine
 * still came up on slot a.
 */
#define CIX_LOADER_GUID                                                                           \
	{                                                                                          \
		0x4a67b082, 0x0a4c, 0x41cf, { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f }    \
	}

/*
 * Returns the bare entry id ("cix-b") the variable names, or an empty
 * string if it was absent, empty, or too long to fit -- any of which
 * means "nothing armed", never an error worth stopping boot for.
 *
 * Always consumes the variable when present, whether or not it goes on
 * to match a real entry: a one-shot that firmware read but did not
 * clear would arm forever, which is worse than falling through to
 * ordinary version-based selection for one boot.
 */
static int read_oneshot_id(char *out, UINTN out_size)
{
	EFI_GUID guid = CIX_LOADER_GUID;
	CHAR16 buf[NAME_MAX_CHARS];
	UINTN size = sizeof(buf);
	UINTN chars, i;
	EFI_STATUS st;

	out[0] = '\0';
	st = ST->RuntimeServices->GetVariable(L16("LoaderEntryOneShot"), &guid, 0, &size, buf);
	if (EFI_ERROR(st))
		return 0;

	ST->RuntimeServices->SetVariable(L16("LoaderEntryOneShot"), &guid, 0, 0, 0);

	chars = size / sizeof(CHAR16);
	/* esp_boot_next_set() writes a trailing UTF-16 NUL; drop it, then
	 * strip ".conf" -- parse_entry_name()'s own e->id never carries
	 * either, and that is what this gets compared against. */
	if (chars > 0 && buf[chars - 1] == 0)
		chars--;
	if (chars >= 5 && buf[chars - 5] == '.' && buf[chars - 4] == 'c' && buf[chars - 3] == 'o' &&
	    buf[chars - 2] == 'n' && buf[chars - 1] == 'f')
		chars -= 5;

	for (i = 0; i < chars && i + 1 < out_size; i++)
		out[i] = (char)buf[i];
	out[i] = '\0';
	return out[0] != '\0';
}

/*
 * The armed id names an entry by id, counter suffix and all stripped
 * (parse_entry_name() already did the same to list[]), so an operator
 * override finds its entry whether or not that entry currently carries
 * an Automatic Boot Assessment counter -- and matches even one whose
 * counter has reached zero, deliberately bypassing pick_entry()'s
 * exhausted-entry filter: forcing a slot the accounting has given up on
 * is the entire point of a manual override.
 */
static int find_entry_by_id(struct entry *list, int n, const char *id)
{
	int i;

	for (i = 0; i < n; i++) {
		if (list[i].linux_path[0] != '\0' && str_eq(list[i].id, id))
			return i;
	}
	return -1;
}

/* ---- ESP access ---- */

static EFI_STATUS open_esp_root(EFI_HANDLE image, EFI_FILE_PROTOCOL **root)
{
	EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_LOADED_IMAGE_PROTOCOL *li;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
	EFI_STATUS st;

	/* The volume this program was itself loaded from -- so entries are
	 * read from the ESP that booted us, never from a guess about which
	 * disk is which. */
	st = BS->HandleProtocol(image, &li_guid, (void **)&li);
	if (EFI_ERROR(st))
		return st;
	st = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs);
	if (EFI_ERROR(st))
		return st;
	return fs->OpenVolume(fs, root);
}

static EFI_STATUS read_whole_file(EFI_FILE_PROTOCOL *dir, CHAR16 *name, void **buf, UINTN *size)
{
	EFI_GUID info_guid = EFI_FILE_INFO_GUID;
	EFI_FILE_PROTOCOL *f;
	EFI_STATUS st;
	UINT8 info_buf[sizeof(EFI_FILE_INFO) + NAME_MAX_CHARS * sizeof(CHAR16)];
	UINTN info_size = sizeof(info_buf);
	EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;

	st = dir->Open(dir, &f, name, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st))
		return st;
	st = f->GetInfo(f, &info_guid, &info_size, info_buf);
	if (EFI_ERROR(st)) {
		f->Close(f);
		return st;
	}
	*size = (UINTN)info->FileSize;
	st = BS->AllocatePool(EfiLoaderData, *size + 1, buf);
	if (EFI_ERROR(st)) {
		f->Close(f);
		return st;
	}
	st = f->Read(f, size, *buf);
	f->Close(f);
	if (EFI_ERROR(st)) {
		BS->FreePool(*buf);
		return st;
	}
	((char *)*buf)[*size] = '\0';
	return EFI_SUCCESS;
}

/*
 * Decrements the entry's counter by renaming its file, which is how the
 * Boot Loader Specification records a boot attempt: the count lives in
 * the filename, so the record survives a machine that never reaches
 * userspace -- which is the entire point of the mechanism.
 *
 * A failure here is deliberately NOT fatal. Refusing to boot because a
 * counter could not be written would turn a read-only or full ESP into
 * an unbootable machine, which is a far worse outcome than booting with
 * an un-decremented counter.
 */
static void decrement_tries(EFI_FILE_PROTOCOL *entries, struct entry *e)
{
	EFI_GUID info_guid = EFI_FILE_INFO_GUID;
	EFI_FILE_PROTOCOL *f;
	UINT8 info_buf[sizeof(EFI_FILE_INFO) + NAME_MAX_CHARS * sizeof(CHAR16)];
	UINTN info_size = sizeof(info_buf);
	EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
	EFI_STATUS st;
	CHAR16 newname[NAME_MAX_CHARS];
	UINTN n = 0;
	int left = e->tries_left - 1;
	int done = e->tries_done + 1;
	UINTN i;

	if (e->tries_left <= 0)
		return;

	st = entries->Open(entries, &f, e->filename,
	                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
	if (EFI_ERROR(st))
		return;
	st = f->GetInfo(f, &info_guid, &info_size, info_buf);
	if (EFI_ERROR(st)) {
		f->Close(f);
		return;
	}

	for (i = 0; e->id[i] != '\0' && n + 12 < NAME_MAX_CHARS; i++)
		newname[n++] = (CHAR16)e->id[i];
	newname[n++] = '+';
	if (left >= 10)
		newname[n++] = (CHAR16)('0' + (left / 10) % 10);
	newname[n++] = (CHAR16)('0' + left % 10);
	newname[n++] = '-';
	if (done >= 10)
		newname[n++] = (CHAR16)('0' + (done / 10) % 10);
	newname[n++] = (CHAR16)('0' + done % 10);
	newname[n++] = '.';
	newname[n++] = 'c';
	newname[n++] = 'o';
	newname[n++] = 'n';
	newname[n++] = 'f';
	newname[n] = 0;

	/* SetInfo with a changed FileName is the UEFI way to rename. */
	for (i = 0; i <= n; i++)
		info->FileName[i] = newname[i];
	info->Size = sizeof(EFI_FILE_INFO) + (n + 1) * sizeof(CHAR16);
	f->SetInfo(f, &info_guid, (UINTN)info->Size, info_buf);
	f->Close(f);
}

/*
 * ---- loader.conf's "default" pattern (#467) ----
 *
 * The other half of what PUT /v1/system/esp writes. Before this,
 * cix-boot.c never opened loader.conf at all -- confirmed by grepping
 * the whole file during #469's own investigation -- so "default" and
 * "timeout" both silently did nothing on a real boot despite esp.c
 * genuinely, correctly persisting them. "timeout" stays that way,
 * deliberately, not by oversight: it controls how long systemd-boot's
 * interactive menu waits before auto-booting, and this program has no
 * menu at all (no keypress handling exists anywhere in this file) --
 * there is nothing for a timeout to bound. Implementing it would be
 * motion without effect. "default" is different: it genuinely narrows
 * which entry gets chosen, so it gets implemented.
 */

/*
 * A minimal glob: '*' matches any run of characters (including none),
 * '?' matches exactly one character, anything else matches itself.
 * No character classes ([...]) -- no pattern this platform's own
 * tooling ever writes uses one (every real default value written here
 * is "cix-*"), and esp_pattern_matches() (daemon/src/esp.c), which
 * this mirrors, is a thin wrapper over glibc's real fnmatch() with no
 * flags -- there is no libc here to call, so this is deliberately a
 * subset rather than a hand-rolled reimplementation of the whole
 * thing. Classic backtracking match, not a novel algorithm.
 */
static int glob_match(const char *pattern, const char *s)
{
	const char *star_p = 0, *star_s = 0;

	while (*s != '\0') {
		if (*pattern == '*') {
			star_p = pattern + 1;
			star_s = s;
			pattern++;
		} else if (*pattern == '?' || *pattern == *s) {
			pattern++;
			s++;
		} else if (star_p != 0) {
			pattern = star_p;
			star_s++;
			s = star_s;
		} else {
			return 0;
		}
	}
	while (*pattern == '*')
		pattern++;
	return *pattern == '\0';
}

/*
 * Reads loader.conf's "default" value, or leaves out[0] a NUL if the
 * file is absent or carries none -- both ordinary (a fresh install's
 * loader.conf may not exist yet, and PUT /v1/system/esp's own default
 * is optional). Matched against each entry's id (list[].id, counter
 * already stripped) only -- not also the raw filename the way
 * esp_pattern_matches() tries as a second spelling, since every
 * pattern this platform writes already matches the id form and a
 * second spelling here is complexity with nothing real to justify it.
 */
static void read_default_pattern(EFI_FILE_PROTOCOL *root, char *out, size_t out_size)
{
	CHAR16 path[] = { '\\', 'l', 'o', 'a', 'd', 'e', 'r', '\\', 'l', 'o', 'a',
		          'd', 'e', 'r', '.', 'c', 'o', 'n', 'f', 0 };
	void *buf = 0;
	UINTN size = 0;
	UINTN i = 0;
	char *text;

	out[0] = '\0';
	if (EFI_ERROR(read_whole_file(root, path, &buf, &size)))
		return;
	if (size > ENTRY_TEXT_MAX)
		size = ENTRY_TEXT_MAX;
	text = (char *)buf;

	while (i < size) {
		UINTN start = i, key_end, val_start, val_end, vlen, k;

		while (i < size && text[i] != '\n')
			i++;
		key_end = start;
		while (key_end < i && text[key_end] != ' ' && text[key_end] != '\t')
			key_end++;
		val_start = key_end;
		while (val_start < i && (text[val_start] == ' ' || text[val_start] == '\t'))
			val_start++;
		val_end = i;
		if (val_end > val_start && text[val_end - 1] == '\r')
			val_end--;

		if (key_end - start == 7 && text[start] == 'd' && text[start + 1] == 'e' &&
		    text[start + 2] == 'f' && text[start + 3] == 'a' && text[start + 4] == 'u' &&
		    text[start + 5] == 'l' && text[start + 6] == 't') {
			vlen = val_end - val_start;
			if (vlen + 1 > out_size)
				vlen = out_size - 1;
			for (k = 0; k < vlen; k++)
				out[k] = text[val_start + k];
			out[vlen] = '\0';
			break;
		}
		i++; /* step over the newline */
	}
	BS->FreePool(buf);
}

/*
 * Best entry: pattern first (empty pattern is "no restriction"), then
 * highest version wins among what the pattern allows. An entry whose
 * counter has reached zero is chosen only when nothing else within the
 * pattern is available -- it is a failed slot, but a machine with two
 * failed slots should still try to boot rather than sit at firmware.
 *
 * A pattern matching nothing at all returns -1 rather than silently
 * falling back to the unrestricted set itself -- that fallback is the
 * CALLER's decision (efi_main() makes it explicitly), not something to
 * bury inside the one function whose whole job is "does the pattern
 * apply".
 */
static int pick_entry(struct entry *list, int n, const char *pattern)
{
	int best = -1, i;

	for (i = 0; i < n; i++) {
		if (list[i].linux_path[0] == '\0')
			continue;
		if (list[i].tries_left == 0)
			continue;
		if (pattern[0] != '\0' && !glob_match(pattern, list[i].id))
			continue;
		if (best < 0 || list[i].version > list[best].version)
			best = i;
	}
	if (best >= 0)
		return best;

	for (i = 0; i < n; i++) {
		if (list[i].linux_path[0] == '\0')
			continue;
		if (pattern[0] != '\0' && !glob_match(pattern, list[i].id))
			continue;
		if (best < 0 || list[i].version > list[best].version)
			best = i;
	}
	return best;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_FILE_PROTOCOL *root, *entries;
	/*
	 * The entry table and the command-line buffer come from the
	 * firmware's pool, not from static storage, and that is a hard
	 * requirement rather than a style choice.
	 *
	 * As static arrays they landed in .bss -- about 55 KB of it -- and
	 * ld's PE writer computes SizeOfImage from file-backed sections
	 * only. The result was a header claiming SizeOfImage 0x13000 while
	 * .bss and .idata actually extended past 0x22000: a malformed image
	 * that sbsign and sbverify both accepted (they only hash it) and
	 * shim rejected on a real boot with "Verification failed: (0x1A)
	 * Security Violation". Pool allocation leaves .bss essentially
	 * empty, so the header describes the file.
	 */
	struct entry *list;
	CHAR16 *cmdline;
	int count = 0, chosen;
	EFI_STATUS status;
	CHAR16 entries_path[] = { '\\', 'l', 'o', 'a', 'd', 'e', 'r', '\\',
		                  'e',  'n', 't', 'r', 'i', 'e', 's', 0 };

	ST = st;
	BS = st->BootServices;

	if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, MAX_ENTRIES * sizeof(struct entry),
	                               (void **)&list)) ||
	    EFI_ERROR(BS->AllocatePool(EfiLoaderData, CMDLINE_MAX * sizeof(CHAR16),
	                               (void **)&cmdline))) {
		print(L16("cix-boot: out of pool memory\r\n"));
		return EFI_LOAD_ERROR;
	}

	status = open_esp_root(image, &root);
	if (EFI_ERROR(status)) {
		print(L16("cix-boot: cannot open the ESP this image was loaded from\r\n"));
		return status;
	}
	status = root->Open(root, &entries, entries_path, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(status)) {
		print(L16("cix-boot: no \\loader\\entries directory on the ESP\r\n"));
		return status;
	}

	for (;;) {
		UINT8 info_buf[sizeof(EFI_FILE_INFO) + NAME_MAX_CHARS * sizeof(CHAR16)];
		UINTN info_size = sizeof(info_buf);
		EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
		struct entry *e;
		void *text = 0;
		UINTN text_size = 0;
		UINTN i;

		status = entries->Read(entries, &info_size, info_buf);
		if (EFI_ERROR(status) || info_size == 0)
			break;
		if (info->Attribute & EFI_FILE_DIRECTORY)
			continue;
		if (count >= MAX_ENTRIES)
			break;

		e = &list[count];
		if (parse_entry_name(info->FileName, e) != 0)
			continue; /* not an entry file */
		for (i = 0; i < NAME_MAX_CHARS - 1 && info->FileName[i] != 0; i++)
			e->filename[i] = info->FileName[i];
		e->filename[i] = 0;

		if (EFI_ERROR(read_whole_file(entries, e->filename, &text, &text_size)))
			continue;
		if (text_size > ENTRY_TEXT_MAX)
			text_size = ENTRY_TEXT_MAX;
		parse_entry_text((char *)text, text_size, e);
		BS->FreePool(text);
		count++;
	}
	entries->Close(entries);

	{
		char oneshot_id[NAME_MAX_CHARS];

		chosen = -1;
		if (read_oneshot_id(oneshot_id, sizeof(oneshot_id)))
			chosen = find_entry_by_id(list, count, oneshot_id);
	}
	if (chosen < 0) {
		char default_pattern[NAME_MAX_CHARS];

		read_default_pattern(root, default_pattern, sizeof(default_pattern));
		chosen = pick_entry(list, count, default_pattern);
		if (chosen < 0 && default_pattern[0] != '\0')
			/* The pattern matched nothing -- an entry it named was
			 * deleted, or it was never valid for what is actually on
			 * this ESP. A stale pattern must not mean the machine
			 * refuses to boot; esp_loader_set() already refuses to
			 * WRITE a pattern matching zero entries (ESP_ERR_WOULD_
			 * ORPHAN), but that does not protect against an entry
			 * being removed afterward. */
			chosen = pick_entry(list, count, "");
	}
	if (chosen < 0) {
		print(L16("cix-boot: no bootable entry found\r\n"));
		return EFI_NOT_FOUND;
	}

	/* Record the attempt BEFORE handing control to the kernel: a kernel
	 * that hangs must still have consumed one try, or the counter never
	 * makes progress and a broken slot is retried forever. */
	if (list[chosen].tries_left > 0) {
		status = root->Open(root, &entries, entries_path,
		                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
		if (!EFI_ERROR(status)) {
			decrement_tries(entries, &list[chosen]);
			entries->Close(entries);
		}
	}

	{
		CHAR16 kernel_path[256];
		void *kbuf = 0;
		UINTN ksize = 0;
		EFI_HANDLE kimage;
		EFI_LOADED_IMAGE_PROTOCOL *kli;
		UINTN i;

		/* The BLS "linux" value is an absolute path with forward
		 * slashes; EFI file paths use backslashes. */
		for (i = 0; i < 255 && list[chosen].linux_path[i] != '\0'; i++) {
			char c = list[chosen].linux_path[i];

			kernel_path[i] = (CHAR16)(c == '/' ? '\\' : c);
		}
		kernel_path[i] = 0;

		if (EFI_ERROR(read_whole_file(root, kernel_path, &kbuf, &ksize))) {
			print(L16("cix-boot: cannot read the kernel named by the entry\r\n"));
			root->Close(root);
			return EFI_NOT_FOUND;
		}
		root->Close(root);

		/* SourceBuffer form: the kernel is already in memory, so no
		 * device path has to be constructed for it. */
		status = BS->LoadImage(0, image, 0, kbuf, ksize, &kimage);
		BS->FreePool(kbuf);
		if (EFI_ERROR(status)) {
			print(L16("cix-boot: firmware refused to load the kernel image\r\n"));
			return status;
		}

		/* The Linux EFI stub takes its whole command line from
		 * LoadOptions, as UCS-2. */
		ascii_to_utf16(list[chosen].options, cmdline, CMDLINE_MAX);
		if (!EFI_ERROR(BS->HandleProtocol(kimage, &li_guid, (void **)&kli))) {
			kli->LoadOptions = cmdline;
			kli->LoadOptionsSize = (UINT32)((str16_len(cmdline) + 1) * sizeof(CHAR16));
		}

		status = BS->StartImage(kimage, 0, 0);
		print(L16("cix-boot: the kernel returned, which it should never do\r\n"));
	}

	return status;
}
