#include "forgecommit.h"

#include "base64.h"
#include "curlfetch.h"
#include "json.h"
#include "persist.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *forge_gitea_create_body(const char *content, size_t content_len, const char *branch,
                              const char *message)
{
	struct json_writer w;
	size_t b64_size;
	char *b64;
	char *out;

	if (content == NULL || content_len == 0 || branch == NULL || message == NULL)
		return NULL;
	b64_size = ((content_len + 2) / 3) * 4 + 1;
	b64 = malloc(b64_size);
	if (b64 == NULL)
		return NULL;
	if (base64_encode((const unsigned char *)content, content_len, b64, b64_size) != 0) {
		free(b64);
		return NULL;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "branch");
	jw_str(&w, branch);
	jw_key(&w, "content");
	jw_str(&w, b64);
	jw_key(&w, "message");
	jw_str(&w, message);
	jw_obj_close(&w);
	free(b64);

	out = malloc(w.len + 1);
	if (out != NULL) {
		memcpy(out, w.buf, w.len);
		out[w.len] = '\0';
	}
	jw_free(&w);
	return out;
}

int forge_gitea_commit_sha(const char *response, size_t response_len, char *sha, size_t sha_size)
{
	struct json_value *root;
	const char *s;
	int rc = -1;

	if (sha_size == 0)
		return -1;
	sha[0] = '\0';
	if (response == NULL || response_len == 0)
		return -1;
	root = json_parse(response, response_len);
	if (root == NULL)
		return -1;
	s = json_as_string(json_object_get(json_object_get(root, "commit"), "sha"));
	if (s != NULL && s[0] != '\0' && strlen(s) < sha_size) {
		snprintf(sha, sha_size, "%s", s);
		rc = 0;
	}
	json_free(root);
	return rc;
}

/*
 * What the forge said, for an error message: its JSON `message` when
 * the body carries one, which is how Gitea explains a 4xx.
 */
static void forge_message(const char *path, char *out, size_t out_size)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;
	const char *m;

	out[0] = '\0';
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return;
	m = json_as_string(json_object_get(root, "message"));
	if (m != NULL)
		snprintf(out, out_size, "%s", m);
	json_free(root);
}

int forge_create_file(const struct forge_target *t, const char *path, const char *content,
                      size_t content_len, const char *message, const char *scratch_dir,
                      char *sha, size_t sha_size, long *http_status, char *err, size_t err_size)
{
	char url[2048];
	char auth[512];
	char body_path[PATH_MAX], resp_path[PATH_MAX];
	char cerr[256];
	char said[256];
	struct curlfetch_opts opts;
	char *body;
	char *resp = NULL;
	size_t resp_len = 0;
	long status = 0;
	int rc;

	sha[0] = '\0';
	*http_status = 0;
	if (t == NULL || t->kind == NULL || strcmp(t->kind, "gitea") != 0) {
		snprintf(err, err_size, "committing a recipe is implemented for a gitea repository only");
		return -1;
	}
	if (t->token == NULL || t->token[0] == '\0') {
		snprintf(err, err_size, "the recipe repository has no token, so nothing can be committed");
		return -1;
	}
	if ((size_t)snprintf(url, sizeof(url), "%s://%s/api/v1/repos/%s/%s/contents/%s", t->scheme,
	                     t->host, t->owner, t->repo, path) >= sizeof(url) ||
	    (size_t)snprintf(auth, sizeof(auth), "Authorization: token %s", t->token) >=
	        sizeof(auth) ||
	    (size_t)snprintf(body_path, sizeof(body_path), "%s/forge-request.json", scratch_dir) >=
	        sizeof(body_path) ||
	    (size_t)snprintf(resp_path, sizeof(resp_path), "%s/forge-response.json", scratch_dir) >=
	        sizeof(resp_path)) {
		snprintf(err, err_size, "a commit request path does not fit");
		return -1;
	}

	body = forge_gitea_create_body(content, content_len, t->branch, message);
	if (body == NULL) {
		snprintf(err, err_size, "could not build the commit request");
		return -1;
	}
	rc = persist_atomic_write(body_path, body, strlen(body));
	free(body);
	if (rc != 0) {
		snprintf(err, err_size, "could not write the commit request to %s", body_path);
		return -1;
	}

	memset(&opts, 0, sizeof(opts));
	opts.url = url;
	opts.path = body_path;
	opts.upload = 1;
	opts.method = "POST";
	opts.response_path = resp_path;
	opts.header1 = auth;
	opts.header2 = "Content-Type: application/json";
	opts.connect_timeout = 15;
	opts.max_time = 60;
	cerr[0] = '\0';
	rc = curlfetch_perform(&opts, &status, cerr, sizeof(cerr));
	*http_status = status;
	if (rc != 0) {
		snprintf(err, err_size, "the commit request to %s failed: %s", t->host, cerr);
		return -1;
	}
	if (status != 201) {
		forge_message(resp_path, said, sizeof(said));
		snprintf(err, err_size, "%s refused the commit of %s with HTTP %ld%s%s", t->host, path,
		         status, said[0] != '\0' ? ": " : "", said);
		return -1;
	}
	if (persist_read_file(resp_path, &resp, &resp_len) != 0 ||
	    forge_gitea_commit_sha(resp, resp_len, sha, sha_size) != 0) {
		free(resp);
		snprintf(err, err_size, "%s answered 201 for %s but named no commit", t->host, path);
		return -1;
	}
	free(resp);
	return 0;
}

