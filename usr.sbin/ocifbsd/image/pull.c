/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by Klara, Inc. under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * $FreeBSD$
 *
 * OCI registry client for pulling images
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "pull.h"
#include "unpack.h"
#include "zfs_store.h"

/*
 * Registry defaults
 */
#define REGISTRY_DEFAULT_PORT	443
#define REGISTRY_DEFAULT_HTTP_PORT	80
#define REGISTRY_API_PREFIX	"/v2/"

/*
 * Progress state for download callbacks
 */
struct progress_data {
	progress_cb	cb;
	void		*opaque;
	const char	*what;
	off_t		total;
};

/*
 * Download state: write body to FILE and optionally report progress.
 */
struct download_state {
	FILE		*out;
	progress_cb	cb;
	void		*opaque;
	const char	*what;
	off_t		total;
	off_t		written;
};

static size_t
download_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct download_state *ds = userdata;
	size_t realsize = size * nmemb;
	size_t n;

	if (ds == NULL || ds->out == NULL)
		return (0);
	n = fwrite(ptr, 1, realsize, ds->out);
	if (n > 0) {
		ds->written += (off_t)n;
		if (ds->cb != NULL)
			ds->cb(ds->opaque, ds->what, ds->written, ds->total);
	}
	return (n);
}

/* Legacy name used by setup_curl for FILE* bodies */
static size_t
write_callback(void *ptr, size_t size, size_t nmemb, void *stream)
{
	return (fwrite(ptr, size, nmemb, (FILE *)stream));
}

static size_t
header_callback(void *ptr, size_t size, size_t nmemb, void *stream)
{
	/*
	 * Capture Content-Length without mutating curl's buffer (writing
	 * past the end is undefined). Copy into a local NUL-terminated line.
	 */
	size_t n = size * nmemb;
	char line[256];
	char *p;
	struct progress_data *pd = stream;

	if (pd == NULL || n == 0)
		return (n);
	if (n >= sizeof(line))
		n = sizeof(line) - 1;
	memcpy(line, ptr, n);
	line[n] = '\0';

	if (strncasecmp(line, "Content-Length:", 15) == 0) {
		p = line + 15;
		while (*p == ' ' || *p == '\t')
			p++;
		/* strip trailing CR/LF */
		while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) {
			line[n - 1] = '\0';
			n--;
		}
		pd->total = (off_t)strtoull(p, NULL, 10);
	}

	return (size * nmemb);
}

static size_t
header_only(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	(void)ptr;
	(void)userdata;
	return (size * nmemb);
}

struct MemoryStruct {
	char	*memory;
	size_t	 size;
};

static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct MemoryStruct *mem = (struct MemoryStruct *)userp;
	char *ptr;

	ptr = realloc(mem->memory, mem->size + realsize + 1);
	if (ptr == NULL)
		return (0);

	mem->memory = ptr;
	memcpy(&(mem->memory[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;

	return (realsize);
}

/*
 * Validate an OCI content digest of the form "sha256:<64 lowercase hex>"
 * (also accepts sha512:<128 hex>). Digests from a manifest are used to
 * build on-disk layer filenames and unpack paths, so a malformed value
 * ("../../etc/x") must be rejected before it can escape the store.
 */
bool
digest_is_valid(const char *digest)
{
	const char *hex;
	size_t want, i;

	if (digest == NULL)
		return (false);
	if (strncmp(digest, "sha256:", 7) == 0) {
		hex = digest + 7;
		want = 64;
	} else if (strncmp(digest, "sha512:", 7) == 0) {
		hex = digest + 7;
		want = 128;
	} else {
		return (false);
	}
	for (i = 0; i < want; i++) {
		char c = hex[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return (false);
	}
	return (hex[want] == '\0');
}

/*
 * Initialize registry structure from reference
 */
int
registry_init(struct registry *reg, const char *reference)
{
	char *registry, *repo, *tag, *digest;
	int ret;

	memset(reg, 0, sizeof(*reg));

	ret = parse_reference(reference, &registry, &repo, &tag, &digest);
	if (ret != 0) {
		fprintf(stderr, "error: invalid reference: %s\n", reference);
		return (-1);
	}

	/*
	 * Docker Hub API host is registry-1.docker.io; token service is
	 * registry.docker.io. References still use docker.io as the name.
	 */
	if (strcmp(registry, "docker.io") == 0 ||
	    strcmp(registry, "index.docker.io") == 0) {
		free(registry);
		registry = strdup("registry-1.docker.io");
	}

	reg->host = registry;
	reg->port = REGISTRY_DEFAULT_PORT;
	reg->path_prefix = strdup(REGISTRY_API_PREFIX);
	reg->tls = true;
	reg->repository = repo;
	reg->tag = tag != NULL ? tag : strdup("latest");
	reg->auth = calloc(1, sizeof(struct registry_auth));
	if (reg->auth == NULL)
		return (-1);

	reg->auth->type = AUTH_ANONYMOUS;
	reg->auth->registry = strdup(registry);
	if (strcmp(registry, "registry-1.docker.io") == 0)
		reg->auth->service = strdup("registry.docker.io");

	free(digest);

	return (0);
}

void
registry_free(struct registry *reg)
{
	if (reg == NULL)
		return;

	free(reg->host);
	free(reg->path_prefix);
	free(reg->repository);
	free(reg->tag);
	if (reg->auth) {
		free(reg->auth->username);
		free(reg->auth->password);
		free(reg->auth->registry);
		free(reg->auth->service);
		free(reg->auth);
	}
}

/*
 * parse_reference / canonicalize_reference live in reference.c
 */

/*
 * Build registry URL
 */
static char *
build_url(struct registry *reg, const char *path)
{
	char *url;
	size_t len;
	const char *scheme;
	char hostbuf[512];
	const char *host;

	if (reg == NULL || reg->host == NULL || path == NULL)
		return (NULL);

	scheme = reg->tls ? "https" : "http";
	host = reg->host;
	/*
	 * Host may already include :port (e.g. localhost:5000 from
	 * parse_reference). Avoid doubling the port.
	 */
	if (strchr(host, ':') != NULL) {
		len = strlen(scheme) + 3 + strlen(host) + strlen(path) + 1;
		url = malloc(len);
		if (url == NULL)
			return (NULL);
		snprintf(url, len, "%s://%s%s", scheme, host, path);
		return (url);
	}

	len = strlen(scheme) + 3 + strlen(host) + 1 + 8 + strlen(path) + 1;
	url = malloc(len);
	if (url == NULL)
		return (NULL);
	if ((reg->tls && reg->port == 443) ||
	    (!reg->tls && reg->port == 80))
		snprintf(url, len, "%s://%s%s", scheme, host, path);
	else
		snprintf(url, len, "%s://%s:%d%s", scheme, host, reg->port,
		    path);
	(void)hostbuf;
	return (url);
}

/*
 * Ensure directory exists including parents (mkdir -p).
 */
static int
mkdirp_path(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	char *p;
	size_t len;

	if (path == NULL || *path == '\0')
		return (-1);
	len = strlcpy(buf, path, sizeof(buf));
	if (len >= sizeof(buf))
		return (-1);
	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, mode) != 0 && errno != EEXIST)
			return (-1);
		*p = '/';
	}
	if (mkdir(buf, mode) != 0 && errno != EEXIST)
		return (-1);
	return (0);
}

