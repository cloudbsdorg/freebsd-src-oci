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

static size_t
write_callback(void *ptr, size_t size, size_t nmemb, void *stream)
{
	size_t realsize = size * nmemb;
	struct progress_data *p = (struct progress_data *)stream;

	if (p->cb != NULL) {
		p->cb(p->opaque, p->what, p->total, realsize);
	}

	return (realsize);
}

static size_t
header_callback(void *ptr, size_t size, size_t nmemb, void *stream)
{
	/* Capture Content-Length header */
	char *header = (char *)ptr;
	char *end = header + size * nmemb;
	*end = '\0';

	if (strncasecmp(header, "Content-Length:", 15) == 0) {
		char *p = header + 15;
		while (*p == ' ' || *p == '\t')
			p++;
		*(p + strlen(p) - 2) = '\0'; /* Remove \r\n */
		struct progress_data *pd = (struct progress_data *)stream;
		pd->total = strtoull(p, NULL, 10);
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

	reg->host = registry;
	reg->port = REGISTRY_DEFAULT_PORT;
	reg->path_prefix = strdup(REGISTRY_API_PREFIX);
	reg->tls = true;
	reg->auth = calloc(1, sizeof(struct registry_auth));
	if (reg->auth == NULL)
		return (-1);

	reg->auth->type = AUTH_ANONYMOUS;
	reg->auth->registry = strdup(registry);

	free(repo);
	free(tag);
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

	len = strlen(path) + 16; /* https://host:port */
	url = malloc(len);
	if (url == NULL)
		return (NULL);

	snprintf(url, len, "https://%s:%d%s", reg->host, reg->port, path);
	return (url);
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

	if (out != NULL) {
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
	}

	/* Authentication */
	if (reg->auth->type == AUTH_BASIC) {
		char *userpwd;
		asprintf(&userpwd, "%s:%s", reg->auth->username,
		    reg->auth->password);
		curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
		free(userpwd);
	} else if (reg->auth->type == AUTH_BEARER) {
		char header[512];
		snprintf(header, sizeof(header),
		    "Authorization: Bearer %s", reg->auth->password);
		struct curl_slist *list = curl_slist_append(NULL, header);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
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
	char *buffer = NULL;
	size_t buflen = 0;
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
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_only);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &buflen);

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

	*data = buffer;
	*len = buflen;
	ret = 0;
	buffer = NULL;

cleanup:
	free(url);
	free(buffer);
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
 * Authenticate with registry
 */
int
authenticate(struct registry *reg, const char *scope)
{
	char *url;
	char *response = NULL;
	char *auth_header = NULL;
	char realm[256];
	char *p, *end;
	int ret = -1;

	/* Build token URL */
	if (reg->auth->type == AUTH_ANONYMOUS) {
		/* Check if we need to authenticate */
		url = build_url(reg, "/v2/");
		if (url == NULL)
			return (-1);

		/* Try anonymous access first */
		CURL *curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &auth_header);
		curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		free(url);

		if (auth_header != NULL &&
		    strstr(auth_header, "Www-Authenticate") != NULL) {
			/* Parse auth header */
			p = strchr(auth_header, '"');
			if (p) {
				end = strchr(p + 1, '"');
				if (end) {
					*end = '\0';
					strlcpy(realm, p + 1, sizeof(realm));
				}
			}
		}

		if (realm[0] == '\0') {
			/* Anonymous access works */
			return (0);
		}
	} else {
		/* Basic auth */
		reg->auth->type = AUTH_BASIC;
		return (0);
	}

	/* Build token request URL */
	url = malloc(1024);
	if (url == NULL)
		return (-1);

	snprintf(url, 1024, "%s?service=%s&scope=%s", realm,
	    reg->auth->service ? reg->auth->service : reg->host,
	    scope ? scope : "");

	/* Fetch token */
	CURL *curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	if (reg->auth->type == AUTH_BASIC) {
		char *userpwd;
		asprintf(&userpwd, "%s:%s", reg->auth->username,
		    reg->auth->password);
		curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
		free(userpwd);
	}

	curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	free(url);

	/* Parse token response */
	if (response != NULL) {
		/* Simple JSON parsing for token */
		p = strstr(response, "\"token\":\"");
		if (p) {
			p += 8;
			end = strchr(p, '"');
			if (end) {
				*end = '\0';
				reg->auth->type = AUTH_BEARER;
				reg->auth->password = strdup(p);
				ret = 0;
			}
		}
		free(response);
	}

	return (ret);
}

/*
 * Fetch manifest for a repository:tag
 */
int
fetch_manifest(struct registry *reg, const char *repo, const char *tag,
    struct oci_manifest **manifest)
{
	char *url;
	char *path;
	int ret = -1;

	/* Build manifest path */
	if (asprintf(&path, "/v2/%s/manifests/%s", repo, tag) == -1)
		return (-1);