/* The API url for `suffix` under the target's repository, and its token header. */
static int forge_api(const struct forge_target *t, const char *suffix, char *url, size_t url_size,
                     char *auth, size_t auth_size, char *err, size_t err_size)
{
	if (t == NULL || t->kind == NULL || strcmp(t->kind, "gitea") != 0) {
		snprintf(err, err_size, "writing to a recipe repository is implemented for gitea only");
		return -1;
	}
	if (t->token == NULL || t->token[0] == '\0') {
		snprintf(err, err_size, "the recipe repository has no token, so nothing can be written");
		return -1;
	}
	if ((size_t)snprintf(url, url_size, "%s://%s/api/v1/repos/%s/%s/contents%s", t->scheme,
	                     t->host, t->owner, t->repo, suffix) >= url_size ||
	    (size_t)snprintf(auth, auth_size, "Authorization: token %s", t->token) >= auth_size) {
		snprintf(err, err_size, "a forge request does not fit");
		return -1;
	}
	return 0;
}

char *forge_gitea_update_body(const struct forge_file_update *files, int count, const char *branch,
                              const char *message)
{
	struct json_writer w;
	char *out;
	int i;

	if (files == NULL || count <= 0 || branch == NULL || message == NULL)
		return NULL;
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "branch");
	jw_str(&w, branch);
	jw_key(&w, "message");
	jw_str(&w, message);
	jw_key(&w, "files");
	jw_arr_open(&w);
	for (i = 0; i < count; i++) {
		size_t b64_size = ((files[i].content_len + 2) / 3) * 4 + 1;
		char *b64;
		int create = files[i].operation != NULL && strcmp(files[i].operation, "create") == 0;

		if (files[i].path == NULL || files[i].content == NULL || files[i].content_len == 0 ||
		    (!create && (files[i].blob_sha == NULL || files[i].blob_sha[0] == '\0'))) {
			jw_free(&w);
			return NULL;
		}
		b64 = malloc(b64_size);
		if (b64 == NULL || base64_encode((const unsigned char *)files[i].content,
		                                 files[i].content_len, b64, b64_size) != 0) {
			free(b64);
			jw_free(&w);
			return NULL;
		}
		jw_obj_open(&w);
		jw_key(&w, "operation");
		jw_str(&w, create ? "create" : "update");
		jw_key(&w, "path");
		jw_str(&w, files[i].path);
		if (!create) {
			jw_key(&w, "sha");
			jw_str(&w, files[i].blob_sha);
		}
		jw_key(&w, "content");
		jw_str(&w, b64);
		jw_obj_close(&w);
		free(b64);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	out = malloc(w.len + 1);
	if (out != NULL) {
		memcpy(out, w.buf, w.len);
		out[w.len] = '\0';
	}
	jw_free(&w);
	return out;
}