/*
 * Make HTTP request with authentication
 */
static CURL *
setup_curl(struct registry *reg, const char *url, FILE *out)
{
	CURL *curl;

	curl = curl_easy_init();
	if (curl == NULL)
		return (NULL);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

	if (out != NULL) {
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
	}

	/* Authentication */
	if (reg->auth->type == AUTH_BASIC) {
		char *userpwd = NULL;
		if (asprintf(&userpwd, "%s:%s", reg->auth->username,
		    reg->auth->password) >= 0) {
			curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
			curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
			free(userpwd);
		}
	} else if (reg->auth->type == AUTH_BEARER) {
		char *header = NULL;
		struct curl_slist *list;

		if (asprintf(&header, "Authorization: Bearer %s",
		    reg->auth->password) >= 0) {
			list = curl_slist_append(NULL, header);
			free(header);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
			/* note: header list is not freed here (setup_curl caller) */
		}
	}

	return (curl);
}

/*
 * Fetch raw data from registry
 */
static int
fetch_data(struct registry *reg, const char *path, char **data, size_t *len)
{
	CURL *curl;
	CURLcode res;
	char *url;
	struct MemoryStruct chunk = { NULL, 0 };
	long response_code;
	struct curl_slist *headers = NULL;
	int ret = -1;

	url = build_url(reg, path);
	if (url == NULL)
		return (-1);

	curl = curl_easy_init();
	if (curl == NULL) {
		free(url);
		return (-1);
	}

	/* Add accept header */
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "Accept: application/vnd.oci.image.manifest.v1+json");
	headers = curl_slist_append(headers, "Accept: application/vnd.docker.distribution.manifest.v2+json");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	/*
	 * Accumulate the body into a growable memory buffer. The previous
	 * version passed fwrite as the write callback with a char** as the
	 * "FILE*", which is undefined behavior and never captured the body.
	 */
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

	res = curl_easy_perform(curl);

	if (res != CURLE_OK) {
		fprintf(stderr, "error: curl failed: %s\n",
		    curl_easy_strerror(res));
		goto cleanup;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 200) {
		fprintf(stderr, "error: HTTP %ld for %s\n", response_code, path);
		goto cleanup;
	}

	*data = chunk.memory;
	*len = chunk.size;
	ret = 0;
	chunk.memory = NULL;

cleanup:
	free(url);
	free(chunk.memory);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	return (ret);
}

/*
 * Check registry API version
 */
static int
registry_check_api(struct registry *reg)
{
	CURL *curl;
	CURLcode res;
	char *url;
	long response_code;
	int ret = -1;

	url = build_url(reg, "/v2/");
	if (url == NULL)
		return (-1);

	curl = curl_easy_init();
	if (curl == NULL) {
		free(url);
		return (-1);
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);

	res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code == 200) {
			ret = 0;
		}
	}

	free(url);
	curl_easy_cleanup(curl);

	return (ret);
}

/*
 * Capture WWW-Authenticate header into a growable string.
 */
static size_t
www_auth_header_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t n = size * nmemb;
	char **acc = userdata;
	char *line, *np;
	size_t old;

	line = malloc(n + 1);
	if (line == NULL)
		return (0);
	memcpy(line, ptr, n);
	line[n] = '\0';

	if (strncasecmp(line, "WWW-Authenticate:", 17) == 0 ||
	    strncasecmp(line, "Www-Authenticate:", 17) == 0) {
		old = (*acc != NULL) ? strlen(*acc) : 0;
		np = realloc(*acc, old + n + 1);
		if (np == NULL) {
			free(line);
			return (0);
		}
		*acc = np;
		memcpy(*acc + old, line, n + 1);
	}
	free(line);
	return (n);
}

