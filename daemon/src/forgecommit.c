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
