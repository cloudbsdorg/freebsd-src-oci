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
 * Line- and indent-aware YAML reader for the manifest converters.
 */

#ifndef _OCIFBSD_YAML_H
#define _OCIFBSD_YAML_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A parsed node. A document is a tree of these.
 *
 *   mapping entry   key != NULL; value is the scalar, or NULL when the entry
 *                   has children instead
 *   sequence item   key == NULL; either value (a scalar item) or children (an
 *                   item that is itself a mapping)
 *
 * Children are held in insertion order, which is also document order, so a
 * numeric path component addresses the n-th sequence item.
 */
struct yaml_node {
	char			*key;
	char			*value;
	struct yaml_node	**children;
	int			 nchildren;
	int			 nalloc;
	bool			 seq;	/* children are sequence items */
	int			 indent;	/* parser bookkeeping */
};

/*
 * Parse a NUL-terminated YAML document into a tree. Returns a root node whose
 * children are the document's top-level entries, or NULL on syntax error or
 * allocation failure (errno set to EINVAL or ENOMEM). Free with yaml_free().
 *
 * Deliberately a SUBSET of YAML -- block mappings, block sequences, scalars,
 * quoted scalars and comments -- which is what the manifests this converts
 * actually use. Anything outside that subset (anchors, aliases, flow
 * collections, multi-line scalars, tabs used for indentation) is reported as
 * an error rather than silently misread.
 */
struct yaml_node	*yaml_parse(const char *text);
void			 yaml_free(struct yaml_node *root);

/*
 * Look up a node by dot-separated path, e.g. "metadata.name" or
 * "spec.template.spec.containers.0.image". A numeric component indexes a
 * sequence. Returns the node, or NULL if the path does not exist.
 */
struct yaml_node	*yaml_get(struct yaml_node *root, const char *path);

/*
 * Convenience: the scalar at `path`, or NULL. The returned pointer is owned by
 * the tree and is valid until yaml_free().
 */
const char		*yaml_get_scalar(struct yaml_node *root,
			    const char *path);

/*
 * The scalar at the FIRST node anywhere in the tree whose key is `key`,
 * searched depth-first in document order. This reproduces the behaviour of the
 * old substring scanner for the handful of lookups where the manifest schema
 * genuinely does not pin the location -- but it matches whole keys in the
 * parsed tree, so it can no longer match a key inside another key's name or a
 * value inside a comment.
 */
const char		*yaml_find_scalar(struct yaml_node *root,
			    const char *key);

/*
 * Typed, range-checked accessors.
 *
 * A manifest field that is supposed to be a number must BE a number. The
 * converters used to run the raw scalar through atoi(), which maps "abc",
 * "", "3.5" and "99999999999999" all onto a plausible-looking integer and
 * silently substitutes it -- so a typo became "zero replicas" or a nonsense
 * port instead of an error the author could see and correct.
 *
 * yaml_get_int consumes the WHOLE scalar, rejects overflow, and enforces
 * [min, max]. It returns 0 on success and -1 otherwise, leaving *out
 * untouched; *why (if non-NULL) receives a short reason suitable for an error
 * message. A missing path is distinguished from a malformed value by
 * yaml_get() returning NULL, so callers can apply a default only when the
 * field is genuinely absent.
 */
int	 yaml_get_int(struct yaml_node *root, const char *path, long min,
	    long max, long *out, const char **why);

/* Ports are 1-65535 by definition; 0 is not a usable listening port. */
#define	YAML_PORT_MIN	1L
#define	YAML_PORT_MAX	65535L

/*
 * Strict scalar-to-integer conversion, exposed for callers that already hold
 * a string. Same contract as yaml_get_int.
 */
int	 yaml_parse_int(const char *s, long min, long max, long *out,
	    const char **why);

/*
 * Render `value` as a scalar that is safe to emit into generated YAML.
 * Writes into buf and returns it. A value containing a newline, a leading or
 * trailing space, a comment marker, or any character that would end the scalar
 * is single-quoted with embedded quotes doubled; an empty or NULL value
 * becomes ''. Without this, a manifest value such as
 *
 *	name: web\nservices:
 *
 * is copied verbatim into the output and injects document structure the
 * manifest never declared.
 */
const char		*yaml_quote(const char *value, char *buf, size_t buflen);

#endif /* _OCIFBSD_YAML_H */
