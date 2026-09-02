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
 * Ensemble to Native format converter
 */

#include <sys/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "convert.h"

/*
 * Detect Ensemble version
 */
static int
ensemble_services_detect_version(const char *compose)
{
	/* Look for version key */
	const char *p = strstr(compose, "version:");
	if (p == NULL) {
		/* Default to v3 */
		return (3);
	}
	
	p += 8;
	while (isspace(*p))
		p++;
	
	/* Check for quoted version */
	if (*p == '"' || *p == '\'')
		p++;
	
	if (p[0] == '2' && (p[1] == '\0' || p[1] == '.'))
		return (2);
	if (p[0] == '3' && (p[1] == '\0' || p[1] == '.'))
		return (3);
	
	return (3);  /* Default to v3 */
}

/*
 * Extract service name from compose file
 */
static char **
ensemble_services_get_services(const char *compose, int *count)
{
	char **services = NULL;
	int services_cap = 0;
	*count = 0;
	
	const char *p = compose;
	
	if ((p = strstr(p, "services:")) != NULL) {
		const char *nl = strchr(p, '\n');
		int svc_indent = -1;

		/* Start scanning at the line after "services:". */
		const char *line = (nl != NULL) ? nl + 1 : p + strlen(p);

		/*
		 * The services block runs until the first non-blank line at
		 * indentation 0 (the next top-level key) or end of input.
		 * Service names sit at the first indentation level inside the
		 * block; more-deeply-indented lines (image:, ports:, ...) are
		 * service properties and must not be picked up.
		 */
		while (*line != '\0') {
			const char *q = line;
			int indent = 0;
			const char *line_end;

			while (*q == ' ' || *q == '\t') {
				indent++;
				q++;
			}
			line_end = strchr(line, '\n');

			if (*q == '\n' || *q == '\0' || *q == '#') {
				/* blank or comment line: skip */
			} else if (indent == 0) {
				break;			/* next top-level key */
			} else {
				if (svc_indent < 0)
					svc_indent = indent;
				if (indent == svc_indent && isalpha((unsigned char)*q)) {
					const char *ne = q;
					while (isalnum((unsigned char)*ne) ||
					    *ne == '_' || *ne == '-' || *ne == '.')
						ne++;
					if (ne > q && *ne == ':') {
						size_t name_len = (size_t)(ne - q);
						char *name = malloc(name_len + 1);
						if (name != NULL) {
							memcpy(name, q, name_len);
							name[name_len] = '\0';
							if (*count >= services_cap) {
								char **grown;
								services_cap = services_cap ?
								    services_cap * 2 : 16;
								grown = realloc(services,
								    services_cap * sizeof(char *));
								if (grown == NULL) {
									free(name);
									return (services);
								}
								services = grown;
							}
							services[*count] = name;
							(*count)++;
						}
					}
				}
			}

			if (line_end == NULL)
				break;
			line = line_end + 1;
		}
	}
	
	return (services);
}

/*
 * Extract compose value for a service
 */
