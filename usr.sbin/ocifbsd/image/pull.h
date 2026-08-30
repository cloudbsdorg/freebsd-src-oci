/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 * Copyright (c) 2026 REVYTECH, Inc.
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

#ifndef _OCIFBSD_IMAGE_PULL_H
#define _OCIFBSD_IMAGE_PULL_H

#include <sys/types.h>
#include <stdbool.h>

/*
 * OCI Media Types
 */
#define OCI_MEDIA_TYPE_MANIFEST		"application/vnd.oci.image.manifest.v1+json"
#define OCI_MEDIA_TYPE_CONFIG		"application/vnd.oci.image.config.v1+json"
#define OCI_MEDIA_TYPE_LAYER		"application/vnd.oci.image.layer.v1.tar+gzip"
#define OCI_MEDIA_TYPE_LAYER_NONDIST	"application/vnd.oci.image.layer.nondistributable.v1.tar+gzip"

/*
 * Docker Media Types (for compatibility)
 */
#define DOCKER_MEDIA_TYPE_MANIFEST	"application/vnd.docker.distribution.manifest.v2+json"
#define DOCKER_MEDIA_TYPE_LAYER		"application/vnd.docker.image.rootfs.diff.tar.gzip"

/*
 * Registry authentication methods
 */
typedef enum {
	AUTH_NONE = 0,
	AUTH_BASIC,
	AUTH_BEARER,
	AUTH_ANONYMOUS
} auth_type_t;

/*
 * Auth credentials
 */
struct registry_auth {
	auth_type_t	type;
	char		*username;
	char		*password;
	char		*registry;	/* for token scope */
	char		*service;	/* for token service */
};

/*
 * Layer descriptor from manifest
 */
struct oci_layer {
	char	*digest;		/* sha256:<hex> */
	char	*media_type;
	size_t	size;
	char	*url;			/* download URL */
	bool	digest_verified;
};

/*
 * Image configuration from config blob
 */
struct oci_config {
	char		*architecture;
	char		*os;
	char		*config;	/* raw JSON config */
	size_t		config_size;
	char		*created;
	char		*author;
	char		*entrypoint;	/* from config */
	char		**cmd;		/* from config */
	char		**env;		/* from config */
	char		*workdir;	/* from config */
	char		*user;		/* from config */
	char		*exposed_ports;	/* from config */
};

/*
 * Image manifest
 */
struct oci_manifest {
	char			*schema_version;
	char			*media_type;
	struct oci_config	*config;	/* config descriptor */
	struct oci_layer	**layers;
	int			nlayers;
	char			*raw;		/* raw JSON */
};

/*
 * Progress callback
 */
typedef void (*progress_cb)(void *opaque, const char *what,
    off_t current, off_t total);

/*
 * Registry operations
 */
struct registry {
	char	*host;			/* registry hostname */
	int	port;			/* registry port */
	char	*path_prefix;		/* API path prefix */
	bool	tls;			/* use HTTPS */
	char	*repository;		/* last reference's repo path */
	char	*tag;			/* last reference's tag */
	struct registry_auth *auth;
};

/*
 * Pull operations
 */
int	 registry_init(struct registry *reg, const char *reference);
void	 registry_free(struct registry *reg);
int	 registry_pull(struct registry *reg, const char *reference,
	     const char *destdir, progress_cb cb, void *opaque);
int	 registry_pull_layer(struct registry *reg, struct oci_layer *layer,
	     const char *destdir, progress_cb cb, void *opaque);

/*
 * Manifest operations
 */
int	 fetch_manifest(struct registry *reg, const char *repo,
	     const char *tag, struct oci_manifest **manifest);
int	 parse_manifest(const char *json, size_t len,
	     struct oci_manifest **manifest);
void	 free_manifest(struct oci_manifest *manifest);

/*
 * Config operations
 */
int	 fetch_config(struct registry *reg, const char *repo,
	     const char *digest, struct oci_config **config);
int	 parse_config(const char *json, size_t len,
	     struct oci_config **config);
void	 free_config(struct oci_config *config);

/*
 * Authentication operations
 */
int	 authenticate(struct registry *reg, const char *scope);
char	*get_bearer_token(const char *auth_header);
int	 refresh_token(struct registry *reg);

/*
 * Reference parsing
 */
int	 parse_reference(const char *ref, char **registry, char **repo,
	     char **tag, char **digest);
int	 canonicalize_reference(const char *ref, char **canonical);

/*
 * Layer verification
 */
int	 verify_layer(const char *path, const char *expected_digest);
int	 compute_digest(const char *path, char **digest);
bool	 digest_is_valid(const char *digest);

#endif /* _OCIFBSD_IMAGE_PULL_H */
