/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
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
 * OCI image push to registry
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <json-c/json.h>
#include <netdb.h>
#include <sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zlib.h>

#include "push.h"
#include "pull.h"

/*
 * In-memory read source for libcurl uploads. Without an explicit
 * CURLOPT_READFUNCTION, libcurl's default reader calls fread() and treats
 * CURLOPT_READDATA as a FILE* — passing a raw buffer there is undefined
 * behavior. This callback feeds bytes from a fixed buffer instead.
 */
struct mem_reader {
	const char	*data;
	size_t		 len;
	size_t		 pos;
};

static size_t
mem_read_callback(char *ptr, size_t size, size_t nmemb, void *userp)
{
	struct mem_reader *r = userp;
	size_t want = size * nmemb;
	size_t avail = r->len - r->pos;

	if (want > avail)
		want = avail;
	if (want > 0) {
		memcpy(ptr, r->data + r->pos, want);
		r->pos += want;
	}
	return (want);
}

/*
 * Capture the Location header into sess->location. userdata is the
 * upload_session; we heap-copy the (non-NUL-terminated) header value into
 * a freshly allocated string rather than writing 1 KiB over a pointer.
 */
static size_t
header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
	size_t len = size * nitems;
	struct upload_session *sess = userdata;

	if (len >= 10 && strncasecmp(buffer, "Location:", 9) == 0) {
		const char *value = buffer + 9;
		const char *end = buffer + len;
		size_t vlen;
		char *copy;

		while (value < end && (*value == ' ' || *value == '\t'))
			value++;
		while (end > value && (end[-1] == '\n' || end[-1] == '\r'))
			end--;
		vlen = (size_t)(end - value);
		copy = malloc(vlen + 1);
		if (copy != NULL) {
			memcpy(copy, value, vlen);
			copy[vlen] = '\0';
			free(sess->location);
			sess->location = copy;
		}
	}

	return (len);
}

/*
 * Upload session management
 */
struct upload_session *
upload_start(struct registry *reg, const char *repo)
{
	struct upload_session *sess;
	char *url = NULL;
	long response_code;
	int ret = -1;

	sess = calloc(1, sizeof(*sess));
	if (sess == NULL)
		return (NULL);

	sess->host = strdup(reg->host);

	/* Build upload initiation URL */
	if (asprintf(&url, "https://%s:%d/v2/%s/blobs/uploads/",
	    reg->host, reg->port, repo) == -1) {
		free(sess);
		return (NULL);
	}

	CURL *curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, sess);

	CURLcode res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code == 202) {
			/* Upload session started */
			ret = 0;
		}
	}

	curl_easy_cleanup(curl);
	free(url);

	if (ret != 0) {
		free(sess);
		return (NULL);
	}

	return (sess);
}

int
upload_chunk(struct upload_session *sess, const char *data, size_t len)
{
	char *url;
	CURL *curl;
	CURLcode res;
	long response_code;
	int ret = -1;

	if (sess->location == NULL)
		return (-1);

	/* Append data to upload URL */
	url = strdup(sess->location);

	struct mem_reader reader = { data, len, 0 };

	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, mem_read_callback);
	curl_easy_setopt(curl, CURLOPT_READDATA, &reader);
	curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)len);

	res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code == 202 || response_code == 204) {
			ret = 0;
		}
	}

	curl_easy_cleanup(curl);
	free(url);

	return (ret);
}

int
upload_complete(struct upload_session *sess, const char *digest)
{
	char *location_with_digest;
	CURL *curl;
	CURLcode res;
	long response_code;
	int ret = -1;

	/* Build URL with digest parameter */
	if (asprintf(&location_with_digest, "%s&digest=%s", sess->location,
	    digest) == -1)
		return (-1);

	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, location_with_digest);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

	res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code == 201) {
			ret = 0;
		}
	}

	curl_easy_cleanup(curl);
	free(location_with_digest);

	return (ret);
}

void
upload_abort(struct upload_session *sess)
{
	char *url;
	CURL *curl;

	if (sess == NULL)
		return;

	if (sess->location) {
		url = strdup(sess->location);
		curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
		curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		free(url);
	}

	free(sess->location);
	free(sess->uuid);
	free(sess->host);
	free(sess);
}

/*
 * Push a single layer to registry
 */
int
push_layer(struct registry *reg, const char *layer_path,
    const char *digest, progress_cb cb, void *opaque)
{
	struct upload_session *sess = NULL;
	FILE *f;
	char buf[65536];
	size_t n;
	int ret = -1;

	/* Open layer file */
	f = fopen(layer_path, "rb");
	if (f == NULL) {
		fprintf(stderr, "error: cannot open layer: %s\n", layer_path);
		return (-1);
	}

	/* Start upload session against the reference's repository. */
	const char *repo = (reg->repository != NULL) ? reg->repository :
	    "library/_uploads";
	sess = upload_start(reg, repo);
	if (sess == NULL) {
		fprintf(stderr, "error: failed to start upload session\n");
		fclose(f);
		return (-1);
	}

	/* Upload layer data in chunks */
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (upload_chunk(sess, buf, n) != 0) {
			fprintf(stderr, "error: upload chunk failed\n");
			goto cleanup;
		}
		if (cb)
			cb(opaque, digest, ftello(f), 0);
	}

	/* Complete upload with digest */
	ret = upload_complete(sess, digest);
	if (ret != 0) {
		fprintf(stderr, "error: failed to complete upload\n");
	}