static char *
ensemble_services_get_value(const char *service_block, const char *key)
{
	const char *p;
	char *result = NULL;
	size_t keylen = strlen(key);

	/*
	 * Find "<indent>key:" at the start of some line within the block. YAML
	 * service properties are indented, so match the key after any leading
	 * whitespace rather than only at column 0. Returns the first match,
	 * which for a well-formed block is this service's value for the key.
	 */
	p = NULL;
	{
		const char *line = service_block;
		/* allow a match on the very first line of the block too */
		for (;;) {
			const char *q = line;
			while (*q == ' ' || *q == '\t')
				q++;
			if (strncmp(q, key, keylen) == 0 && q[keylen] == ':') {
				p = q + keylen + 1;
				break;
			}
			line = strchr(line, '\n');
			if (line == NULL)
				break;
			line++;
		}
	}

	if (p == NULL)
		return (NULL);

	while (*p == ' ' || *p == '\t')
		p++;
	
	/* Check for inline array/list */
	if (*p == '[') {
		/* JSON-style array - collect until matching ] */
		const char *start = p;
		int depth = 1;
		p++;
		while (*p && depth > 0) {
			if (*p == '[') depth++;
			else if (*p == ']') depth--;
			p++;
		}
		result = malloc(p - start + 1);
		if (result != NULL) {
			memcpy(result, start, p - start);
			result[p - start] = '\0';
		}
		return (result);
	}
	
	/* Check for block array (lines starting with -) */
	if (*p == '\n') {
		/* Look for first item */
		p++;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '-') {
			const char *start = p + 1;
			while (*start == ' ' || *start == '\t') start++;
			const char *end = strchr(start, '\n');
			if (end == NULL) end = start + strlen(start);
			while (end > start && isspace(end[-1])) end--;
			result = malloc(end - start + 1);
			if (result != NULL) {
				memcpy(result, start, end - start);
				result[end - start] = '\0';
			}
			return (result);
		}
	}
	
	/* Single value - until end of line */
	const char *end = strchr(p, '\n');
	if (end == NULL)
		end = p + strlen(p);
	
	/* Trim */
	while (end > p && isspace(end[-1]))
		end--;
	
	result = malloc(end - p + 1);
	if (result != NULL) {
		memcpy(result, p, end - p);
		result[end - p] = '\0';
	}
	
	return (result);
}

/*
 * Locate a service's block: the line "<indent><name>:" (indented under the
 * services: map), and return a malloc'd copy of just that service's block —
 * from its name line down to (but not including) the next non-blank line at the
 * same or lower indentation (the next service, or a top-level key such as
 * volumes:/networks:). Bounding the block is essential: without it a lookup for
 * "ports"/"volumes" on a service that omits them would match a *later*
 * service's values and bleed fields across services. Caller frees the result.
 * Returns NULL if the service is not found or on allocation failure.
 */
static char *
ensemble_services_find_block(const char *compose, const char *name)
{
	char needle[128];
	size_t nlen;
	const char *sp = compose;

	nlen = (size_t)snprintf(needle, sizeof(needle), "%s:", name);
	while ((sp = strstr(sp, needle)) != NULL) {
		const char *bol = sp;
		const char *w, *end;
		size_t indent, len;
		char *block;

		while (bol > compose && bol[-1] != '\n')
			bol--;
		/* Accept only when just indentation precedes the name. */
		w = bol;
		while (w < sp && (*w == ' ' || *w == '\t'))
			w++;
		if (w != sp) {
			sp += nlen;
			continue;
		}
		indent = (size_t)(sp - bol);	/* this service's indent depth */

		/* Advance past the name line, then to the block boundary. */
		end = strchr(sp, '\n');
		if (end == NULL)
			return (strdup(bol));
		end++;
		while (*end != '\0') {
			const char *line = end;
			size_t li = 0;

			while (line[li] == ' ' || line[li] == '\t')
				li++;
			/* A non-blank line at <= this indent ends the block. */
			if (line[li] != '\0' && line[li] != '\n' &&
			    li <= indent)
				break;
			end = strchr(line, '\n');
			if (end == NULL) {
				end = line + strlen(line);
				break;
			}
			end++;
		}

		len = (size_t)(end - bol);
		block = malloc(len + 1);
		if (block == NULL)
			return (NULL);
		memcpy(block, bol, len);
		block[len] = '\0';
		return (block);
	}
	return (NULL);
}

/*
 * Convert Ensemble to native format
 */
int
ensemble_services_convert(const char *compose, char **output,
    struct convert_options *opts)
{
	int version = ensemble_services_detect_version(compose);
	
	switch (version) {
	case 2:
		return ensemble_services_convert_v2(compose, output, opts);
	case 3:
	default:
		return ensemble_services_convert_v3(compose, output, opts);
	}
}

/*
 * Convert Ensemble v2
 */
int
ensemble_services_convert_v2(const char *compose, char **output,
    struct convert_options *opts)
{
	char **services;
	int count;
	char *result = NULL;
	
