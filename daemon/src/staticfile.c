#include "staticfile.h"
#include "http.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *content_type_for(const char *path)
{
	size_t len = strlen(path);

	if (len >= 5 && strcmp(path + len - 5, ".html") == 0)
		return "text/html";
	if (len >= 3 && strcmp(path + len - 3, ".js") == 0)
		return "application/javascript";
	if (len >= 4 && strcmp(path + len - 4, ".css") == 0)
		return "text/css";
	if (len >= 4 && strcmp(path + len - 4, ".svg") == 0)
		return "image/svg+xml";
	return "application/octet-stream";
}

static void respond_plain(int fd, int status, const char *status_text, const char *msg)
{
	http_set_blocking(fd);
	http_write_response(fd, status, status_text, "text/plain", msg, strlen(msg));
}

void static_serve(int fd, const char *web_root, const char *req_path)
{
	char full_path[PATH_MAX];
	const char *rel = req_path;
	int file_fd;
	struct stat st;
	char *buf;
	size_t total;
	ssize_t n;

	if (strstr(req_path, "..") != NULL) {
		respond_plain(fd, 400, "Bad Request", "bad path\n");
		return;
	}

	if (strcmp(rel, "/") == 0)
		rel = "/index.html";

	if (snprintf(full_path, sizeof(full_path), "%s%s", web_root, rel) >= (int)sizeof(full_path)) {
		respond_plain(fd, 400, "Bad Request", "path too long\n");
		return;
	}

	file_fd = open(full_path, O_RDONLY);
	if (file_fd < 0) {
		respond_plain(fd, 404, "Not Found", "not found\n");
		return;
	}

	if (fstat(file_fd, &st) != 0) {
		close(file_fd);
		respond_plain(fd, 500, "Internal Server Error", "error\n");
		return;
	}

	buf = st.st_size > 0 ? malloc((size_t)st.st_size) : NULL;
	if (st.st_size > 0 && buf == NULL) {
		close(file_fd);
		respond_plain(fd, 500, "Internal Server Error", "error\n");
		return;
	}

	total = 0;
	while (total < (size_t)st.st_size) {
		n = read(file_fd, buf + total, (size_t)st.st_size - total);
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	close(file_fd);

	http_set_blocking(fd);
	http_write_response(fd, 200, "OK", content_type_for(full_path), buf, total);
	free(buf);
}
