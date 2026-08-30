/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * $FreeBSD$
 *
 * Load a local OCI image archive (oci-layout/index.json/blobs) into the
 * local image store so `ocifbsd create --image` / `run --image` can use it.
 */

#ifndef _OCIFBSD_IMAGE_LOAD_H
#define _OCIFBSD_IMAGE_LOAD_H

/*
 * Import an OCI image from a local archive or directory.
 *
 * archive_path may be either a directory in OCI image-layout form (contains
 * oci-layout, index.json and blobs/), or a .tar/.tar.gz/.tar.xz archive of
 * that layout (as shipped by download.freebsd.org's OCI-IMAGES).
 *
 * ref_override, if non-NULL, names the store entry (registry/repo:tag form);
 * otherwise the org.opencontainers.image.ref.name annotation from index.json
 * is used.
 *
 * On success returns 0 and, if out_store_path is non-NULL, sets it to a
 * malloc'd store path (the caller frees). Returns -1 on failure.
 */
int	load_oci_archive(const char *archive_path, const char *ref_override,
	    char **out_store_path);

#endif /* _OCIFBSD_IMAGE_LOAD_H */
