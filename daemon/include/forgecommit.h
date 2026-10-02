#ifndef FORGECOMMIT_H
#define FORGECOMMIT_H

#include <stddef.h>

/*
 * ADR-0323: the platform commits a recipe it wrote to the recipe
 * repository BEFORE publishing it, so git holds every revision a host
 * ever built and the commit itself is the audit record of why.
 *
 * One file, one commit, created and never overwritten. A forge refuses
 * to create a path that already exists, and that refusal is the
 * immutability guard at the far end: a published recipe version is
 * never rewritten (ADR-0107), so there is nothing to pre-check here
 * that the forge does not already check atomically.
 *
 * Gitea only. Its create-file call was read from the server's own API
 * spec (git.home.arpa, Gitea 1.25.4, /swagger.v1.json, 2026-10-02):
 * POST /api/v1/repos/{owner}/{repo}/contents/{filepath} with a JSON
 * CreateFileOptions body whose `content` is base64 and required,
 * answered 201 with a FileResponse carrying `commit`, or
 * 403/404/422/423. GitHub and GitLab have their own shapes and are
 * added when a host needs them, not guessed at here.
 */

struct forge_target {
	const char *kind;   /* "gitea" */
	const char *scheme; /* "https" */
	const char *host;
	const char *owner;
	const char *repo;
	const char *token;  /* write access; never logged */
	const char *branch; /* the ref the host syncs from */
};

#define FORGE_COMMIT_SHA_MAX 65

/*
 * The CreateFileOptions body for one file: base64 content, the target
 * branch, and the commit message. malloc'd, NUL-terminated; NULL if it
 * cannot be built. Pure, so a test can check it byte for byte.
 */
char *forge_gitea_create_body(const char *content, size_t content_len, const char *branch,
                              const char *message);

/*
 * The commit sha from a FileResponse body (`commit.sha`). 0 when one
 * is present, -1 otherwise. Pure.
 */
int forge_gitea_commit_sha(const char *response, size_t response_len, char *sha,
                           size_t sha_size);

/*
 * Creates `path` in the target's repository, on its branch, as one
 * commit. Blocking network I/O: call it only from a forked child, as
 * every curlfetch_perform() caller does (#285). scratch_dir holds the
 * request and response bodies. 0 on a 201 with the commit sha in
 * `sha`; -1 otherwise, with `err` saying what the forge said -- for a
 * 422 that is usually that the file already exists, which is the
 * refusal this design relies on, and it is reported as such.
 */
int forge_create_file(const struct forge_target *t, const char *path, const char *content,
                      size_t content_len, const char *message, const char *scratch_dir,
                      char *sha, size_t sha_size, long *http_status, char *err, size_t err_size);


/*
 * ADR-0324: an approval a host writes into a recipe after a build
 * writes back to git. That is the one edit a published recipe version
 * ever takes, so it is the one place this module replaces a file
 * rather than creating one -- and it never does so blind: the caller
 * reads the file first (forge_get_file), checks git's text is that
 * version's text without the approval, and the replacement names the
 * blob it read, which the forge enforces as an optimistic lock.
 *
 * Gitea's batch call, read from the server's own API spec (Gitea
 * 1.25.4, /swagger.v1.json, 2026-10-02): POST
 * /api/v1/repos/{owner}/{repo}/contents (repoChangeFiles) with a
 * ChangeFilesOptions body -- `files`, each a ChangeFileOperation of
 * `operation` "update", `path`, base64 `content` and the current blob
 * `sha` -- answered 201 with a FilesResponse whose `commit.sha` sits
 * where a FileResponse's does. One commit for the whole batch, because
 * every commit to the owner's forge is mirrored to the public catalogue.
 */
struct forge_file_update {
	const char *path;
	const char *content;
	size_t content_len;
	const char *blob_sha; /* the blob forge_get_file() read: the lock */
};

/* The ChangeFilesOptions body, every file an "update". Pure. */
char *forge_gitea_update_body(const struct forge_file_update *files, int count, const char *branch,
                              const char *message);

/*
 * Reads `path` on the target's branch (repoGetContents): its blob sha
 * and its decoded content, malloc'd and NUL-terminated. 0 on success,
 * 1 when the forge has no such file (404), -1 otherwise with err set.
 * Blocking network I/O, as forge_create_file().
 */
int forge_get_file(const struct forge_target *t, const char *path, const char *scratch_dir,
                   char *blob_sha, size_t blob_sha_size, char **content, size_t *content_len,
                   long *http_status, char *err, size_t err_size);

/* Replaces every file in one commit. 0 on a 201 with its commit sha in `sha`. */
int forge_update_files(const struct forge_target *t, const struct forge_file_update *files,
                       int count, const char *message, const char *scratch_dir, char *sha,
                       size_t sha_size, long *http_status, char *err, size_t err_size);

#endif /* FORGECOMMIT_H */