	url = build_url(reg, path);
	free(path);
	if (url == NULL)
		return (-1);

	/* Fetch manifest */
	CURL *curl = curl_easy_init();
	struct MemoryStruct chunk = { .memory = NULL, .size = 0 };

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

	/* Accept OCI and Docker manifests */
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers,
	    "Accept: application/vnd.oci.image.manifest.v1+json");
	headers = curl_slist_append(headers,
	    "Accept: application/vnd.docker.distribution.manifest.v2+json");
	headers = curl_slist_append(headers,
	    "Accept: application/vnd.docker.distribution.manifest.v1+json");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	CURLcode res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		long response_code;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code == 200) {
			ret = parse_manifest(chunk.memory, chunk.size, manifest);
		}
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(url);
	free(chunk.memory);

	return (ret);
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
		nlayers = json_object_array_length(layers_obj);
		m->nlayers = nlayers;
		m->layers = calloc(nlayers + 1, sizeof(struct oci_layer *));

		for (i = 0; i < nlayers; i++) {
			json_object *layer_obj, *digest, *size, *mtype;
			layer_obj = json_object_array_get_idx(layers_obj, i);

			m->layers[i] = calloc(1, sizeof(struct oci_layer));

			if (json_object_object_get_ex(layer_obj, "digest", &digest))
				m->layers[i]->digest = strdup(
				    json_object_get_string(digest));

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
	int ret = -1;

	if (asprintf(&path, "/v2/%s/blobs/%s", repo, digest) == -1)
		return (-1);

	url = build_url(reg, path);
	free(path);
	if (url == NULL)
		return (-1);

	CURL *curl = curl_easy_init();
	struct MemoryStruct chunk = { .memory = NULL, .size = 0 };

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

	CURLcode res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		long response_code;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code == 200) {
			ret = parse_config(chunk.memory, chunk.size, config);
		}
	}

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
		json_object *env, *workingDir, *user;

		if (json_object_object_get_ex(config_obj, "Env", &env)) {
			int i, n = json_object_array_length(env);
			cfg->env = calloc(n + 1, sizeof(char *));
			for (i = 0; i < n; i++)
				cfg->env[i] = strdup(
				    json_object_get_string(
					json_object_array_get_idx(env, i)));
		}

		if (json_object_object_get_ex(config_obj, "WorkingDir", &workingDir))
			cfg->workdir = strdup(json_object_get_string(workingDir));

		if (json_object_object_get_ex(config_obj, "User", &user))
			cfg->user = strdup(json_object_get_string(user));
	}

	json_object_put(obj);

	*config = cfg;
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
	struct progress_data pd = { cb, opaque, layer->digest, layer->size };
	int ret = -1;

	if (layer->url == NULL) {
		/* Need to get download URL from registry */
		if (asprintf(&path, "/v2/%s/blobs/%s", reg->host, layer->digest) == -1)
			return (-1);
		url = build_url(reg, path);
		free(path);
	} else {
		url = strdup(layer->url);
	}

	if (url == NULL)
		return (-1);

	/* Create output file */
	snprintf(filename, sizeof(filename), "%s/%s.layer",
	    destdir, layer->digest);
	out = fopen(filename, "wb");
	if (out == NULL) {
		free(url);
		return (-1);
	}

	CURL *curl = curl_easy_init();

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &pd);

	/* Set progress */
	if (cb != NULL) {
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	}

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
	}

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

	/* Parse reference */
	ret = parse_reference(reference, &registry, &repo, &tag, &digest);
	if (ret != 0) {
		fprintf(stderr, "error: invalid reference: %s\n", reference);
		return (-1);
	}

	/* Authenticate */
	if (authenticate(reg, repo) != 0) {
		fprintf(stderr, "error: authentication failed\n");
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

	/* Create destination directory */
	if (mkdir(destdir, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "error: cannot create directory: %s\n", destdir);
		goto cleanup;
	}

	/* Pull layers */
	for (i = 0; i < manifest->nlayers; i++) {
		char layer_dir[PATH_MAX];

		if (cb)
			cb(opaque, manifest->layers[i]->digest, 0, manifest->layers[i]->size);

		snprintf(layer_dir, sizeof(layer_dir), "%s/layers", destdir);
		if (mkdir(layer_dir, 0755) != 0 && errno != EEXIST) {
			fprintf(stderr, "error: cannot create layers directory\n");
			continue;
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
		fprintf(mf, "%s\n", manifest->raw);
		fclose(mf);
	}

	/* Save config */
	char config_path[PATH_MAX];
	snprintf(config_path, sizeof(config_path),
	    "%s/config.json", destdir);
	FILE *cfg = fopen(config_path, "w");
	if (cfg) {
		if (config)
			fprintf(cfg, "%s\n", config->config);
		fclose(cfg);
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
	(void)algo;

	free(actual_digest);
	return (ret);
}
