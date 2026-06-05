/*-
 * Copyright (c) 2024 The FreeBSD Foundation
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
 * Kubernetes YAML/JSON to Native format converter
 */

#include <sys/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "convert.h"

/*
 * K8s resource types
 */
typedef enum {
	K8S_DEPLOYMENT = 0,
	K8S_SERVICE,
	K8S_CONFIGMAP,
	K8S_SECRET,
	K8S_INGRESS,
	K8S_PVC,
	K8S_NAMESPACE,
	K8S_POD,
	K8S_STATEFULSET,
	K8S_DAEMONSET,
	K8S_JOB,
	K8S_CRONJOB,
	K8S_UNKNOWN
} k8s_kind_t;

/*
 * Detect K8s resource kind
 */
static k8s_kind_t
k8s_detect_kind(const char *yaml)
{
	if (strstr(yaml, "kind:") == NULL)
		return (K8S_UNKNOWN);
	
	/* Extract kind value */
	const char *p = strstr(yaml, "kind:");
	if (p == NULL)
		return (K8S_UNKNOWN);
	
	p += 5;
	while (isspace(*p))
		p++;
	
	/* Compare against known kinds */
	if (strncmp(p, "Deployment", 10) == 0)
		return (K8S_DEPLOYMENT);
	if (strncmp(p, "Service", 7) == 0)
		return (K8S_SERVICE);
	if (strncmp(p, "ConfigMap", 9) == 0)
		return (K8S_CONFIGMAP);
	if (strncmp(p, "Secret", 6) == 0)
		return (K8S_SECRET);
	if (strncmp(p, "Ingress", 7) == 0)
		return (K8S_INGRESS);
	if (strncmp(p, "PersistentVolumeClaim", 21) == 0)
		return (K8S_PVC);
	if (strncmp(p, "Namespace", 9) == 0)
		return (K8S_NAMESPACE);
	if (strncmp(p, "Pod", 3) == 0)
		return (K8S_POD);
	if (strncmp(p, "StatefulSet", 11) == 0)
		return (K8S_STATEFULSET);
	if (strncmp(p, "DaemonSet", 9) == 0)
		return (K8S_DAEMONSET);
	if (strncmp(p, "Job", 3) == 0)
		return (K8S_JOB);
	if (strncmp(p, "CronJob", 7) == 0)
		return (K8S_CRONJOB);
	
	return (K8S_UNKNOWN);
}

/*
 * Extract field from YAML (simple parser)
 */
static char *
yaml_get_field(const char *yaml, const char *field)
{
	char *result = NULL;
	const char *p;
	char search[64];
	size_t field_len;
	
	field_len = strlen(field);
	snprintf(search, sizeof(search), "%s:", field);
	
	p = strstr(yaml, search);
	if (p == NULL)
		return (NULL);
	
	p += field_len + 1;
	while (isspace(*p))
		p++;
	
	/* Check for quoted string */
	if (*p == '"' || *p == '\'') {
		char quote = *p++;
		const char *end = strchr(p, quote);
		if (end != NULL) {
			result = malloc(end - p + 1);
			if (result != NULL) {
				memcpy(result, p, end - p);
				result[end - p] = '\0';
			}
		}
	} else {
		/* Unquoted - until end of line */
		const char *end = p;
		while (*end && *end != '\n' && *end != '#')
			end++;
		
		/* Trim trailing whitespace */
		while (end > p && isspace(end[-1]))
			end--;
		
		if (end > p) {
			result = malloc(end - p + 1);
			if (result != NULL) {
				memcpy(result, p, end - p);
				result[end - p] = '\0';
			}
		}
	}
	
	return (result);
}

/*
 * Convert K8s Deployment to native format
 */