cleanup:
	upload_abort(sess);
	fclose(f);

	return (ret);
}

/*
 * Compute digest of a file
 */
static int
compute_file_digest(const char *path, char **digest)
{
	SHA256_CTX ctx;
	unsigned char hash[SHA256_DIGEST_LENGTH];
	unsigned char buf[65536];
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

	hex = malloc(SHA256_DIGEST_LENGTH * 2 + 8);
	if (hex == NULL)
		return (-1);

	snprintf(hex, SHA256_DIGEST_LENGTH * 2 + 8, "sha256:");
	for (n = 0; n < SHA256_DIGEST_LENGTH; n++)
		sprintf(hex + 7 + n * 2, "%02x", hash[n]);

	*digest = hex;
	return (0);
}

/*
 * Create tar archive from directory
 */
static int
create_tar_from_directory(const char *srcdir, const char *destfile)
{
	struct archive *a;
	struct archive_entry *entry;
	FTS *fts;
	FTSENT *ent;
	char *paths[2];
	char path[PATH_MAX];
	int ret = 0;

	a = archive_write_new();
	if (a == NULL)
		return (-1);

	archive_write_add_filter_gzip(a);
	archive_write_set_format_pax_restricted(a);
	archive_write_open_filename(a, destfile);

	paths[0] = (char *)srcdir;
	paths[1] = NULL;

	fts = fts_open(paths, FTS_PHYSICAL, NULL);
	if (fts == NULL) {
		archive_write_free(a);
		return (-1);
	}

	while ((ent = fts_read(fts)) != NULL) {
		/* Skip root directory */
		if (ent->fts_level == 0)
			continue;

		snprintf(path, sizeof(path), "%s", ent->fts_path + strlen(srcdir) + 1);

		if (ent->fts_info == FTS_F) {
			entry = archive_entry_new();
			archive_entry_set_pathname(entry, path);
			archive_entry_set_size(entry, ent->fts_statp->st_size);
			archive_entry_set_filetype(entry, AE_IFREG);
			archive_entry_set_mode(entry, ent->fts_statp->st_mode);
			archive_entry_set_uid(entry, ent->fts_statp->st_uid);
			archive_entry_set_gid(entry, ent->fts_statp->st_gid);

			archive_write_header(a, entry);

			/* Copy file contents */
			FILE *f = fopen(ent->fts_path, "rb");
			if (f) {
				char buf[65536];
				size_t n;
				while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
					archive_write_data(a, buf, n);
				fclose(f);
			}

			archive_entry_free(entry);
		} else if (ent->fts_info == FTS_D) {
			entry = archive_entry_new();
			archive_entry_set_pathname(entry, path);
			archive_entry_set_filetype(entry, AE_IFDIR);
			archive_entry_set_mode(entry, ent->fts_statp->st_mode);
			archive_write_header(a, entry);
			archive_entry_free(entry);
		} else if (ent->fts_info == FTS_SL) {
			char linkbuf[PATH_MAX];
			ssize_t linklen = readlink(ent->fts_path, linkbuf, sizeof(linkbuf) - 1);
			if (linklen > 0) {
				linkbuf[linklen] = '\0';
				entry = archive_entry_new();
				archive_entry_set_pathname(entry, path);
				archive_entry_set_filetype(entry, AE_IFLNK);
				archive_entry_set_symlink(entry, linkbuf);
				archive_entry_set_mode(entry, ent->fts_statp->st_mode);
				archive_write_header(a, entry);
				archive_entry_free(entry);
			}
		}
	}

	fts_close(fts);
	archive_write_close(a);
	archive_write_free(a);

	return (ret);
}

/*
 * Create layer from directory
 */
int
create_layer_from_directory(const char *srcdir, const char *destfile,
    const char **exclude_patterns __unused, int nexclude __unused)
{
	return (create_tar_from_directory(srcdir, destfile));
}

/*
 * Push entire image
 */
