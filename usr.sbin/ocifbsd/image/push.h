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
 * OCI image push to registry
 */

#ifndef _OCIFBSD_IMAGE_PUSH_H
#define _OCIFBSD_IMAGE_PUSH_H

#include <sys/types.h>
#include <stdbool.h>

#include "pull.h"

/*
 * Push options
 */
struct push_options {
	bool		force;		/* overwrite existing */
	bool		verify;		/* verify layer before push */
	bool		quiet;		/* suppress progress output */
};

/*
 * Push operations
 */
int	 push_image(struct registry *reg, const char *reference,
	     const char *sourcedir, progress_cb cb, void *opaque);
int	 push_layer(struct registry *reg, const char *layer_path,
	     const char *digest, progress_cb cb, void *opaque);

/*
 * Layer creation
 */
int	 create_layer_from_directory(const char *srcdir, const char *destfile,
	     const char **exclude_patterns, int nexclude);
int	 compute_layer_diff(const char *base, const char *layer,
	     char **digest);

/*
 * Manifest creation
 */
int	 create_manifest(const char *config_digest, struct oci_layer **layers,
	     int nlayers, char **manifest_json);
int	 push_manifest(struct registry *reg, const char *repo, const char *tag,
	     const char *manifest_json);

/*
 * Upload session management
 */
struct upload_session {
	char	*location;	/* upload URL */
	char	*uuid;		/* upload UUID */
	char	*host;		/* registry host */
	int	socket;		/* connection socket */
};

struct upload_session *upload_start(struct registry *reg, const char *repo);
int	 upload_chunk(struct upload_session *sess, const char *data, size_t len);
int	 upload_complete(struct upload_session *sess, const char *digest);
void	 upload_abort(struct upload_session *sess);

#endif /* _OCIFBSD_IMAGE_PUSH_H */