static void
parse_www_authenticate(const char *hdr, char *realm, size_t realm_sz,
    char *service, size_t service_sz)
{
	const char *p, *end;
	size_t n;

	realm[0] = '\0';
	service[0] = '\0';
	if (hdr == NULL)
		return;

	p = strstr(hdr, "realm=\"");
	if (p != NULL) {
		p += 7;
		end = strchr(p, '"');
		if (end != NULL) {
			n = (size_t)(end - p);
			if (n >= realm_sz)
				n = realm_sz - 1;
			memcpy(realm, p, n);
			realm[n] = '\0';
		}
	}
	p = strstr(hdr, "service=\"");
	if (p != NULL) {
		p += 9;
		end = strchr(p, '"');
		if (end != NULL) {
			n = (size_t)(end - p);
			if (n >= service_sz)
				n = service_sz - 1;
			memcpy(service, p, n);
			service[n] = '\0';
		}
	}
}

static struct curl_slist *
auth_headers(struct registry *reg, struct curl_slist *headers)
{
	char *hdr = NULL;

	if (reg->auth != NULL && reg->auth->type == AUTH_BEARER &&
	    reg->auth->password != NULL) {
		/*
		 * Docker Hub JWTs are often >2KB. A fixed 1KB buffer
		 * truncates the token and yields HTTP 401 on manifests.
		 */
		if (asprintf(&hdr, "Authorization: Bearer %s",
		    reg->auth->password) < 0)
			return (headers);
		headers = curl_slist_append(headers, hdr);
		free(hdr);
	}
	return (headers);
}

/*
 * Authenticate with registry (Docker Hub token flow supported).
 * scope is the repository name (e.g. library/hello-world); we build
 * repository:<name>:pull for the token request.
 */
