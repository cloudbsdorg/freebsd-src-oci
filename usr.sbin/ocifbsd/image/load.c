/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * $FreeBSD$
 *
 * Load a local OCI image archive into the image store.
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "load.h"
#include "pull.h"
#include "unpack.h"
#include "zfs_store.h"

/* Recursive mkdir; returns 0 on success (or if the dir already exists). */
static int
load_mkdirp(const char *path, mode_t mode)
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

/* Recursive remove of a directory tree (best-effort). */
static void
load_rm_rf(const char *path)
{
	char cmd_path[PATH_MAX];
	struct stat st;
	DIR *d;
	struct dirent *e;

	if (path == NULL || lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		d = opendir(path);
		if (d != NULL) {
			while ((e = readdir(d)) != NULL) {
				if (strcmp(e->d_name, ".") == 0 ||
				    strcmp(e->d_name, "..") == 0)
					continue;
				snprintf(cmd_path, sizeof(cmd_path), "%s/%s",
				    path, e->d_name);
				load_rm_rf(cmd_path);
			}
			closedir(d);
		}
		rmdir(path);
	} else {
		unlink(path);
	}
}

/*
 * Extract a tar archive (any libarchive-supported compression) into destdir.
 * Rejects entries with absolute paths or ".." components so a crafted archive
 * cannot escape destdir. Returns 0 on success.
 */
static int
load_extract_archive(const char *archive_path, const char *destdir)
{
	struct archive *a, *ext;
	struct archive_entry *entry;
	int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM;
	int ret = -1;
	int r;

	a = archive_read_new();
	ext = archive_write_disk_new();
	if (a == NULL || ext == NULL)
		goto out;
	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);
	archive_write_disk_set_options(ext, flags);

	if (archive_read_open_filename(a, archive_path, 65536) != ARCHIVE_OK)
		goto out;

	for (;;) {
		const char *name;
		char full[PATH_MAX];

		r = archive_read_next_header(a, &entry);
		if (r == ARCHIVE_EOF)
			break;
		if (r != ARCHIVE_OK)
			goto out;

		name = archive_entry_pathname(entry);
		if (name == NULL || name[0] == '/' || strstr(name, "..") != NULL)
			continue;	/* skip unsafe entry */

		if ((size_t)snprintf(full, sizeof(full), "%s/%s", destdir, name) >=
		    sizeof(full))
			continue;
		archive_entry_set_pathname(entry, full);

		if (archive_write_header(ext, entry) != ARCHIVE_OK)
			continue;
		if (archive_entry_size(entry) > 0) {
			const void *buff;
			size_t size;
			la_int64_t offset;

			for (;;) {
				r = archive_read_data_block(a, &buff, &size,
				    &offset);
				if (r == ARCHIVE_EOF)
					break;
				if (r != ARCHIVE_OK)
					goto out;
				if (archive_write_data_block(ext, buff, size,
				    offset) != ARCHIVE_OK)
					goto out;
			}
		}
		archive_write_finish_entry(ext);
	}
	ret = 0;
out:
	if (a != NULL) {
		archive_read_close(a);
		archive_read_free(a);
	}
	if (ext != NULL) {
		archive_write_close(ext);
		archive_write_free(ext);
	}
	return (ret);
}

/* Build blobs/sha256/<hex> path from a "sha256:<hex>" digest. */
static int
blob_path(const char *layoutdir, const char *digest, char *out, size_t outlen)
{
	const char *colon = strchr(digest != NULL ? digest : "", ':');
	const char *algo, *hex;
	size_t algolen;

	if (colon == NULL)
		return (-1);
	algo = digest;
	algolen = (size_t)(colon - digest);
	hex = colon + 1;
	if ((size_t)snprintf(out, outlen, "%s/blobs/%.*s/%s", layoutdir,
	    (int)algolen, algo, hex) >= outlen)
		return (-1);
	return (0);
}

/* Write a properly-escaped JSON string literal (with quotes) to fp. */
static void
fjson_str(FILE *fp, const char *s)
{
	fputc('"', fp);
	for (; s != NULL && *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		switch (c) {
		case '"':
		case '\\':
			fputc('\\', fp);
			fputc(c, fp);
			break;
		case '\n': fputs("\\n", fp); break;
		case '\r': fputs("\\r", fp); break;
		case '\t': fputs("\\t", fp); break;
		default:
			if (c < 32)
				fprintf(fp, "\\u%04x", c);
			else
				fputc(c, fp);
		}
	}
	fputc('"', fp);
}

/*
 * Emit a JSON string array element list a, "b" (comma-separated, no
 * brackets) from a json array, or a default fallback. Writes into fp.
 */
static void
emit_json_str_array(FILE *fp, struct json_object *arr, const char *fallback)
{
	int i, n;

	if (arr == NULL || json_object_get_type(arr) != json_type_array ||
	    json_object_array_length(arr) == 0) {
		if (fallback != NULL)
			fprintf(fp, "%s", fallback);
		return;
	}
	n = json_object_array_length(arr);
	for (i = 0; i < n; i++) {
		struct json_object *el = json_object_array_get_idx(arr, i);

		if (i)
			fprintf(fp, ", ");
		fjson_str(fp, json_object_get_string(el));
	}
}