int
push_image(struct registry *reg, const char *reference,
    const char *sourcedir, progress_cb cb, void *opaque)
{
	char *registry, *repo, *tag, *digest;
	char manifest_path[PATH_MAX];
	struct oci_layer *layers = NULL;
	struct oci_layer **layer_ptrs = NULL;
	int nlayers = 0;
	DIR *dir;
	struct dirent *ent;
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

	/* Find layer files in sourcedir */
	snprintf(manifest_path, sizeof(manifest_path), "%s/layers", sourcedir);
	dir = opendir(manifest_path);
	if (dir == NULL) {
		fprintf(stderr, "error: cannot open layers directory: %s\n",
		    manifest_path);
		goto cleanup;
	}

	while ((ent = readdir(dir)) != NULL) {
		char layer_path[PATH_MAX];
		char *layer_digest;
		struct oci_layer *grown;
		struct stat lst;

		if (ent->d_type != DT_REG)
			continue;

		snprintf(layer_path, sizeof(layer_path), "%s/%s",
		    manifest_path, ent->d_name);

		/* Compute digest of layer */
		if (compute_file_digest(layer_path, &layer_digest) != 0) {
			fprintf(stderr, "error: failed to compute digest for %s\n",
			    layer_path);
			continue;
		}

		/* Push layer */
		fprintf(stderr, "pushing layer: %s\n", layer_digest);
		if (push_layer(reg, layer_path, layer_digest, cb, opaque) != 0) {
			fprintf(stderr, "error: failed to push layer %s\n",
			    layer_digest);
			free(layer_digest);
			continue;
		}

		/* Track the layer as a real descriptor (digest + size) */
		grown = realloc(layers, (nlayers + 1) * sizeof(*layers));
		if (grown == NULL) {
			free(layer_digest);
			closedir(dir);
			ret = -1;
			goto cleanup;
		}
		layers = grown;
		memset(&layers[nlayers], 0, sizeof(layers[nlayers]));
		layers[nlayers].digest = layer_digest;
		layers[nlayers].size = (stat(layer_path, &lst) == 0) ?
		    (size_t)lst.st_size : 0;
		nlayers++;
	}
	closedir(dir);

	/* Push manifest */
	char *manifest_json;
	char *config_digest = NULL;
	char config_path[PATH_MAX];

	snprintf(config_path, sizeof(config_path), "%s/config.json", sourcedir);
	if (compute_file_digest(config_path, &config_digest) != 0) {
		fprintf(stderr, "error: failed to compute config digest: %s\n",
		    config_path);
		goto cleanup;
	}

	/* Build an array of pointers to the layer descriptors. */
	if (nlayers > 0) {
		layer_ptrs = calloc(nlayers, sizeof(*layer_ptrs));
		if (layer_ptrs == NULL) {
			ret = -1;
			goto cleanup;
		}
		for (i = 0; i < nlayers; i++)
			layer_ptrs[i] = &layers[i];
	}

	ret = create_manifest(config_digest, layer_ptrs, nlayers, &manifest_json);
	if (ret == 0) {
		ret = push_manifest(reg, repo, tag, manifest_json);
		free(manifest_json);
	}

cleanup:
	free(registry);
	free(repo);
	free(tag);
	free(digest);
	free(config_digest);

	for (i = 0; i < nlayers; i++)
		free(layers[i].digest);
	free(layers);
	free(layer_ptrs);

	return (ret);
}

/*
 * Create OCI manifest JSON
 */
int
create_manifest(const char *config_digest, struct oci_layer **layers,
    int nlayers, char **manifest_json)
{
	json_object *manifest;
	json_object *config;
	json_object *layers_arr;
	int i;

	manifest = json_object_new_object();
	json_object_object_add(manifest, "schemaVersion",
	    json_object_new_int(2));
	json_object_object_add(manifest, "mediaType",
	    json_object_new_string(OCI_MEDIA_TYPE_MANIFEST));

	/* Config descriptor */
	config = json_object_new_object();
	json_object_object_add(config, "mediaType",
	    json_object_new_string(OCI_MEDIA_TYPE_CONFIG));
	json_object_object_add(config, "digest", json_object_new_string(config_digest));
	json_object_object_add(config, "size", json_object_new_int(0));
	json_object_object_add(manifest, "config", config);

	/* Layers */
	layers_arr = json_object_new_array();
	for (i = 0; i < nlayers; i++) {
		json_object *layer_desc = json_object_new_object();
		json_object_object_add(layer_desc, "mediaType",
		    json_object_new_string(OCI_MEDIA_TYPE_LAYER));
		json_object_object_add(layer_desc, "digest",
		    json_object_new_string(layers[i]->digest));
		json_object_object_add(layer_desc, "size",
		    json_object_new_int64(layers[i]->size));
		json_object_array_add(layers_arr, layer_desc);
	}
	json_object_object_add(manifest, "layers", layers_arr);

	*manifest_json = strdup(json_object_to_json_string_ext(manifest,
	    JSON_C_TO_STRING_PRETTY));

	json_object_put(manifest);

	return (0);
}

/*
 * Push manifest to registry
 */
int
push_manifest(struct registry *reg, const char *repo, const char *tag,
    const char *manifest_json)
{
	char *url;
	CURL *curl;
	struct curl_slist *headers = NULL;
	long response_code;
	int ret = -1;

	/* Build manifest URL */
	if (asprintf(&url, "https://%s:%d/v2/%s/manifests/%s",
	    reg->host, reg->port, repo, tag) == -1)
		return (-1);

	struct mem_reader reader = { manifest_json, strlen(manifest_json), 0 };

	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, mem_read_callback);
	curl_easy_setopt(curl, CURLOPT_READDATA, &reader);
	curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
	    (curl_off_t)reader.len);

	headers = curl_slist_append(headers,
	    "Content-Type: application/vnd.oci.image.manifest.v1+json");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	CURLcode res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code == 201 || response_code == 202) {
			ret = 0;
		}
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(url);

	return (ret);
}