	services = ensemble_services_get_services(compose, &count);
	if (services == NULL || count == 0) {
		*output = strdup("# No services found in the stack file\n");
		return (CONVERT_SUCCESS);
	}
	
	/*
	 * Build output into a memory stream. Each service is appended in O(1)
	 * amortized; the previous asprintf("%s", result, ...) chain recopied the
	 * entire accumulated result every iteration, making the whole conversion
	 * O(n^2) in the output size.
	 */
	char *buf = NULL;
	size_t bufsz = 0;
	FILE *ms = open_memstream(&buf, &bufsz);

	if (ms == NULL) {
		for (int i = 0; i < count; i++)
			free(services[i]);
		free(services);
		return (CONVERT_MEMORY_ERROR);
	}

	fprintf(ms,
	    "# Native OCI FreeBSD Configuration\n"
	    "# Converted from Ensemble v2\n"
	    "# Generated by ocifbsd_convert\n"
	    "\n"
	    "version: \"1.0\"\n"
	    "\n"
	    "name: ensemble-stack\n"
	    "namespace: default\n"
	    "\n");

	for (int i = 0; i < count; i++) {
		char *service_block;
		char *image, *command, *ports, *volumes, *environment;
		char *networks, *depends_on;

		/* Extract service block (indented under services:) */
		service_block = ensemble_services_find_block(compose, services[i]);
		if (service_block == NULL)
			continue;

		/* Extract values */
		image = ensemble_services_get_value(service_block, "image");
		command = ensemble_services_get_value(service_block, "command");
		ports = ensemble_services_get_value(service_block, "ports");
		volumes = ensemble_services_get_value(service_block, "volumes");
		environment = ensemble_services_get_value(service_block, "environment");
		networks = ensemble_services_get_value(service_block, "networks");
		depends_on = ensemble_services_get_value(service_block, "depends_on");

		/* Warn about directives read but not translated (see v3). */
		if (environment != NULL)
			fprintf(stderr, "warning: service '%s': 'environment' "
			    "was not converted; set it in the native config\n",
			    services[i]);
		if (networks != NULL)
			fprintf(stderr, "warning: service '%s': 'networks' was "
			    "not converted; ocifbsd uses native VNET networking\n",
			    services[i]);
		if (depends_on != NULL)
			fprintf(stderr, "warning: service '%s': 'depends_on' "
			    "was not converted; start ordering is not modeled\n",
			    services[i]);

		if (i == 0)
			fprintf(ms, "services:\n");
		fprintf(ms,
		    "  - name: %s\n"
		    "    image: %s\n"
		    "%s%s%s%s%s%s%s%s\n",
		    services[i],
		    image ? image : services[i],
		    command ? "    command: " : "",
		    command ? command : "",
		    command ? "\n" : "",
		    ports ? "    ports:\n" : "",
		    ports ? "      - " : "",
		    ports ? ports : "",
		    ports ? "\n" : "",
		    volumes ? "    volumes:\n" : "");

		free(image);
		free(command);
		free(ports);
		free(volumes);
		free(environment);
		free(networks);
		free(depends_on);
		free(service_block);
	}

	/* Free services */
	for (int i = 0; i < count; i++)
		free(services[i]);
	free(services);

	fclose(ms);
	*output = buf;
	return (CONVERT_SUCCESS);
}

/*
 * Convert Ensemble v3
 */
int
ensemble_services_convert_v3(const char *compose, char **output,
    struct convert_options *opts)
{
	char **services;
	int count;

	services = ensemble_services_get_services(compose, &count);
	if (services == NULL || count == 0) {
		*output = strdup("# No services found in the stack file\n");
		return (CONVERT_SUCCESS);
	}

	/*
	 * Build output into a memory stream (O(1) amortized per append) rather
	 * than the old O(n^2) asprintf("%s", result, ...) prefix-recopy chain.
	 */
	char *buf = NULL;
	size_t bufsz = 0;
	FILE *ms = open_memstream(&buf, &bufsz);

