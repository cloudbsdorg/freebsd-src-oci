/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Registry alias table for ocifbsd.
 *
 * Maps a registry reference name (as it appears in an image reference, e.g.
 * "docker.io") to the concrete API host, token realm, and token service used
 * to talk to it. The mapping is data, not code: it is loaded from a
 * user-editable configuration file, falling back to built-in defaults so the
 * common registries work out of the box.
 *
 * Configuration file search order (first that exists wins):
 *   1. $OCIFBSD_REGISTRIES_CONF
 *   2. /usr/local/etc/ocifbsd/registries.conf
 *   3. /etc/ocifbsd/registries.conf
 *
 * File format: one registry per line, whitespace-separated columns; blank
 * lines and lines beginning with '#' are ignored. Use '-' for an absent
 * optional field.
 *
 *   # name            api_host              auth_realm                     auth_service         tls
 *   docker.io         registry-1.docker.io  https://auth.docker.io/token   registry.docker.io   https
 *   registry.example  registry.example      -                              -                    https
 *   mirror.internal   mirror.internal       -                              -                    http
 *
 * Transport policy follows the Docker/Podman model: TLS (HTTPS) is required by
 * default. The optional trailing "tls" column marks a registry insecure --
 * "http" (aliases: "insecure", "true") permits plain HTTP; "https" (aliases:
 * "secure", "tls", "false", or an absent column) requires TLS. As in Podman,
 * localhost (localhost, *.localhost, 127.0.0.0/8, ::1) is treated as insecure
 * by default without any configuration, but a config entry can override it.
 */

#ifndef OCIFBSD_IMAGE_REGISTRIES_H
#define OCIFBSD_IMAGE_REGISTRIES_H

struct registry_alias {
	char	*name;		/* reference name, e.g. "docker.io" */
	char	*api_host;	/* API host, e.g. "registry-1.docker.io" */
	char	*auth_realm;	/* token endpoint URL, or NULL if none */
	char	*auth_service;	/* token service, or NULL if none */
	int	 insecure;	/* nonzero: plain HTTP permitted (no TLS) */
};

/*
 * Look up a registry alias by reference name (the host portion of an image
 * reference). Returns a pointer into an internal cached table -- do NOT free
 * or modify it -- or NULL if the name is not configured. The configuration
 * file is loaded and merged over the built-in defaults on first use.
 */
const struct registry_alias	*registry_alias_lookup(const char *name);

/*
 * Look up a registry alias by its API host (used for the token-realm fallback
 * when a registry challenges without a WWW-Authenticate header). Returns a
 * cached pointer or NULL.
 */
const struct registry_alias	*registry_alias_by_host(const char *host);

/*
 * Whether plain HTTP (no TLS) is permitted for a registry reference name.
 * Returns nonzero for a registry the configuration marks insecure, and for
 * localhost by default (unless a config entry overrides it); zero otherwise.
 * TLS is the default, so callers should attempt HTTPS unless this returns
 * nonzero.
 */
int				 registry_alias_insecure(const char *name);

/*
 * The registry used for unqualified image references (a short name like
 * "nginx" with no registry host). Defaults to "docker.io"; override in the
 * configuration file with a line:
 *
 *   default-registry   registry.example.com
 *
 * Returns a pointer into internal storage -- do not free.
 */
const char			*registry_default_name(void);

#endif /* OCIFBSD_IMAGE_REGISTRIES_H */
