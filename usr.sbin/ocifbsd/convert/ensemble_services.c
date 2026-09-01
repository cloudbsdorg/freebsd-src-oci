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
	
	while ((p = strstr(p, "services:")) != NULL) {
		p += 9;
		
		/* Find next top-level key or end */
		const char *end = p;
		while (*end && *end != '\n') {
			if (!isspace(*end) && *end != '#') {
				/* Check for top-level key */
				if (*end == 'x' && end[1] == ':') break;
				if (*end == 'v' && strncmp(end, "version:", 8) == 0) break;
				if (*end == 'n' && strncmp(end, "networks:", 9) == 0) break;
				if (*end == 'v' && strncmp(end, "volumes:", 8) == 0) break;
				if (*end == 's' && strncmp(end, "services:", 9) == 0) break;
			}
			end++;
		}
		
		/* Parse service names (lines that start with a word followed by colon at indent) */
		const char *line_start = p;
		while (line_start < end) {
			const char *line_end = strchr(line_start, '\n');
			if (line_end == NULL)
				line_end = end;
			
			/* Check if this looks like a service name */
			const char *name_start = line_start;
			while (isspace(*name_start) && name_start < line_end)
				name_start++;
			
			if (name_start < line_end && !isspace(*name_start) &&
			    *name_start != '#' && *name_start != '-' &&
			    isalpha(*name_start)) {
				const char *name_end = name_start;
				while (isalnum(*name_end) || *name_end == '_' || *name_end == '-')
					name_end++;
				
				if (name_end > name_start && *name_end == ':') {
					/* Found a service name */
					size_t name_len = name_end - name_start;
					char *name = malloc(name_len + 1);
					if (name != NULL) {
						memcpy(name, name_start, name_len);
						name[name_len] = '\0';
						
						if (*count >= services_cap) {
							services_cap = services_cap ? services_cap * 2 : 16;
							services = realloc(services,
							    services_cap * sizeof(char *));
						}
						if (services != NULL) {
							services[*count] = name;
							(*count)++;
						} else {
							free(name);
						}
					}
				}
			}
			
			line_start = line_end + 1;
		}
		
		break;  /* Only process first "services:" block */
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
	char search[64];
	char *result = NULL;
	
	snprintf(search, sizeof(search), "\n%s:", key);
	
	p = strstr(service_block, search);
	if (p == NULL) {
		/* Try at start of block */
		snprintf(search, sizeof(search), "%s:", key);
		if (strncmp(service_block, search, strlen(search)) == 0)
			p = service_block;
	}
	
	if (p == NULL)
		return (NULL);
	
	p += strlen(search);
	while (isspace(*p))
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
		*output = strdup("# No services found in compose file\n");
		return (CONVERT_SUCCESS);
	}
	
	/* Build output */
	asprintf(&result,
	    "# Native OCI FreeBSD Configuration\n"
	    "# Converted from Ensemble v2\n"
	    "# Generated by ocifbsd-convert\n"
	    "\n"
	    "version: \"1.0\"\n"
	    "\n"
	    "name: compose-project\n"
	    "namespace: default\n"
	    "\n");
	
	for (int i = 0; i < count; i++) {
		char *service_block;
		char *image, *command, *ports, *volumes, *environment;
		char *networks, *depends_on;
		char *entry;
		
		/* Extract service block */
		asprintf(&entry, "\n%s:", services[i]);
		service_block = strstr(compose, entry);
		free(entry);
		
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
		
		char *new_result;
		asprintf(&new_result,
		    "%sservices:\n"
		    "  - name: %s\n"
		    "    image: %s\n"
		    "%s%s%s%s%s%s%s%s\n",
		    result,
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
		free(result);
		result = new_result;
		
		free(image);
		free(command);
		free(ports);
		free(volumes);
		free(environment);
		free(networks);
		free(depends_on);
	}
	
	/* Free services */
	for (int i = 0; i < count; i++)
		free(services[i]);
	free(services);
	
	*output = result;
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
	char *result = NULL;
	
	services = ensemble_services_get_services(compose, &count);
	if (services == NULL || count == 0) {
		*output = strdup("# No services found in compose file\n");
		return (CONVERT_SUCCESS);
	}
	
	/* Build output with v3-specific features */
	asprintf(&result,
	    "# Native OCI FreeBSD Configuration\n"
	    "# Converted from Ensemble v3\n"
	    "# Generated by ocifbsd-convert\n"
	    "\n"
	    "version: \"1.0\"\n"
	    "\n"
	    "name: compose-project\n"
	    "namespace: default\n"
	    "\n");
	
	for (int i = 0; i < count; i++) {
		char *service_block;
		char *image, *command, *ports, *volumes, *environment;
		char *networks, *depends_on, *deploy_replicas;
		char *entry;
		
		asprintf(&entry, "\n%s:", services[i]);
		service_block = strstr(compose, entry);
		free(entry);
		
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
		
		int replicas = deploy_replicas ? atoi(deploy_replicas) : 1;
		
		char *new_result;
		asprintf(&new_result,
		    "%sservices:\n"
		    "  - name: %s\n"
		    "    image: %s\n"
		    "    replicas: %d\n"
		    "%s%s%s%s%s%s%s%s%s%s\n",
		    result,
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
		free(result);
		result = new_result;
		
		free(image);
		free(command);
		free(ports);
		free(volumes);
		free(environment);
		free(networks);
		free(depends_on);
		free(deploy_replicas);
	}
	
	/* Check for networks section */
	char *networks = strstr(compose, "networks:");
	if (networks != NULL) {
		const char *p = networks + 9;
		while (*p && *p != '\n') p++;
		if (*p == '\n') {
			char *net_block = strndup(networks, p - networks);
			if (net_block != NULL) {
				char *new_result;
				asprintf(&new_result, "%s\nnetworks:\n  - name: default\n    driver: bridge\n",
				    result);
				free(result);
				result = new_result;
				free(net_block);
			}
		}
	}
	
	/* Check for volumes section */
	char *volumes = strstr(compose, "volumes:");
	if (volumes != NULL) {
		const char *p = volumes + 8;
		while (*p && *p != '\n') p++;
		if (*p == '\n') {
			char *new_result;
			asprintf(&new_result, "%s\nvolumes:\n  # Note: Add volume definitions here\n",
			    result);
			free(result);
			result = new_result;
		}
	}
	
	for (int i = 0; i < count; i++)
		free(services[i]);
	free(services);
	
	*output = result;
	return (CONVERT_SUCCESS);
}