/*
 * Generate an OCI runtime config.json in storedir from the image config blob
 * (which carries Cmd/Entrypoint/Env/WorkingDir). Returns 0 on success.
 */
static int
write_runtime_config(const char *storedir, const char *config_blob)
{
	struct json_object *cfg = NULL, *inner = NULL;
	struct json_object *cmd = NULL, *entrypoint = NULL, *env = NULL;
	struct json_object *workdir = NULL;
	char path[PATH_MAX];
	FILE *fp;
	const char *cwd = "/";

	if (config_blob != NULL)
		cfg = json_object_from_file(config_blob);
	if (cfg != NULL && json_object_object_get_ex(cfg, "config", &inner) &&
	    inner != NULL) {
		json_object_object_get_ex(inner, "Cmd", &cmd);
		json_object_object_get_ex(inner, "Entrypoint", &entrypoint);
		json_object_object_get_ex(inner, "Env", &env);
		if (json_object_object_get_ex(inner, "WorkingDir", &workdir) &&
		    workdir != NULL) {
			const char *w = json_object_get_string(workdir);
			if (w != NULL && w[0] != '\0')
				cwd = w;
		}
	}

	snprintf(path, sizeof(path), "%s/config.json", storedir);
	fp = fopen(path, "w");
	if (fp == NULL) {
		if (cfg != NULL)
			json_object_put(cfg);
		return (-1);
	}

	fprintf(fp, "{\n");
	fprintf(fp, "  \"ociVersion\": \"1.0.2\",\n");
	fprintf(fp, "  \"process\": {\n");
	fprintf(fp, "    \"terminal\": false,\n");
	fprintf(fp, "    \"user\": { \"uid\": 0, \"gid\": 0 },\n");
	fprintf(fp, "    \"args\": [ ");
	/* args = Entrypoint followed by Cmd; default to /bin/sh. */
	if ((entrypoint != NULL &&
	    json_object_get_type(entrypoint) == json_type_array &&
	    json_object_array_length(entrypoint) > 0) ||
	    (cmd != NULL && json_object_get_type(cmd) == json_type_array &&
	    json_object_array_length(cmd) > 0)) {
		bool need_comma = false;
		if (entrypoint != NULL &&
		    json_object_get_type(entrypoint) == json_type_array &&
		    json_object_array_length(entrypoint) > 0) {
			emit_json_str_array(fp, entrypoint, NULL);
			need_comma = json_object_array_length(entrypoint) > 0;
		}
		if (cmd != NULL &&
		    json_object_get_type(cmd) == json_type_array &&
		    json_object_array_length(cmd) > 0) {
			if (need_comma)
				fprintf(fp, ", ");
			emit_json_str_array(fp, cmd, NULL);
		}
	} else {
		fprintf(fp, "\"/bin/sh\"");
	}
	fprintf(fp, " ],\n");
	fprintf(fp, "    \"env\": [ ");
	emit_json_str_array(fp, env,
	    "\"PATH=/bin:/sbin:/usr/bin:/usr/sbin\"");
	fprintf(fp, " ],\n");
	fprintf(fp, "    \"cwd\": ");
	fjson_str(fp, cwd);
	fprintf(fp, "\n");
	fprintf(fp, "  },\n");
	fprintf(fp, "  \"root\": { \"path\": \"rootfs\", \"readonly\": false }\n");
	fprintf(fp, "}\n");
	fclose(fp);

	if (cfg != NULL)
		json_object_put(cfg);
	return (0);
}