int
authenticate(struct registry *reg, const char *scope)
{
	char *url = NULL;
	char *www = NULL;
	char realm[512];
	char service[256];
	char scope_q[512];
	struct MemoryStruct chunk = { 0 };
	long response_code = 0;
	CURLcode res;
	CURL *curl;
	char *p, *end;
	int ret = -1;

	if (reg == NULL || reg->auth == NULL)
		return (-1);

	if (reg->auth->type == AUTH_BASIC)
		return (0);

	/* Already have a bearer token */
	if (reg->auth->type == AUTH_BEARER && reg->auth->password != NULL)
		return (0);

	url = build_url(reg, "/v2/");
	if (url == NULL)
		return (-1);

	curl = curl_easy_init();
	if (curl == NULL) {
		free(url);
		return (-1);
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, www_auth_header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &www);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
	res = curl_easy_perform(curl);
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
	curl_easy_cleanup(curl);
	free(url);
	free(chunk.memory);
	chunk.memory = NULL;
	chunk.size = 0;

	if (res != CURLE_OK) {
		fprintf(stderr, "error: cannot reach registry %s: %s\n",
		    reg->host, curl_easy_strerror(res));
		free(www);
		return (-1);
	}

	/* 200 with no auth challenge — anonymous OK */
	if (response_code == 200) {
		free(www);
		return (0);
	}

	parse_www_authenticate(www, realm, sizeof(realm), service,
	    sizeof(service));
	if (realm[0] == '\0' && www != NULL) {
		/*
		 * Some curl builds deliver the challenge only as a bare
		 * Bearer line; also try the raw header blob.
		 */
		parse_www_authenticate(www, realm, sizeof(realm), service,
		    sizeof(service));
	}
	if (realm[0] == '\0' && response_code == 401) {
		/*
		 * Docker Hub always challenges on /v2/. Fall back to the
		 * well-known token endpoint when the header is missing.
		 */
		if (strcmp(reg->host, "registry-1.docker.io") == 0 ||
		    strcmp(reg->host, "registry.docker.io") == 0) {
			strlcpy(realm, "https://auth.docker.io/token",
			    sizeof(realm));
			if (service[0] == '\0')
				strlcpy(service, "registry.docker.io",
				    sizeof(service));
		}
	}
	free(www);

	if (realm[0] == '\0') {
		/* No realm; allow caller to try without token only if not 401 */
		if (response_code == 401) {
			fprintf(stderr,
			    "error: registry %s returned 401 without realm\n",
			    reg->host);
			return (-1);
		}
		return (0);
	}

	if (service[0] != '\0') {
		free(reg->auth->service);
		reg->auth->service = strdup(service);
	}

	/* scope: repository:NAME:pull */
	if (scope != NULL && scope[0] != '\0')
		snprintf(scope_q, sizeof(scope_q),
		    "repository:%s:pull", scope);
	else if (reg->repository != NULL)
		snprintf(scope_q, sizeof(scope_q),
		    "repository:%s:pull", reg->repository);
	else
		scope_q[0] = '\0';

	if (asprintf(&url, "%s?service=%s&scope=%s", realm,
	    reg->auth->service != NULL ? reg->auth->service : reg->host,
	    scope_q) < 0)
		return (-1);

	curl = curl_easy_init();
	if (curl == NULL) {
		free(url);
		return (-1);
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
	res = curl_easy_perform(curl);
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
	curl_easy_cleanup(curl);
	free(url);

	if (res != CURLE_OK || response_code != 200 || chunk.memory == NULL) {
		fprintf(stderr, "error: token request failed (HTTP %ld)\n",
		    response_code);
		free(chunk.memory);
		return (-1);
	}

	/* Prefer "token", fall back to "access_token" */
	p = strstr(chunk.memory, "\"token\":\"");
	if (p != NULL)
		p += 9;
	else {
		p = strstr(chunk.memory, "\"access_token\":\"");
		if (p != NULL)
			p += 16;
	}
	if (p != NULL) {
		end = strchr(p, '"');
		if (end != NULL) {
			*end = '\0';
			free(reg->auth->password);
			reg->auth->password = strdup(p);
			reg->auth->type = AUTH_BEARER;
			ret = 0;
		}
	}
	free(chunk.memory);
	if (ret != 0)
		fprintf(stderr, "error: no token in auth response\n");
	return (ret);
}

/*
 * True if JSON is a multi-arch index / manifest list (not an image manifest).
 */
static int
manifest_json_is_index(const char *json, const struct oci_manifest *m)
{
	if (m != NULL && m->media_type != NULL) {
		if (strstr(m->media_type, "manifest.list") != NULL ||
		    strstr(m->media_type, "image.index") != NULL)
			return (1);
	}
	if (m != NULL && m->nlayers == 0 && json != NULL &&
	    strstr(json, "\"manifests\"") != NULL)
		return (1);
	return (0);
}

/*
 * Pick a platform-specific manifest digest from an OCI index.
 * Prefer freebsd/amd64, then linux/amd64, then any amd64, then first entry.
 */
static int
select_platform_digest(const char *json, char *out, size_t outsz)
{
	json_object *obj, *manifests, *entry, *platform, *os_o, *arch_o;
	json_object *digest_o;
	int i, n, best_score = -1;
	const char *best = NULL;

	if (json == NULL || out == NULL || outsz == 0)
		return (-1);
	out[0] = '\0';

	obj = json_tokener_parse(json);
	if (obj == NULL)
		return (-1);
	if (!json_object_object_get_ex(obj, "manifests", &manifests) ||
	    !json_object_is_type(manifests, json_type_array)) {
		json_object_put(obj);
		return (-1);
	}

	n = json_object_array_length(manifests);
	for (i = 0; i < n; i++) {
		const char *os_s = "";
		const char *arch_s = "";
		int score = 1;

		entry = json_object_array_get_idx(manifests, i);
		if (entry == NULL)
			continue;
		if (json_object_object_get_ex(entry, "platform", &platform) &&
		    platform != NULL) {
			if (json_object_object_get_ex(platform, "os", &os_o))
				os_s = json_object_get_string(os_o);
			if (json_object_object_get_ex(platform, "architecture",
			    &arch_o))
				arch_s = json_object_get_string(arch_o);
		}
		if (os_s == NULL)
			os_s = "";
		if (arch_s == NULL)
			arch_s = "";

		if (strcmp(arch_s, "amd64") == 0 ||
		    strcmp(arch_s, "x86_64") == 0)
			score += 10;
		if (strcmp(os_s, "freebsd") == 0)
			score += 100;
		else if (strcmp(os_s, "linux") == 0)
			score += 50;

		if (score > best_score &&
		    json_object_object_get_ex(entry, "digest", &digest_o)) {
			best = json_object_get_string(digest_o);
			best_score = score;
		}
	}

	if (best == NULL || best[0] == '\0') {
		json_object_put(obj);
		return (-1);
	}
	strlcpy(out, best, outsz);
	json_object_put(obj);
	return (0);
}

/*
 * Fetch a single manifest document (no index resolution).
 */
static int
fetch_manifest_once(struct registry *reg, const char *repo, const char *ref,
    char **out_json, size_t *out_len)
{
	char *url;
	char *path;
	struct MemoryStruct chunk = { .memory = NULL, .size = 0 };
	struct curl_slist *headers = NULL;
	CURL *curl;
	CURLcode res;
	long response_code = 0;
	int ret = -1;

	if (asprintf(&path, "/v2/%s/manifests/%s", repo, ref) == -1)
		return (-1);
	url = build_url(reg, path);
	free(path);
	if (url == NULL)
		return (-1);

	curl = curl_easy_init();
	if (curl == NULL) {
		free(url);
		return (-1);
	}

	headers = curl_slist_append(headers,
	    "Accept: application/vnd.oci.image.manifest.v1+json");
	headers = curl_slist_append(headers,
	    "Accept: application/vnd.docker.distribution.manifest.v2+json");
	headers = curl_slist_append(headers,
	    "Accept: application/vnd.oci.image.index.v1+json");
	headers = curl_slist_append(headers,
	    "Accept: application/vnd.docker.distribution.manifest.list.v2+json");
	headers = auth_headers(reg, headers);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

	res = curl_easy_perform(curl);
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

	if (res == CURLE_OK && response_code == 200 && chunk.memory != NULL) {
		*out_json = chunk.memory;
		*out_len = chunk.size;
		chunk.memory = NULL;
		ret = 0;
	} else if (res != CURLE_OK) {
		fprintf(stderr, "error: fetch manifest: %s\n",
		    curl_easy_strerror(res));
	} else {
		fprintf(stderr,
		    "error: fetch manifest HTTP %ld for %s:%s\n",
		    response_code, repo, ref);
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(url);
	free(chunk.memory);
	return (ret);
}

/*
 * Fetch manifest for a repository:tag (resolves multi-arch indexes).
 */
int
fetch_manifest(struct registry *reg, const char *repo, const char *tag,
    struct oci_manifest **manifest)
{
	char *json = NULL;
	size_t len = 0;
	char plat_digest[128];
	int ret = -1;

	if (manifest == NULL)
		return (-1);
	*manifest = NULL;

	if (fetch_manifest_once(reg, repo, tag, &json, &len) != 0)
		return (-1);

	ret = parse_manifest(json, len, manifest);
	if (ret != 0) {
		free(json);
		return (-1);
	}

	/* Multi-arch index: pick platform and re-fetch image manifest */
	if (manifest_json_is_index(json, *manifest)) {
		if (select_platform_digest(json, plat_digest,
		    sizeof(plat_digest)) != 0) {
			fprintf(stderr,
			    "error: multi-arch index has no usable platform\n");
			free_manifest(*manifest);
			*manifest = NULL;
			free(json);
			return (-1);
		}
		fprintf(stderr, "resolving multi-arch index -> %s\n",
		    plat_digest);
		free_manifest(*manifest);
		*manifest = NULL;
		free(json);
		json = NULL;
		len = 0;

		if (fetch_manifest_once(reg, repo, plat_digest, &json,
		    &len) != 0)
			return (-1);
		ret = parse_manifest(json, len, manifest);
		if (ret != 0) {
			free(json);
			return (-1);
		}
		if (manifest_json_is_index(json, *manifest)) {
			fprintf(stderr,
			    "error: nested multi-arch index not supported\n");
			free_manifest(*manifest);
			*manifest = NULL;
			free(json);
			return (-1);
		}
	}

	if (*manifest != NULL)
		(*manifest)->raw = json;
	else
		free(json);

	return (0);
}

/*
 * Parse manifest JSON
 */
static int
parse_manifest_json(json_object *obj, struct oci_manifest **manifest)
{
	struct oci_manifest *m;
	json_object *schema_obj, *media_obj, *layers_obj, *config_obj;
	int nlayers, i;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return (-1);

	/* Get schemaVersion */
	if (json_object_object_get_ex(obj, "schemaVersion", &schema_obj)) {
		m->schema_version = strdup(
		    json_object_to_json_string(schema_obj));
	}

	/* Get mediaType */
	if (json_object_object_get_ex(obj, "mediaType", &media_obj)) {
		m->media_type = strdup(json_object_get_string(media_obj));
	}

	/* Get config descriptor */
	if (json_object_object_get_ex(obj, "config", &config_obj)) {
		m->config = calloc(1, sizeof(struct oci_config));
		if (m->config) {
			json_object *digest, *mtype;
			if (json_object_object_get_ex(config_obj, "digest", &digest))
				m->config->config = strdup(
				    json_object_get_string(digest));
			if (json_object_object_get_ex(config_obj, "mediaType", &mtype))
				/* Store media type */;
		}
	}

	/* Get layers */
	if (json_object_object_get_ex(obj, "layers", &layers_obj)) {
		/*
		 * Guard against a malformed manifest: "layers" must be an
		 * array. Without this check json_object_array_length() on a
		 * non-array returns 0 or misbehaves and later indexing derefs
		 * a NULL element.
		 */
		if (json_object_get_type(layers_obj) != json_type_array) {
			fprintf(stderr,
			    "error: manifest 'layers' is not an array\n");
			free_manifest(m);
			return (-1);
		}
		nlayers = json_object_array_length(layers_obj);
		m->nlayers = nlayers;
		m->layers = calloc(nlayers + 1, sizeof(struct oci_layer *));
		if (m->layers == NULL) {
			free_manifest(m);
			return (-1);
		}

		for (i = 0; i < nlayers; i++) {
			json_object *layer_obj, *digest, *size, *mtype;
			layer_obj = json_object_array_get_idx(layers_obj, i);

			m->layers[i] = calloc(1, sizeof(struct oci_layer));
			if (m->layers[i] == NULL) {
				free_manifest(m);
				return (-1);
			}
			if (layer_obj == NULL ||
			    json_object_get_type(layer_obj) != json_type_object)
				continue;

			if (json_object_object_get_ex(layer_obj, "digest", &digest)) {
				const char *ds = json_object_get_string(digest);
				/*
				 * Reject a digest that is not a well-formed
				 * sha256:<64-hex>. The digest is later used to
				 * build on-disk filenames and unpack paths; an
				 * unvalidated value (e.g. "../../etc/x") would
				 * escape the layer store.
				 */
				if (!digest_is_valid(ds)) {
					fprintf(stderr,
					    "error: invalid layer digest in "
					    "manifest: %s\n", ds ? ds : "(null)");
					free_manifest(m);
					return (-1);
				}
				m->layers[i]->digest = strdup(ds);
			}

			if (json_object_object_get_ex(layer_obj, "mediaType", &mtype))
				m->layers[i]->media_type = strdup(
				    json_object_get_string(mtype));

			if (json_object_object_get_ex(layer_obj, "size", &size))
				m->layers[i]->size = json_object_get_int64(size);
		}
	}

	*manifest = m;
	return (0);
}

int
parse_manifest(const char *json, size_t len, struct oci_manifest **manifest)
{
	json_tokener *tok;
	enum json_tokener_error err;
	json_object *obj;

	tok = json_tokener_new();
	if (tok == NULL)
		return (-1);

	obj = json_tokener_parse_ex(tok, json, len);
	err = json_tokener_get_error(tok);

	if (err != json_tokener_success) {
		fprintf(stderr, "error: JSON parse failed: %s\n",
		    json_tokener_error_desc(err));
		json_tokener_free(tok);
		return (-1);
	}

	json_tokener_free(tok);

	int ret = parse_manifest_json(obj, manifest);
	json_object_put(obj);

	return (ret);
}

void
free_manifest(struct oci_manifest *manifest)
{
	int i;

	if (manifest == NULL)
		return;

	free(manifest->schema_version);
	free(manifest->media_type);

	if (manifest->config) {
		free(manifest->config->config);
		free(manifest->config);
	}

	if (manifest->layers) {
		for (i = 0; i < manifest->nlayers; i++) {
			free(manifest->layers[i]->digest);
			free(manifest->layers[i]->media_type);
			free(manifest->layers[i]);
		}
		free(manifest->layers);
	}

	free(manifest->raw);
	free(manifest);
}

/*
 * Fetch image config blob
 */
int
fetch_config(struct registry *reg, const char *repo, const char *digest,
    struct oci_config **config)
{
	char *url;
	char *path;
	struct curl_slist *headers = NULL;
	int ret = -1;

	if (asprintf(&path, "/v2/%s/blobs/%s", repo, digest) == -1)
		return (-1);

	url = build_url(reg, path);
	free(path);
	if (url == NULL)
		return (-1);

	CURL *curl = curl_easy_init();
	struct MemoryStruct chunk = { .memory = NULL, .size = 0 };

	if (curl == NULL) {
		free(url);
		return (-1);
	}

	headers = auth_headers(reg, headers);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
	if (headers != NULL)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	CURLcode res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		long response_code;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code == 200 && chunk.memory != NULL) {
			ret = parse_config(chunk.memory, chunk.size, config);
		} else {
			fprintf(stderr,
			    "error: fetch config HTTP %ld for %s\n",
			    response_code, digest);
		}
	} else {
		fprintf(stderr, "error: fetch config: %s\n",
		    curl_easy_strerror(res));
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(url);
	free(chunk.memory);

	return (ret);
}

int
parse_config(const char *json, size_t len, struct oci_config **config)
{
	struct oci_config *cfg;
	json_tokener *tok;
	enum json_tokener_error err;
	json_object *obj;

	tok = json_tokener_new();
	if (tok == NULL)
		return (-1);

	obj = json_tokener_parse_ex(tok, json, len);
	err = json_tokener_get_error(tok);

	if (err != json_tokener_success) {
		json_tokener_free(tok);
		return (-1);
	}

	json_tokener_free(tok);

	cfg = calloc(1, sizeof(*cfg));
	if (cfg == NULL) {
		json_object_put(obj);
		return (-1);
	}

	cfg->config = strndup(json, len);
	cfg->config_size = len;

	/* Parse JSON fields */
	json_object *arch, *os, *created, *config_obj;
	if (json_object_object_get_ex(obj, "architecture", &arch))
		cfg->architecture = strdup(json_object_get_string(arch));
	if (json_object_object_get_ex(obj, "os", &os))
		cfg->os = strdup(json_object_get_string(os));
	if (json_object_object_get_ex(obj, "created", &created))
		cfg->created = strdup(json_object_get_string(created));

	/* Parse config section */
	if (json_object_object_get_ex(obj, "config", &config_obj)) {
		json_object *env, *workingDir, *user, *ep, *cmd;
		int i, nep = 0, ncmd = 0, n, ai;

		if (json_object_object_get_ex(config_obj, "Env", &env) &&
		    json_object_is_type(env, json_type_array)) {
			n = json_object_array_length(env);
			cfg->env = calloc((size_t)n + 1, sizeof(char *));
			if (cfg->env != NULL) {
				for (i = 0; i < n; i++)
					cfg->env[i] = strdup(
					    json_object_get_string(
						json_object_array_get_idx(
						    env, i)));
			}
		}

		if (json_object_object_get_ex(config_obj, "WorkingDir",
		    &workingDir))
			cfg->workdir = strdup(
			    json_object_get_string(workingDir));

		if (json_object_object_get_ex(config_obj, "User", &user))
			cfg->user = strdup(json_object_get_string(user));

		/*
		 * Entrypoint + Cmd → process.args for OCI runtime config.
		 * Order: entrypoint elements then cmd elements.
		 */
		if (json_object_object_get_ex(config_obj, "Entrypoint", &ep) &&
		    json_object_is_type(ep, json_type_array))
			nep = json_object_array_length(ep);
		if (json_object_object_get_ex(config_obj, "Cmd", &cmd) &&
		    json_object_is_type(cmd, json_type_array))
			ncmd = json_object_array_length(cmd);
		n = nep + ncmd;
		if (n > 0) {
			cfg->cmd = calloc((size_t)n + 1, sizeof(char *));
			if (cfg->cmd != NULL) {
				ai = 0;
				for (i = 0; i < nep; i++)
					cfg->cmd[ai++] = strdup(
					    json_object_get_string(
						json_object_array_get_idx(
						    ep, i)));
				for (i = 0; i < ncmd; i++)
					cfg->cmd[ai++] = strdup(
					    json_object_get_string(
						json_object_array_get_idx(
						    cmd, i)));
			}
		}
	}

	json_object_put(obj);

	*config = cfg;
	return (0);
}

/*
 * Write image-config.json (registry blob) and OCI runtime config.json
 * so the image store directory is a usable bundle for create/run.
 */
static int
write_runtime_config(const char *destdir, struct oci_config *cfg)
{
	char path[PATH_MAX];
	FILE *f;
	int i;

	if (destdir == NULL)
		return (-1);

	/* Preserve raw image config separately from runtime config */
	if (cfg != NULL && cfg->config != NULL) {
		snprintf(path, sizeof(path), "%s/image-config.json", destdir);
		f = fopen(path, "w");
		if (f != NULL) {
			fprintf(f, "%s\n", cfg->config);
			fclose(f);
		}
	}

	snprintf(path, sizeof(path), "%s/config.json", destdir);
	f = fopen(path, "w");
	if (f == NULL)
		return (-1);

	fprintf(f, "{\n");
	fprintf(f, "  \"ociVersion\": \"1.0.2\",\n");
	fprintf(f, "  \"hostname\": \"ocifbsd\",\n");
	fprintf(f, "  \"process\": {\n");
	fprintf(f, "    \"terminal\": false,\n");
	fprintf(f, "    \"user\": { \"uid\": 0, \"gid\": 0 },\n");
	fprintf(f, "    \"args\": [");
	if (cfg != NULL && cfg->cmd != NULL && cfg->cmd[0] != NULL) {
		for (i = 0; cfg->cmd[i] != NULL; i++) {
			if (i > 0)
				fprintf(f, ", ");
			/* Minimal JSON string escape for common paths */
			fprintf(f, "\"");
			{
				const char *p;
				for (p = cfg->cmd[i]; *p != '\0'; p++) {
					if (*p == '"' || *p == '\\')
						fputc('\\', f);
					fputc(*p, f);
				}
			}
			fprintf(f, "\"");
		}
	} else {
		fprintf(f, "\"/bin/sh\"");
	}
	fprintf(f, "],\n");
	fprintf(f, "    \"env\": [");
	if (cfg != NULL && cfg->env != NULL && cfg->env[0] != NULL) {
		for (i = 0; cfg->env[i] != NULL; i++) {
			if (i > 0)
				fprintf(f, ", ");
			fprintf(f, "\"");
			{
				const char *p;
				for (p = cfg->env[i]; *p != '\0'; p++) {
					if (*p == '"' || *p == '\\')
						fputc('\\', f);
					fputc(*p, f);
				}
			}
			fprintf(f, "\"");
		}
	} else {
		fprintf(f, "\"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\"");
	}
	fprintf(f, "],\n");
	fprintf(f, "    \"cwd\": \"%s\"\n",
	    (cfg != NULL && cfg->workdir != NULL && cfg->workdir[0] != '\0') ?
	    cfg->workdir : "/");
	fprintf(f, "  },\n");
	fprintf(f, "  \"root\": {\n");
	fprintf(f, "    \"path\": \"rootfs\",\n");
	fprintf(f, "    \"readonly\": false\n");
	fprintf(f, "  }\n");
	fprintf(f, "}\n");
	fclose(f);
	return (0);
}

void
free_config(struct oci_config *config)
{
	int i;

	if (config == NULL)
		return;

	free(config->architecture);
	free(config->os);
	free(config->config);
	free(config->created);
	free(config->author);
	free(config->entrypoint);

	if (config->cmd) {
		for (i = 0; config->cmd[i]; i++)
			free(config->cmd[i]);
		free(config->cmd);
	}

	if (config->env) {
		for (i = 0; config->env[i]; i++)
			free(config->env[i]);
		free(config->env);
	}

	free(config->workdir);
	free(config->user);
	free(config->exposed_ports);
	free(config);
}

/*
 * Pull a single layer
 */
int
registry_pull_layer(struct registry *reg, struct oci_layer *layer,
    const char *destdir, progress_cb cb, void *opaque)
{
	char *url;
	char *path;
	FILE *out;
	char filename[PATH_MAX];
	struct download_state ds;
	struct curl_slist *headers = NULL;
	int ret = -1;

	if (layer == NULL || layer->digest == NULL)
		return (-1);

	/*
	 * Never build a path or filename from an unvalidated digest. The
	 * manifest parser already checks this, but registry_pull_layer is a
	 * public entry point, so guard here too.
	 */
	if (!digest_is_valid(layer->digest)) {
		fprintf(stderr, "error: refusing layer with invalid digest: %s\n",
		    layer->digest ? layer->digest : "(null)");
		return (-1);
	}

	if (layer->url == NULL) {
		const char *repo = reg->repository != NULL ? reg->repository :
		    reg->host;

		/* OCI Distribution: /v2/<name>/blobs/<digest> */
		if (asprintf(&path, "/v2/%s/blobs/%s", repo, layer->digest) ==
		    -1)
			return (-1);
		url = build_url(reg, path);
		free(path);
	} else {
		url = strdup(layer->url);
	}

	if (url == NULL)
		return (-1);

	/* Create output file (digest may contain ':') */
	snprintf(filename, sizeof(filename), "%s/%s.layer",
	    destdir, layer->digest);
	out = fopen(filename, "wb");
	if (out == NULL) {
		free(url);
		return (-1);
	}

	memset(&ds, 0, sizeof(ds));
	ds.out = out;
	ds.cb = cb;
	ds.opaque = opaque;
	ds.what = layer->digest;
	ds.total = (off_t)layer->size;
	ds.written = 0;

	CURL *curl = curl_easy_init();
	if (curl == NULL) {
		fclose(out);
		free(url);
		return (-1);
	}

	headers = auth_headers(reg, headers);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ds);
	if (headers != NULL)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	CURLcode res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		long response_code;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code == 200) {
			ret = 0;
		} else {
			fprintf(stderr, "error: HTTP %ld for layer %s\n",
			    response_code, layer->digest);
		}
	} else {
		fprintf(stderr, "error: layer download: %s\n",
		    curl_easy_strerror(res));
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(url);
	fclose(out);

	if (ret == 0) {
		/* Verify layer digest */
		if (verify_layer(filename, layer->digest) != 0) {
			fprintf(stderr, "error: layer digest mismatch: %s\n",
			    layer->digest);
			unlink(filename);
			ret = -1;
		}
	} else {
		unlink(filename);
	}

	return (ret);
}