int
k8s_convert_deployment(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	char *app = yaml_get_field(yaml, "app");
	char *image = yaml_get_field(yaml, "image");
	char *replicas_str = yaml_get_field(yaml, "replicas");
	char *container_port_str = yaml_get_field(yaml, "containerPort");
	
	int replicas = replicas_str ? atoi(replicas_str) : 1;
	
	/* Build simplified output */
	char *result;
	asprintf(&result,
	    "# Converted from Kubernetes Deployment\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "services:\n"
	    "  - name: %s\n"
	    "    image: %s\n"
	    "    replicas: %d\n"
	    "    ports:\n"
	    "      - container: %s\n"
	    "        protocol: tcp\n",
	    namespace ? namespace : "default",
	    name ? name : (app ? app : "unknown"),
	    name ? name : (app ? app : "unknown"),
	    namespace ? namespace : opts->namespace,
	    name ? name : (app ? app : "unknown"),
	    image ? image : "nginx:latest",
	    replicas,
	    container_port_str ? container_port_str : "80");
	
	free(name);
	free(namespace);
	free(app);
	free(image);
	free(replicas_str);
	free(container_port_str);
	
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert K8s Service to native format
 */
int
k8s_convert_service(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	char *selector = yaml_get_field(yaml, "selector");
	char *port_name = yaml_get_field(yaml, "port");
	char *target_port_str = yaml_get_field(yaml, "targetPort");
	char *type = yaml_get_field(yaml, "type");
	
	char *result;
	asprintf(&result,
	    "# Converted from Kubernetes Service\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "# Service definition (referenced by deployment)\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "services:\n"
	    "  - name: %s-svc\n"
	    "    selector: %s\n"
	    "    ports:\n"
	    "      - name: %s\n"
	    "        container: %s\n"
	    "        protocol: tcp\n",
	    namespace ? namespace : "default",
	    name ? name : "unknown",
	    name ? name : "unknown",
	    namespace ? namespace : opts->namespace,
	    name ? name : "unknown",
	    selector ? selector : name ? name : "app",
	    port_name ? port_name : "http",
	    target_port_str ? target_port_str : "80");
	
	free(name);
	free(namespace);
	free(selector);
	free(port_name);
	free(target_port_str);
	free(type);
	
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert K8s ConfigMap to native format
 */
int
k8s_convert_configmap(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	
	char *result;
	asprintf(&result,
	    "# Converted from Kubernetes ConfigMap\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "configs:\n"
	    "  # Note: Add config data below\n"
	    "  # data:\n"
	    "  #   key: value\n",
	    namespace ? namespace : "default",
	    name ? name : "unknown",
	    name ? name : "unknown",
	    namespace ? namespace : opts->namespace);
	
	free(name);
	free(namespace);
	
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert K8s Secret to native format
 */
int
k8s_convert_secret(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	
	char *result;
	asprintf(&result,
	    "# Converted from Kubernetes Secret\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "secrets:\n"
	    "  # Note: Secret data must be base64 encoded\n"
	    "  # type: opaque | kubernetes.io/tls | etc.\n"
	    "  # data:\n"
	    "  #   key: <base64-encoded-value>\n",
	    namespace ? namespace : "default",
	    name ? name : "unknown",
	    name ? name : "unknown",
	    namespace ? namespace : opts->namespace);
	
	free(name);
	free(namespace);
	
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert K8s Ingress to native format
 */
int
k8s_convert_ingress(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	
	char *result;
	asprintf(&result,
	    "# Converted from Kubernetes Ingress\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "networks:\n"
	    "  - name: %s-net\n"
	    "    driver: bridge\n"
	    "    ingress: true\n"
	    "    # Configure ingress rules in network config\n"
	    "    # rules:\n"
	    "    #   - host: example.com\n"
	    "    #     paths:\n"
	    "    #       - path: /\n"
	    "    #         service: myservice\n"
	    "    #         port: 80\n",
	    namespace ? namespace : "default",
	    name ? name : "unknown",
	    name ? name : "unknown",
	    namespace ? namespace : opts->namespace,
	    name ? name : "unknown");
	
	free(name);
	free(namespace);
	
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert K8s PVC to native format
 */
int
k8s_convert_persistentvolumeclaim(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	char *storage_str = yaml_get_field(yaml, "storage");
	
	char *result;
	asprintf(&result,
	    "# Converted from Kubernetes PersistentVolumeClaim\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "volumes:\n"
	    "  - name: %s\n"
	    "    driver: zfs\n"
	    "    size: %s\n"
	    "    # access_mode: ReadWriteOnce | ReadOnlyMany | ReadWriteMany\n"
	    "    access_mode: ReadWriteOnce\n",
	    namespace ? namespace : "default",
	    name ? name : "unknown",
	    name ? name : "unknown",
	    namespace ? namespace : opts->namespace,
	    name ? name : "data",
	    storage_str ? storage_str : "1Gi");
	
	free(name);
	free(namespace);
	free(storage_str);
	
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert K8s Namespace to native format
 */
int
k8s_convert_namespace(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	
	char *result;
	asprintf(&result,
	    "# Converted from Kubernetes Namespace\n"
	    "\n"
	    "# Namespace definition\n"
	    "name: %s\n",
	    name ? name : "default");
	
	free(name);
	
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert multi-document K8s YAML
 */
int
k8s_convert_multi(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *result = NULL;
	size_t result_cap = 0;
	size_t result_len = 0;
	const char *doc_start;
	const char *p;
	
	/* Add header */
	asprintf(&result,
	    "# Native OCI FreeBSD Configuration\n"
	    "# Converted from Kubernetes YAML\n"
	    "# Generated by ocifbsd-convert\n"
	    "\n"
	    "version: \"1.0\"\n"
	    "\n");
	result_len = strlen(result);
	result_cap = result_len + 1;
	
	/* Split by document separator */
	p = yaml;
	while ((doc_start = strstr(p, "---")) != NULL) {
		char *doc;
		size_t doc_len;
		const char *doc_end;
		k8s_kind_t kind;
		char *converted = NULL;
		int ret;
		
		doc_start += 3;
		while (isspace(*doc_start))
			doc_start++;
		
		doc_end = strstr(doc_start, "---");
		if (doc_end == NULL)
			doc_end = doc_start + strlen(doc_start);
		
		doc_len = doc_end - doc_start;
		doc = malloc(doc_len + 1);
		if (doc == NULL)
			continue;
		
		memcpy(doc, doc_start, doc_len);
		doc[doc_len] = '\0';
		
		kind = k8s_detect_kind(doc);
		
		switch (kind) {
		case K8S_DEPLOYMENT:
			ret = k8s_convert_deployment(doc, &converted, opts);
			break;
		case K8S_SERVICE:
			ret = k8s_convert_service(doc, &converted, opts);
			break;
		case K8S_CONFIGMAP:
			ret = k8s_convert_configmap(doc, &converted, opts);
			break;
		case K8S_SECRET:
			ret = k8s_convert_secret(doc, &converted, opts);
			break;
		case K8S_INGRESS:
			ret = k8s_convert_ingress(doc, &converted, opts);
			break;
		case K8S_PVC:
			ret = k8s_convert_persistentvolumeclaim(doc, &converted, opts);
			break;
		case K8S_NAMESPACE:
			ret = k8s_convert_namespace(doc, &converted, opts);
			break;
		default:
			asprintf(&converted, "# Skipped unknown kind\n");
			ret = CONVERT_SUCCESS;
		}
		
		if (ret == CONVERT_SUCCESS && converted != NULL) {
			size_t clen = strlen(converted);
			size_t needed = result_len + clen + 64;
			if (needed > result_cap) {
				size_t new_cap = needed * 2;
				char *new_result = realloc(result, new_cap);
				if (new_result == NULL) {
					free(converted);
					free(doc);
					free(result);
					return (CONVERT_MEMORY_ERROR);
				}
				result = new_result;
				result_cap = new_cap;
			}
			memcpy(result + result_len, converted, clen);
			result_len += clen;
			result[result_len] = '\0';
		}
		
		free(converted);
		free(doc);
		p = doc_end + 3;
	}
	
	*output = result;
	return (CONVERT_SUCCESS);
}