int forge_get_file(const struct forge_target *t, const char *path, const char *scratch_dir,
                   char *blob_sha, size_t blob_sha_size, char **content, size_t *content_len,
                   long *http_status, char *err, size_t err_size)
{
	char suffix[PATH_MAX + 256], url[2048], auth[512], resp_path[PATH_MAX], cerr[256];
	struct curlfetch_opts opts;
	struct json_value *root;
	const char *sha, *b64;
	char *resp = NULL, *clean, *decoded;
	size_t resp_len = 0, i, n = 0;
	long status = 0;
	int rc, dn;

	blob_sha[0] = '\0';
	*content = NULL;
	*content_len = 0;
	err[0] = '\0';
	*http_status = 0;
	if ((size_t)snprintf(suffix, sizeof(suffix), "/%s?ref=%s", path,
	                     t != NULL && t->branch != NULL ? t->branch : "") >= sizeof(suffix)) {
		snprintf(err, err_size, "the path %s does not fit", path);
		return -1;
	}
	if (forge_api(t, suffix, url, sizeof(url), auth, sizeof(auth), err, err_size) != 0)
		return -1;
	if ((size_t)snprintf(resp_path, sizeof(resp_path), "%s/forge-get.json", scratch_dir) >=
	    sizeof(resp_path)) {
		snprintf(err, err_size, "the scratch path does not fit");
		return -1;
	}
	memset(&opts, 0, sizeof(opts));
	opts.url = url;
	opts.path = resp_path;
	opts.header1 = auth;
	opts.connect_timeout = 15;
	opts.max_time = 60;
	cerr[0] = '\0';
	rc = curlfetch_perform(&opts, &status, cerr, sizeof(cerr));
	*http_status = status;
	if (status == 404)
		return 1;
	if (rc != 0 || status != 200) {
		snprintf(err, err_size, "reading %s from %s failed (HTTP %ld): %s", path, t->host, status,
		         cerr);
		return -1;
	}
	if (persist_read_file(resp_path, &resp, &resp_len) != 0 || resp == NULL) {
		snprintf(err, err_size, "reading %s from %s returned nothing", path, t->host);
		return -1;
	}
	root = json_parse(resp, resp_len);
	free(resp);
	sha = root != NULL ? json_as_string(json_object_get(root, "sha")) : NULL;
	b64 = root != NULL ? json_as_string(json_object_get(root, "content")) : NULL;
	if (sha == NULL || b64 == NULL || strlen(sha) >= blob_sha_size) {
		json_free(root);
		snprintf(err, err_size, "%s answered for %s without a blob sha and content", t->host,
		         path);
		return -1;
	}
	snprintf(blob_sha, blob_sha_size, "%s", sha);
	/* Gitea sends one line of base64 (measured, 2026-10-02); GitHub
	 * wraps it. The decoder takes neither newlines nor a NUL inside. */
	clean = malloc(strlen(b64) + 1);
	decoded = malloc(strlen(b64) + 1);
	if (clean == NULL || decoded == NULL) {
		free(clean);
		free(decoded);
		json_free(root);
		snprintf(err, err_size, "out of memory");
		return -1;
	}
	for (i = 0; b64[i] != '\0'; i++)
		if (b64[i] != '\n' && b64[i] != '\r')
			clean[n++] = b64[i];
	clean[n] = '\0';
	json_free(root);
	dn = base64_decode(clean, (unsigned char *)decoded, n + 1);
	free(clean);
	if (dn < 0) {
		free(decoded);
		snprintf(err, err_size, "%s sent %s with content that is not base64", t->host, path);
		return -1;
	}
	decoded[dn] = '\0';
	*content = decoded;
	*content_len = (size_t)dn;
	return 0;
}

int forge_update_files(const struct forge_target *t, const struct forge_file_update *files,
                       int count, const char *message, const char *scratch_dir, char *sha,
                       size_t sha_size, long *http_status, char *err, size_t err_size)
{
	char url[2048], auth[512], body_path[PATH_MAX], resp_path[PATH_MAX], cerr[256], said[256];
	struct curlfetch_opts opts;
	char *body, *resp = NULL;
	size_t resp_len = 0;
	long status = 0;
	int rc;

	sha[0] = '\0';
	*http_status = 0;
	err[0] = '\0';
	if (forge_api(t, "", url, sizeof(url), auth, sizeof(auth), err, err_size) != 0)
		return -1;
	if ((size_t)snprintf(body_path, sizeof(body_path), "%s/forge-update.json", scratch_dir) >=
	        sizeof(body_path) ||
	    (size_t)snprintf(resp_path, sizeof(resp_path), "%s/forge-update-response.json",
	                     scratch_dir) >= sizeof(resp_path)) {
		snprintf(err, err_size, "the scratch path does not fit");
		return -1;
	}
	body = forge_gitea_update_body(files, count, t->branch, message);
	if (body == NULL) {
		snprintf(err, err_size, "could not build the update request");
		return -1;
	}
	rc = persist_atomic_write(body_path, body, strlen(body));
	free(body);
	if (rc != 0) {
		snprintf(err, err_size, "could not write the update request to %s", body_path);
		return -1;
	}
	memset(&opts, 0, sizeof(opts));
	opts.url = url;
	opts.path = body_path;
	opts.upload = 1;
	opts.method = "POST";
	opts.response_path = resp_path;
	opts.header1 = auth;
	opts.header2 = "Content-Type: application/json";
	opts.connect_timeout = 15;
	opts.max_time = 120;
	cerr[0] = '\0';
	rc = curlfetch_perform(&opts, &status, cerr, sizeof(cerr));
	*http_status = status;
	if (rc != 0) {
		snprintf(err, err_size, "the update request to %s failed: %s", t->host, cerr);
		return -1;
	}
	if (status != 201) {
		forge_message(resp_path, said, sizeof(said));
		snprintf(err, err_size, "%s refused the update of %d file(s) with HTTP %ld%s%s", t->host,
		         count, status, said[0] != '\0' ? ": " : "", said);
		return -1;
	}
	if (persist_read_file(resp_path, &resp, &resp_len) != 0 ||
	    forge_gitea_commit_sha(resp, resp_len, sha, sha_size) != 0) {
		free(resp);
		snprintf(err, err_size, "%s answered 201 to the update but named no commit", t->host);
		return -1;
	}
	free(resp);
	return 0;
}
