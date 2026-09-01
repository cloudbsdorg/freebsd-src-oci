/*-
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
 * OCI image layer unpacking
 */

#ifndef _OCIFBSD_IMAGE_UNPACK_H
#define _OCIFBSD_IMAGE_UNPACK_H

#include <sys/types.h>
#include <stdbool.h>

/*
 * Unpack options
 */
struct unpack_options {
	bool		keep_permissions;	/* preserve uid/gid */
	bool		expand_whiteouts;	/* expand whiteout files */
	bool		strip_whiteouts;	/* remove whiteout files after expanding */
	bool		preserve_xattrs;	/* preserve extended attributes */
	char		*root;			/* destination root */
};

/*
 * Whiteout handling
 *
 * In OCI/Docker, whiteout files are used to mark files that should be
 * deleted when building the layer. These are files with a ".wh." prefix
 * in their name. When unpacking, we can either:
 *   1. Expand whiteouts: create .wh.<filename> to mark deletion
 *   2. Strip whiteouts: actually delete the marked files
 */
struct whiteout_info {
	char	**files;	/* files to be whiteouted */
	int	nfiles;
};

/*
 * Unpack operations
 */
int	 unpack_layer(const char *tarball, const char *dest,
	     struct unpack_options *opts);
int	 unpack_layers(const char **tarballs, int ntarballs,
	     const char *dest, struct unpack_options *opts);
int	 unpack_image(const char *imagedir, const char *dest,
	     struct unpack_options *opts);

/*
 * Whiteout operations
 */
int	 find_whiteouts(const char *dir, struct whiteout_info **info);
int	 expand_whiteouts(const char *dir, const struct whiteout_info *info);
int	 apply_whiteouts(const char *dir);
void	 free_whiteout_info(struct whiteout_info *info);

/*
 * Whiteout file detection
 */
bool	 is_whiteout(const char *filename);
bool	 is_opaque(const char *dirname);
char	*get_whiteout_target(const char *whiteout_name);

/*
 * Compression detection
 */
typedef enum {
	COMPRESSION_NONE = 0,
	COMPRESSION_GZIP,
	COMPRESSION_BZIP2,
	COMPRESSION_XZ,
	COMPRESSION_ZSTD
} compression_type_t;

compression_type_t detect_compression(const char *path);
const char *compression_extension(compression_type_t type);

#endif /* _OCIFBSD_IMAGE_UNPACK_H */
