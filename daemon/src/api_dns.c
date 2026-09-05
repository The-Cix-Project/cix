#include "api_dns.h"

#include "apiresp.h"
#include "apiroute.h"
#include "daemonpaths.h"
#include "http.h"
#include "json.h"
#include "siteconfig.h"
#include "dns.h"
#include "registry.h"
#include "namecheck.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static void respond_dns_error(int fd, enum dns_error err)
{
	switch (err) {
	case DNS_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid DNS record name");
		break;
	case DNS_ERR_INVALID_IP:
		respond_error(fd, 400, "Bad Request", "invalid ip");
		break;
	case DNS_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "a record with this name already exists");
		break;
	case DNS_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "DNS record table full");
		break;
	case DNS_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no such DNS record");
		break;
	case DNS_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "DNS record operation failed");
		break;
	}
}

void handle_dns_record_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *name, *ip;
	char qualified_name[DNS_NAME_MAX];
	struct in_addr addr;
	struct dns_record *rec;
	enum dns_error derr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	name = json_as_string(json_object_get(root, "name"));
	ip = json_as_string(json_object_get(root, "ip"));

	if (name == NULL || ip == NULL || inet_pton(AF_INET, ip, &addr) != 1) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "name/ip missing or invalid");
		return;
	}

	/* ADR-0052: a bare label (no dot) gets this site's default suffix
	 * appended server-side -- a name that already contains a dot is
	 * left exactly as typed, no daemon-side override, ever. */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));

	derr = dns_record_create(qualified_name, addr.s_addr, NULL, &rec);
	json_free(root);

	if (derr != DNS_OK) {
		respond_dns_error(fd, derr);
		return;
	}

	jw_init(&w);
	dns_write_json_one(rec, &w);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_dns_record_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "records");
	dns_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_dns_record_get_one(int fd, const char *name)
{
	char qualified_name[DNS_NAME_MAX];
	struct dns_record *rec;
	struct json_writer w;

	/* task #760: dns_record_find() matches the record's own stored,
	 * already-qualified name exactly (a plain strcmp() -- see dns.c) --
	 * handle_dns_record_create() has always qualified a bare label
	 * before storing one (ADR-0052), but this GET (and, until this
	 * fix, PUT/DELETE below) never did the same on the read side,
	 * so a record created with a bare --name=foo could only ever be
	 * looked back up by its full site-qualified FQDN, not the same
	 * bare name a caller just used to create it. Found live during
	 * the task #760 sweep. siteconfig_qualify() is a safe no-op on an
	 * already-qualified name (any label containing a '.' passes
	 * through unchanged), so this is correct for both a bare label
	 * and an already-fully-qualified name. */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));
	rec = dns_record_find(qualified_name);

	if (rec == NULL) {
		respond_error(fd, 404, "Not Found", "no such DNS record");
		return;
	}
	jw_init(&w);
	dns_write_json_one(rec, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_dns_record_update(int fd, const char *name, const char *body, size_t body_len)
{
	char qualified_name[DNS_NAME_MAX];
	struct json_value *root;
	const char *ip;
	struct in_addr addr;
	struct dns_record *rec;
	enum dns_error derr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	ip = json_as_string(json_object_get(root, "ip"));
	if (ip == NULL || inet_pton(AF_INET, ip, &addr) != 1) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "ip missing or invalid");
		return;
	}
	json_free(root);

	/* task #760: see handle_dns_record_get_one()'s own comment above --
	 * same qualify-before-lookup fix, so a record created with a bare
	 * --name= can be updated back with that same bare name. */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));
	derr = dns_record_update(qualified_name, addr.s_addr, &rec);
	if (derr != DNS_OK) {
		respond_dns_error(fd, derr);
		return;
	}

	jw_init(&w);
	dns_write_json_one(rec, &w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_dns_record_delete(int fd, const char *name)
{
	char qualified_name[DNS_NAME_MAX];
	enum dns_error derr;

	/* task #760: see handle_dns_record_get_one()'s own comment above --
	 * same qualify-before-lookup fix, so a record created with a bare
	 * --name= can be deleted back with that same bare name. */
	siteconfig_qualify(name, qualified_name, sizeof(qualified_name));
	derr = dns_record_delete(qualified_name);

	if (derr != DNS_OK) {
		respond_dns_error(fd, derr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

static void respond_dns_server_error(int fd, enum dns_server_error err)
{
	switch (err) {
	case DNS_SERVER_ERR_INVALID_PATH:
		respond_error(fd, 400, "Bad Request", "invalid hosts_path");
		break;
	case DNS_SERVER_ERR_DUPLICATE:
		respond_error(fd, 409, "Conflict", "this container is already registered");
		break;
	case DNS_SERVER_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "DNS server binding table full");
		break;
	case DNS_SERVER_ERR_WRITE_FAILED:
		respond_error(fd, 500, "Internal Server Error", "failed to write hosts file");
		break;
	case DNS_SERVER_ERR_NOT_FOUND:
	default:
		respond_error(fd, 404, "Not Found", "no such DNS server binding");
		break;
	}
}

void handle_dns_server_create(int fd, const char *body, size_t body_len)
{
	struct json_value *root;
	const char *container_name, *hosts_path;
	struct registry_entry *entry;
	enum dns_server_error serr;
	struct json_writer w;

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	container_name = json_as_string(json_object_get(root, "container"));
	hosts_path = json_as_string(json_object_get(root, "hosts_path"));

	if (container_name == NULL || hosts_path == NULL) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", "container/hosts_path missing");
		return;
	}

	entry = registry_find(container_name);
	if (entry == NULL || !entry->running) {
		json_free(root);
		respond_error(fd, 404, "Not Found", "no such running container");
		return;
	}

	serr = dns_server_register(container_name, entry->handle.pid, entry->handle.pidfd,
	                            hosts_path);

	if (serr != DNS_SERVER_OK) {
		json_free(root);
		respond_dns_server_error(fd, serr);
		return;
	}
	/*
	 * container_name/hosts_path still point into root -- build the
	 * response before freeing it, not after (freeing first and then
	 * reading through these pointers would be a use-after-free).
	 */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container_name);
	jw_key(&w, "hosts_path");
	jw_str(&w, hosts_path);
	jw_obj_close(&w);
	json_free(root);
	respond_json(fd, 201, "Created", &w);
	jw_free(&w);
}

void handle_dns_server_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "servers");
	dns_server_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_dns_server_delete(int fd, const char *name)
{
	enum dns_server_error serr = dns_server_unregister(name);

	if (serr != DNS_SERVER_OK) {
		respond_dns_server_error(fd, serr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}