	if (ms == NULL) {
		for (int i = 0; i < count; i++)
			free(services[i]);
		free(services);
		return (CONVERT_MEMORY_ERROR);
	}

	fprintf(ms,
	    "# Native OCI FreeBSD Configuration\n"
	    "# Converted from Ensemble v3\n"
	    "# Generated by ocifbsd_convert\n"
	    "\n"
	    "version: \"1.0\"\n"
	    "\n"
	    "name: ensemble-stack\n"
	    "namespace: default\n"
	    "\n");

	for (int i = 0; i < count; i++) {
		char *service_block;
		char *image, *command, *ports, *volumes, *environment;
		char *networks, *depends_on, *deploy_replicas;

		service_block = ensemble_services_find_block(compose, services[i]);
		if (service_block == NULL)
			continue;

		image = ensemble_services_get_value(service_block, "image");
		command = ensemble_services_get_value(service_block, "command");
		ports = ensemble_services_get_value(service_block, "ports");
		volumes = ensemble_services_get_value(service_block, "volumes");
		environment = ensemble_services_get_value(service_block, "environment");
		networks = ensemble_services_get_value(service_block, "networks");
		depends_on = ensemble_services_get_value(service_block, "depends_on");
		
		/* v3 deploy section */
		deploy_replicas = NULL;
		char *deploy = strstr(service_block, "deploy:");
		if (deploy != NULL) {
			deploy_replicas = ensemble_services_get_value(deploy, "replicas");
		}

		/*
		 * Warn about directives that are read but not translated, so the
		 * converted config is never silently incomplete. ocifbsd converts
		 * the core service shape (image, ports, volumes, replicas); the
		 * rest must be set in the native config by hand.
		 */
		if (environment != NULL)
			fprintf(stderr, "warning: service '%s': 'environment' "
			    "was not converted; set it in the native config\n",
			    services[i]);
		if (networks != NULL)
			fprintf(stderr, "warning: service '%s': 'networks' was "
			    "not converted; ocifbsd uses native VNET networking\n",
			    services[i]);
		if (depends_on != NULL)
			fprintf(stderr, "warning: service '%s': 'depends_on' "
			    "was not converted; start ordering is not modeled\n",
			    services[i]);

		int replicas = deploy_replicas ? atoi(deploy_replicas) : 1;

		if (i == 0)
			fprintf(ms, "services:\n");
		fprintf(ms,
		    "  - name: %s\n"
		    "    image: %s\n"
		    "    replicas: %d\n"
		    "%s%s%s%s%s%s%s%s%s%s\n",
		    services[i],
		    image ? image : services[i],
		    replicas,
		    command ? "    command: " : "",
		    command ? command : "",
		    command ? "\n" : "",
		    ports ? "    ports:\n" : "",
		    ports ? "      - " : "",
		    ports ? ports : "",
		    ports ? "\n" : "",
		    volumes ? "    volumes:\n" : "",
		    volumes ? "      - " : "",
		    volumes ? volumes : "");

		free(image);
		free(command);
		free(ports);
		free(volumes);
		free(environment);
		free(networks);
		free(depends_on);
		free(deploy_replicas);
		free(service_block);
	}

	/* Check for networks section */
	char *networks = strstr(compose, "networks:");
	if (networks != NULL) {
		const char *p = networks + 9;
		while (*p && *p != '\n') p++;
		if (*p == '\n')
			fprintf(ms, "\nnetworks:\n  - name: default\n"
			    "    driver: bridge\n");
	}

	/* Check for volumes section */
	char *volumes = strstr(compose, "volumes:");
	if (volumes != NULL) {
		const char *p = volumes + 8;
		while (*p && *p != '\n') p++;
		if (*p == '\n')
			fprintf(ms, "\nvolumes:\n"
			    "  # Note: Add volume definitions here\n");
	}

	for (int i = 0; i < count; i++)
		free(services[i]);
	free(services);

	fclose(ms);
	*output = buf;
	return (CONVERT_SUCCESS);
}