/*
 * Pull entire image
 */
int
registry_pull(struct registry *reg, const char *reference,
    const char *destdir, progress_cb cb, void *opaque)
{
	char *registry, *repo, *tag, *digest;
	struct oci_manifest *manifest = NULL;
	struct oci_config *config = NULL;
	int i, ret = -1;

	/* Parse reference (do not reuse ret for intermediate successes) */
	if (parse_reference(reference, &registry, &repo, &tag, &digest) != 0) {
		fprintf(stderr, "error: invalid reference: %s\n", reference);
		return (-1);
	}

	/* Prefer repository stored at registry_init when present */
	if (reg->repository != NULL) {
		free(repo);
		repo = strdup(reg->repository);
	}

	/* Authenticate */
	if (authenticate(reg, repo) != 0) {
		fprintf(stderr, "error: authentication failed\n");
		ret = -1;
		goto cleanup;
	}

	/* Fetch manifest */
	if (fetch_manifest(reg, repo, tag, &manifest) != 0) {
		fprintf(stderr, "error: failed to fetch manifest\n");
		goto cleanup;
	}

	/* Fetch config */
	if (manifest->config && manifest->config->config) {
		if (fetch_config(reg, repo, manifest->config->config, &config) != 0) {
			fprintf(stderr, "error: failed to fetch config\n");
			goto cleanup;
		}
	}

	/* Create destination directory (including parents) */
	if (mkdirp_path(destdir, 0755) != 0) {
		fprintf(stderr, "error: cannot create directory: %s: %s\n",
		    destdir, strerror(errno));
		goto cleanup;
	}

	/* Pull layers */
	for (i = 0; i < manifest->nlayers; i++) {
		char layer_dir[PATH_MAX];

		if (cb)
			cb(opaque, manifest->layers[i]->digest, 0,
			    manifest->layers[i]->size);

		snprintf(layer_dir, sizeof(layer_dir), "%s/layers", destdir);
		if (mkdirp_path(layer_dir, 0755) != 0) {
			fprintf(stderr,
			    "error: cannot create layers directory: %s\n",
			    layer_dir);
			ret = -1;
			goto cleanup;
		}

		ret = registry_pull_layer(reg, manifest->layers[i],
		    layer_dir, cb, opaque);
		if (ret != 0) {
			fprintf(stderr, "error: failed to pull layer %d\n", i);
			goto cleanup;
		}
	}

	/* Save manifest */
	char manifest_path[PATH_MAX];
	snprintf(manifest_path, sizeof(manifest_path),
	    "%s/manifest.json", destdir);
	FILE *mf = fopen(manifest_path, "w");
	if (mf) {
		if (manifest->raw != NULL)
			fprintf(mf, "%s\n", manifest->raw);
		fclose(mf);
	}

	/*
	 * Apply layers in order into destdir/rootfs (merged OCI rootfs).
	 * Failures are reported but do not fail the pull of blobs/manifest.
	 */
	if (manifest->nlayers > 0) {
		char rootfs[PATH_MAX];
		char layer_path[PATH_MAX];

		snprintf(rootfs, sizeof(rootfs), "%s/rootfs", destdir);
		if (mkdirp_path(rootfs, 0755) != 0) {
			fprintf(stderr,
			    "warning: cannot create rootfs %s: %s\n",
			    rootfs, strerror(errno));
		} else {
			for (i = 0; i < manifest->nlayers; i++) {
				snprintf(layer_path, sizeof(layer_path),
				    "%s/layers/%s.layer", destdir,
				    manifest->layers[i]->digest);
				if (unpack_layer(layer_path, rootfs,
				    NULL) != 0) {
					fprintf(stderr,
					    "warning: unpack layer %d failed\n",
					    i);
				}
			}
		}
	}

	/*
	 * Write image-config.json + OCI runtime config.json so destdir is a
	 * valid create/run bundle (root.path = rootfs).
	 */
	if (write_runtime_config(destdir, config) != 0) {
		fprintf(stderr,
		    "warning: failed to write runtime config under %s\n",
		    destdir);
	}

	ret = 0;

cleanup:
	free(registry);
	free(repo);
	free(tag);
	free(digest);
	free_manifest(manifest);
	free_config(config);

	return (ret);
}