int
load_oci_archive(const char *archive_path, const char *ref_override,
    char **out_store_path)
{
	char layoutdir[PATH_MAX];
	char tmpl[PATH_MAX];
	char idxpath[PATH_MAX];
	char mpath[PATH_MAX];
	char cpath[PATH_MAX];
	char rootfs[PATH_MAX];
	struct stat st;
	struct json_object *index = NULL, *manifests = NULL, *m0 = NULL;
	struct json_object *mdigest = NULL, *annos = NULL, *refname = NULL;
	struct json_object *manifest = NULL, *config = NULL, *cdigest = NULL;
	struct json_object *layers = NULL;
	char *registry = NULL, *repo = NULL, *tag = NULL, *rdigest = NULL;
	char *store = NULL;
	const char *ref;
	bool made_tmp = false;
	int ret = -1;
	int i, nlayers;

	if (archive_path == NULL)
		return (-1);

	/* Resolve the image-layout directory (extract the archive if needed). */
	if (stat(archive_path, &st) != 0)
		return (-1);
	if (S_ISDIR(st.st_mode)) {
		strlcpy(layoutdir, archive_path, sizeof(layoutdir));
	} else {
		const char *base = getenv("OCIFBSD_DATA_DIR");
		if (base == NULL || base[0] == '\0')
			base = "/var/lib/ocifbsd";
		snprintf(tmpl, sizeof(tmpl), "%s/.load.XXXXXX", base);
		load_mkdirp(base, 0755);
		if (mkdtemp(tmpl) == NULL) {
			/* Fall back to /tmp. */
			snprintf(tmpl, sizeof(tmpl), "/tmp/ocifbsd-load.XXXXXX");
			if (mkdtemp(tmpl) == NULL)
				return (-1);
		}
		made_tmp = true;
		strlcpy(layoutdir, tmpl, sizeof(layoutdir));
		if (load_extract_archive(archive_path, layoutdir) != 0)
			goto out;
	}

	/* Read index.json. */
	snprintf(idxpath, sizeof(idxpath), "%s/index.json", layoutdir);
	index = json_object_from_file(idxpath);
	if (index == NULL ||
	    !json_object_object_get_ex(index, "manifests", &manifests) ||
	    json_object_get_type(manifests) != json_type_array ||
	    json_object_array_length(manifests) == 0) {
		fprintf(stderr, "error: %s is not an OCI image layout\n",
		    archive_path);
		goto out;
	}
	m0 = json_object_array_get_idx(manifests, 0);
	if (!json_object_object_get_ex(m0, "digest", &mdigest)) {
		fprintf(stderr, "error: index manifest has no digest\n");
		goto out;
	}

	/* Determine the reference name. */
	ref = ref_override;
	if (ref == NULL && json_object_object_get_ex(m0, "annotations", &annos) &&
	    json_object_object_get_ex(annos,
	    "org.opencontainers.image.ref.name", &refname))
		ref = json_object_get_string(refname);
	if (ref == NULL || ref[0] == '\0')
		ref = "imported:latest";

	if (parse_reference(ref, &registry, &repo, &tag, &rdigest) != 0) {
		fprintf(stderr, "error: invalid reference: %s\n", ref);
		goto out;
	}
	store = zfs_image_path(registry, repo, tag);
	if (store == NULL)
		goto out;

	/* Read the image manifest. */
	if (blob_path(layoutdir, json_object_get_string(mdigest), mpath,
	    sizeof(mpath)) != 0)
		goto out;
	manifest = json_object_from_file(mpath);
	if (manifest == NULL) {
		fprintf(stderr, "error: cannot read manifest blob\n");
		goto out;
	}
	if (!json_object_object_get_ex(manifest, "layers", &layers) ||
	    json_object_get_type(layers) != json_type_array) {
		fprintf(stderr, "error: manifest has no layers array\n");
		goto out;
	}

	/* Prepare the store rootfs. */
	snprintf(rootfs, sizeof(rootfs), "%s/rootfs", store);
	if (load_mkdirp(rootfs, 0755) != 0) {
		fprintf(stderr, "error: cannot create %s: %s\n", rootfs,
		    strerror(errno));
		goto out;
	}

	/* Verify and unpack each layer in order. */
	nlayers = json_object_array_length(layers);
	for (i = 0; i < nlayers; i++) {
		struct json_object *layer, *ldigest;
		const char *ds;
		char lpath[PATH_MAX];
		struct unpack_options opts;

		layer = json_object_array_get_idx(layers, i);
		if (layer == NULL ||
		    !json_object_object_get_ex(layer, "digest", &ldigest))
			continue;
		ds = json_object_get_string(ldigest);
		if (!digest_is_valid(ds)) {
			fprintf(stderr, "error: invalid layer digest: %s\n",
			    ds ? ds : "(null)");
			goto out;
		}
		if (blob_path(layoutdir, ds, lpath, sizeof(lpath)) != 0)
			goto out;
		if (verify_layer(lpath, ds) != 0) {
			fprintf(stderr, "error: layer digest mismatch: %s\n",
			    ds);
			goto out;
		}
		memset(&opts, 0, sizeof(opts));
		opts.keep_permissions = true;
		opts.strip_whiteouts = true;
		if (unpack_layer(lpath, rootfs, &opts) != 0) {
			fprintf(stderr, "error: failed to unpack layer %s\n",
			    ds);
			goto out;
		}
	}

	/* Write the runtime config.json from the image config blob. */
	cpath[0] = '\0';
	if (json_object_object_get_ex(manifest, "config", &config) &&
	    json_object_object_get_ex(config, "digest", &cdigest))
		blob_path(layoutdir, json_object_get_string(cdigest), cpath,
		    sizeof(cpath));
	if (write_runtime_config(store, cpath[0] ? cpath : NULL) != 0) {
		fprintf(stderr, "error: cannot write config.json\n");
		goto out;
	}

	if (out_store_path != NULL) {
		*out_store_path = store;
		store = NULL;
	}
	ret = 0;

out:
	if (made_tmp)
		load_rm_rf(layoutdir);
	if (index != NULL)
		json_object_put(index);
	if (manifest != NULL)
		json_object_put(manifest);
	free(registry);
	free(repo);
	free(tag);
	free(rdigest);
	free(store);
	return (ret);
}
