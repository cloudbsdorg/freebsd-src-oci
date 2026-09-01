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
 * YAML/JSON parser utilities for config conversion
 */

#include <sys/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "convert.h"

/*
 * Parse YAML (stub - uses simple string parsing)
 */
int
parse_yaml(const char *input, char **output, struct convert_options *opts)
{
	/* Simple pass-through for now */
	/* Full implementation would parse YAML into intermediate representation */
	*output = strdup(input);
	return (*output != NULL) ? CONVERT_SUCCESS : CONVERT_MEMORY_ERROR;
}

/*
 * Parse JSON (stub - uses simple string parsing)
 */
int
parse_json(const char *input, char **output, struct convert_options *opts)
{
	/* Simple pass-through for now */
	/* Full implementation would parse JSON into intermediate representation */
	*output = strdup(input);
	return (*output != NULL) ? CONVERT_SUCCESS : CONVERT_MEMORY_ERROR;
}

/*
 * Escape string for YAML output
 */
char *
yaml_escape(const char *str)
{
	char *result;
	char *p, *q;
	size_t len;
	
	if (str == NULL)
		return (strdup("\"\""));

	/* Calculate needed space: open quote + body + close quote + NUL. */
	len = strlen(str) + 3;

	/* Check for characters that need escaping */
	for (p = (char *)str; *p; p++) {
		if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t')
			len++;
	}

	result = malloc(len);
	if (result == NULL)
		return (NULL);

	/* Add quotes and escape special chars */
	*result = '"';
	q = result + 1;
	for (p = (char *)str; *p; p++) {
		switch (*p) {
		case '"':
		case '\\':
			*q++ = '\\';
			*q++ = *p;
			break;
		case '\n':
			*q++ = '\\';
			*q++ = 'n';
			break;
		case '\r':
			*q++ = '\\';
			*q++ = 'r';
			break;
		case '\t':
			*q++ = '\\';
			*q++ = 't';
			break;
		default:
			*q++ = *p;
		}
	}
	*q++ = '"';
	*q = '\0';
	
	return (result);
}

/*
 * Escape string for JSON output
 */
char *
json_escape(const char *str)
{
	char *result;
	char *p, *q;
	size_t len;
	
	if (str == NULL)
		return (strdup("\"\""));

	/*
	 * Calculate needed space: open quote + body + close quote + NUL.
	 * A control character other than \n \r \t expands to "\uXXXX" (6
	 * bytes), so it needs 5 extra beyond its own byte.
	 */
	len = strlen(str) + 3;

	/* Check for characters that need escaping */
	for (p = (char *)str; *p; p++) {
		unsigned char c = (unsigned char)*p;

		if (c == '"' || c == '\\' || c == '\n' || c == '\r' ||
		    c == '\t')
			len++;
		else if (c < 32)
			len += 5;
	}

	result = malloc(len);
	if (result == NULL)
		return (NULL);

	/* Add quotes and escape special chars */
	*result = '"';
	q = result + 1;
	for (p = (char *)str; *p; p++) {
		unsigned char c = (unsigned char)*p;

		switch (c) {
		case '"':
		case '\\':
			*q++ = '\\';
			*q++ = c;
			break;
		case '\n':
			*q++ = '\\';
			*q++ = 'n';
			break;
		case '\r':
			*q++ = '\\';
			*q++ = 'r';
			break;
		case '\t':
			*q++ = '\\';
			*q++ = 't';
			break;
		default:
			if (c < 32) {
				/* Control character - escape as unicode */
				sprintf(q, "\\u%04x", c);
				q += 6;
			} else {
				*q++ = c;
			}
		}
	}
	*q++ = '"';
	*q = '\0';
	
	return (result);
}

/*
 * Native format output helpers
 */
char *
native_format_service(const char *name, const char *image,
    int replicas, const char *ports, const char *volumes,
    const char *environment, const char *networks,
    const char *depends_on, struct convert_options *opts)
{
	char *result;
	
	asprintf(&result,
	    "  - name: %s\n"
	    "    image: %s\n"
	    "    replicas: %d\n"
	    "%s%s%s%s%s%s",
	    name ? name : "unnamed",
	    image ? image : "nginx:latest",
	    replicas,
	    ports ? "    ports:\n" : "",
	    ports ? "      - " : "",
	    ports ? ports : "",
	    ports ? "\n" : "",
	    volumes ? "    volumes:\n      - " : "",
	    volumes ? volumes : "");
	
	return (result);
}

char *
native_format_network(const char *name, const char *driver,
    const char *subnet, struct convert_options *opts)
{
	char *result;
	
	asprintf(&result,
	    "  - name: %s\n"
	    "    driver: %s\n"
	    "%s%s%s\n",
	    name ? name : "default",
	    driver ? driver : "bridge",
	    subnet ? "    subnet: " : "",
	    subnet ? subnet : "",
	    subnet ? "\n" : "");
	
	return (result);
}

char *
native_format_volume(const char *name, const char *driver,
    const char *opts, struct convert_options *copts)
{
	char *result;
	
	asprintf(&result,
	    "  - name: %s\n"
	    "    driver: %s\n",
	    name ? name : "unnamed",
	    driver ? driver : "zfs");
	
	return (result);
}

char *
native_format_stack(const char *name, const char *services,
    const char *networks, const char *volumes,
    struct convert_options *opts)
{
	char *result;
	
	asprintf(&result,
	    "name: %s\n"
	    "namespace: default\n"
	    "%s%s%s",
	    name ? name : "unnamed",
	    services ? "services:\n" : "",
	    services ? services : "",
	    networks ? "\nnetworks:\n" : "");
	
	return (result);
}