/*
 * Compute SHA256 digest of a file
 */
int
compute_digest(const char *path, char **digest)
{
	SHA256_CTX ctx;
	unsigned char hash[SHA256_DIGEST_LENGTH];
	unsigned char buf[8192];
	char *hex;
	FILE *f;
	size_t n;

	f = fopen(path, "rb");
	if (f == NULL)
		return (-1);

	SHA256_Init(&ctx);

	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		SHA256_Update(&ctx, buf, n);
	}

	SHA256_Final(hash, &ctx);
	fclose(f);

	/* Convert to hex string */
	hex = malloc(SHA256_DIGEST_LENGTH * 2 + 8);
	if (hex == NULL)
		return (-1);

	for (n = 0; n < SHA256_DIGEST_LENGTH; n++)
		snprintf(hex + n * 2, 3, "%02x", hash[n]);

	*digest = hex;
	return (0);
}

/*
 * Verify layer digest
 */
int
verify_layer(const char *path, const char *expected_digest)
{
	char *actual_digest;
	int ret;

	const char *hex = strchr(expected_digest, ':');
	if (hex == NULL) {
		fprintf(stderr, "error: invalid digest format\n");
		return (-1);
	}
	hex++; /* Skip ':' */

	/* Compute actual digest */
	ret = compute_digest(path, &actual_digest);
	if (ret != 0)
		return (-1);

	/*
	 * compute_digest() returns raw hex without a "sha256:" prefix.
	 * expected is typically "sha256:<hex>".
	 */
	if (strncasecmp(expected_digest, "sha256:", 7) == 0 &&
	    strcasecmp(actual_digest, hex) == 0) {
		ret = 0;
	} else {
		fprintf(stderr, "error: digest mismatch\n");
		fprintf(stderr, "  expected: %s\n", expected_digest);
		fprintf(stderr, "  actual: sha256:%s\n", actual_digest);
		ret = -1;
	}

	free(actual_digest);
	return (ret);
}
