#include "curlfetch.h"

#include <curl/curl.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static size_t write_to_file(char *data, size_t size, size_t nmemb, void *stream)
{
	return fwrite(data, size, nmemb, (FILE *)stream);
}

static size_t read_from_file(char *buf, size_t size, size_t nmemb, void *stream)
{
	return fread(buf, size, nmemb, (FILE *)stream);
}

static long backoff_seconds(int attempt)
{
	long s = 1;
	int i;

	for (i = 1; i < attempt; i++) {
		s *= 2;
		if (s >= 30)
			return 30;
	}
	return s;
}

/*
 * Which failures are worth another attempt when the caller did not ask
 * for retry_all_errors -- curl(1)'s own curated set for plain --retry:
 * a connection that never established, a transfer that stalled or
 * timed out, and a 5xx/408/429 response (CURLOPT_FAILONERROR turns
 * those into CURLE_HTTP_RETURNED_ERROR, so the status has to be
 * inspected separately). Deliberately NOT including a bare connection
 * reset mid-transfer (CURLE_RECV_ERROR/CURLE_SEND_ERROR) here -- that
 * is exactly the gap retry_all_errors exists to cover (ADR-0056).
 */
static int is_curatedly_retryable(CURLcode code, long http_status)
{
	switch (code) {
	case CURLE_COULDNT_CONNECT:
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY:
	case CURLE_OPERATION_TIMEDOUT:
		return 1;
	case CURLE_HTTP_RETURNED_ERROR:
		return http_status == 408 || http_status == 429 ||
		       (http_status >= 500 && http_status < 600);
	default:
		return 0;
	}
}

/* One transfer attempt. resume_from is bytes already on disk (0 for a
 * fresh attempt); ignored for an upload. Returns the CURLcode and, for
 * an upload, the HTTP status via *out_http_status regardless of
 * success -- matching curl(1)'s --write-out "%{http_code}" always
 * reporting what the server said. */
static CURLcode perform_one(CURL *curl, const struct curlfetch_opts *opts, long resume_from,
                            long *out_http_status, char *curl_err)
{
	FILE *f;
	CURLcode rc;
	struct curl_slist *headers = NULL;

	curl_easy_setopt(curl, CURLOPT_URL, opts->url);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	/* 50, not left at the library default: libcurl's own default with
	 * FOLLOWLOCATION on is unlimited (-1), but curl(1) -- the subprocess
	 * every one of these call sites used to be -- has always applied
	 * its own compiled-in cap of 50 whenever -L is given without an
	 * explicit --max-redirs. None of the twelve call sites passed
	 * --max-redirs, so 50 is what they actually ran with; matching the
	 * library default here would be a real behavior change disguised
	 * as "no change". */
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
	/*
	 * libcurl sends NO User-Agent header unless told to -- unlike the
	 * curl(1) CLI tool every one of these call sites used to be, which
	 * always sends "curl/<version>". Confirmed live on 192.168.15.95,
	 * 2026-09-15: fetching a real GNU mirror URL without this returned
	 * a bare HTTP 403, where the same URL through the old curl
	 * subprocess always succeeded -- some servers (this one included)
	 * treat a missing User-Agent as a bot signal. Matching the CLI
	 * tool's own default exactly is the correct fix, not inventing a
	 * new identity: it is the identity every one of these fetches has
	 * presented since this project existed.
	 */
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/" LIBCURL_VERSION);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	if (opts->connect_timeout > 0)
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, opts->connect_timeout);
	if (opts->max_time > 0)
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, opts->max_time);
	if (opts->low_speed_limit > 0 && opts->low_speed_time > 0) {
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, opts->low_speed_limit);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, opts->low_speed_time);
	}
	if (opts->header1 != NULL)
		headers = curl_slist_append(headers, opts->header1);
	if (opts->header2 != NULL)
		headers = curl_slist_append(headers, opts->header2);
	if (headers != NULL)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	if (opts->upload) {
		struct stat st;

		f = fopen(opts->path, "rb");
		if (f == NULL) {
			snprintf(curl_err, CURL_ERROR_SIZE, "cannot open %s for upload", opts->path);
			curl_slist_free_all(headers);
			return CURLE_READ_ERROR;
		}
		if (fstat(fileno(f), &st) != 0) {
			fclose(f);
			snprintf(curl_err, CURL_ERROR_SIZE, "cannot stat %s", opts->path);
			curl_slist_free_all(headers);
			return CURLE_READ_ERROR;
		}
		curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_from_file);
		curl_easy_setopt(curl, CURLOPT_READDATA, f);
		curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)st.st_size);
	} else {
		curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
		f = fopen(opts->path, resume_from > 0 ? "ab" : "wb");
		if (f == NULL) {
			snprintf(curl_err, CURL_ERROR_SIZE, "cannot open %s", opts->path);
			curl_slist_free_all(headers);
			return CURLE_WRITE_ERROR;
		}
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
		if (resume_from > 0)
			curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resume_from);
	}

	rc = curl_easy_perform(curl);
	if (out_http_status != NULL) {
		long status = 0;

		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		*out_http_status = status;
	}
	fclose(f);
	curl_slist_free_all(headers);
	return rc;
}

int curlfetch_perform(const struct curlfetch_opts *opts, long *out_http_status, char *err,
                      size_t err_size)
{
	CURL *curl;
	CURLcode rc = CURLE_OK;
	char curl_err[CURL_ERROR_SIZE] = "";
	long http_status = 0;
	int attempt;
	long resume_from = 0;

	if (out_http_status != NULL)
		*out_http_status = 0;
	if (err != NULL && err_size > 0)
		err[0] = '\0';

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		if (err != NULL)
			snprintf(err, err_size, "curl_global_init failed");
		return -1;
	}
	curl = curl_easy_init();
	if (curl == NULL) {
		curl_global_cleanup();
		if (err != NULL)
			snprintf(err, err_size, "curl_easy_init failed");
		return -1;
	}

	for (attempt = 0; attempt <= opts->retry_count; attempt++) {
		if (attempt > 0) {
			long delay = opts->retry_delay > 0 ? opts->retry_delay : backoff_seconds(attempt);

			sleep((unsigned int)delay);
			if (opts->resume && !opts->upload) {
				struct stat st;

				resume_from = (stat(opts->path, &st) == 0) ? (long)st.st_size : 0;
			}
		}
		curl_err[0] = '\0';
		rc = perform_one(curl, opts, resume_from, &http_status, curl_err);
		if (rc == CURLE_OK)
			break;
		if (attempt >= opts->retry_count)
			break;
		if (!opts->retry_all_errors && !is_curatedly_retryable(rc, http_status))
			break;
	}

	curl_easy_cleanup(curl);
	curl_global_cleanup();

	if (out_http_status != NULL)
		*out_http_status = http_status;

	if (rc != CURLE_OK) {
		if (err != NULL)
			snprintf(err, err_size, "%s",
			         curl_err[0] != '\0' ? curl_err : curl_easy_strerror(rc));
		return -1;
	}
	return 0;
}
