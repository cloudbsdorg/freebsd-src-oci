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
 * Ensemble YAML/JSON to Native format converter
 */

#include <sys/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "convert.h"

/*
 * Ensemble resource types
 */
typedef enum {
	ENSEMBLE_DEPLOYMENT = 0,
	ENSEMBLE_SERVICE,
	ENSEMBLE_CONFIGMAP,
	ENSEMBLE_SECRET,
	ENSEMBLE_INGRESS,
	ENSEMBLE_PVC,
	ENSEMBLE_NAMESPACE,
	ENSEMBLE_POD,
	ENSEMBLE_STATEFULSET,
	ENSEMBLE_DAEMONSET,
	ENSEMBLE_JOB,
	ENSEMBLE_CRONJOB,
	ENSEMBLE_UNKNOWN
} ensemble_kind_t;

/*
 * Detect Ensemble resource kind
 */
static ensemble_kind_t
ensemble_detect_kind(const char *yaml)
{
	if (strstr(yaml, "kind:") == NULL)
		return (ENSEMBLE_UNKNOWN);

	/* Extract kind value */
	const char *p = strstr(yaml, "kind:");
	if (p == NULL)
		return (ENSEMBLE_UNKNOWN);

	p += 5;
	while (isspace((unsigned char)*p))
		p++;
	/* Skip an opening quote so kind: "Pod" is matched like kind: Pod. */
	if (*p == '"' || *p == '\'')
		p++;

	/*
	 * Extract the kind token (up to whitespace, comment, or quote) and
	 * match it exactly. A prefix match would misclassify "ServiceAccount"
	 * as Service and "PodTemplate" as Pod.
	 */
	{
		const char *e = p;
		size_t klen;
		static const struct {
			const char	*name;
			ensemble_kind_t	 kind;
		} kinds[] = {
			{ "Deployment", ENSEMBLE_DEPLOYMENT },
			{ "Service", ENSEMBLE_SERVICE },
			{ "ConfigMap", ENSEMBLE_CONFIGMAP },
			{ "Secret", ENSEMBLE_SECRET },
			{ "Ingress", ENSEMBLE_INGRESS },
			{ "PersistentVolumeClaim", ENSEMBLE_PVC },
			{ "Namespace", ENSEMBLE_NAMESPACE },
			{ "Pod", ENSEMBLE_POD },
			{ "StatefulSet", ENSEMBLE_STATEFULSET },
			{ "DaemonSet", ENSEMBLE_DAEMONSET },
			{ "Job", ENSEMBLE_JOB },
			{ "CronJob", ENSEMBLE_CRONJOB },
		};
		size_t i;

		while (*e != '\0' && !isspace((unsigned char)*e) &&
		    *e != '#' && *e != '"' && *e != '\'')
			e++;
		klen = (size_t)(e - p);

		for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
			if (strlen(kinds[i].name) == klen &&
			    strncmp(p, kinds[i].name, klen) == 0)
				return (kinds[i].kind);
		}
	}

	return (ENSEMBLE_UNKNOWN);
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
	while (isspace((unsigned char)*p))
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
		while (end > p && isspace((unsigned char)end[-1]))
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
 * Resolve the effective namespace for a converted object: the object's own
 * namespace if present, otherwise the option default, otherwise "default".
 * Guards against a NULL opts->namespace (e.g. a zero-initialized options
 * struct from an API caller), which would otherwise be passed to a "%s".
 */
static const char *
ensemble_resolve_namespace(const char *namespace, struct convert_options *opts)
{
	if (namespace != NULL)
		return (namespace);
	if (opts != NULL && opts->namespace != NULL)
		return (opts->namespace);
	return ("default");
}

/*
 * Convert Ensemble Deployment to native format
 */
int
ensemble_convert_deployment(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	char *app = yaml_get_field(yaml, "app");
	char *image = yaml_get_field(yaml, "image");
	char *replicas_str = yaml_get_field(yaml, "replicas");
	char *container_port_str = yaml_get_field(yaml, "containerPort");

	int replicas = replicas_str ? atoi(replicas_str) : 1;
	const char *svc_name = name ? name : (app ? app : "unknown");
	char portbuf[160] = "";

	/*
	 * Emit only fields the manifest actually declares — do not invent a
	 * default port or image. A ports block is written only when the
	 * Deployment names a containerPort; a missing image is flagged rather
	 * than replaced with a plausible-but-wrong value.
	 */
	if (container_port_str != NULL)
		snprintf(portbuf, sizeof(portbuf),
		    "    ports:\n"
		    "      - container: %s\n"
		    "        protocol: tcp\n",
		    container_port_str);
	if (image == NULL)
		fprintf(stderr, "warning: Deployment '%s': no container image "
		    "found; set 'image' in the native config\n", svc_name);

	/* Build simplified output */
	char *result;
	asprintf(&result,
	    "# Converted from Ensemble Deployment\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "services:\n"
	    "  - name: %s\n"
	    "    image: %s\n"
	    "    replicas: %d\n"
	    "%s",
	    namespace ? namespace : "default",
	    svc_name,
	    svc_name,
	    namespace ? namespace : opts->namespace,
	    svc_name,
	    image ? image : "# TODO: set image",
	    replicas,
	    portbuf);

	free(name);
	free(namespace);
	free(app);
	free(image);
	free(replicas_str);
	free(container_port_str);

	if (result == NULL)
		return (CONVERT_MEMORY_ERROR);
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert Ensemble Service to native format
 */
int
ensemble_convert_service(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	char *selector = yaml_get_field(yaml, "selector");
	char *port_name = yaml_get_field(yaml, "port");
	char *target_port_str = yaml_get_field(yaml, "targetPort");
	char *type = yaml_get_field(yaml, "type");

	const char *svc_name = name ? name : "unknown";
	char selbuf[160] = "";
	char portbuf[200] = "";
	char *result;

	/*
	 * Emit only what the Service declares — no invented selector, port
	 * name, or port number. A Service maps a published port to a target
	 * (container) port; emit the target port when present.
	 */
	if (selector != NULL)
		snprintf(selbuf, sizeof(selbuf), "    selector: %s\n", selector);
	if (target_port_str != NULL || port_name != NULL)
		snprintf(portbuf, sizeof(portbuf),
		    "    ports:\n"
		    "      - container: %s\n"
		    "        protocol: tcp\n",
		    target_port_str ? target_port_str : port_name);

	asprintf(&result,
	    "# Converted from Ensemble Service\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "# Service definition (referenced by deployment)\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "services:\n"
	    "  - name: %s\n"
	    "%s%s",
	    namespace ? namespace : "default",
	    svc_name,
	    svc_name,
	    namespace ? namespace : opts->namespace,
	    svc_name,
	    selbuf,
	    portbuf);

	free(name);
	free(namespace);
	free(selector);
	free(port_name);
	free(target_port_str);
	free(type);

	if (result == NULL)
		return (CONVERT_MEMORY_ERROR);
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert Ensemble ConfigMap to native format
 */
int
ensemble_convert_configmap(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	const char *ns = ensemble_resolve_namespace(namespace, opts);

	if (strstr(yaml, "data:") != NULL)
		fprintf(stderr, "warning: ConfigMap '%s': 'data' was not "
		    "converted; add the key/value pairs to the native config\n",
		    name ? name : "unknown");

	char *result;
	asprintf(&result,
	    "# Converted from Ensemble ConfigMap\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "configs:\n"
	    "  # Note: Add config data below\n"
	    "  # data:\n"
	    "  #   key: value\n",
	    ns,
	    name ? name : "unknown",
	    name ? name : "unknown",
	    ns);

	free(name);
	free(namespace);

	if (result == NULL)
		return (CONVERT_MEMORY_ERROR);
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert Ensemble Secret to native format
 */
int
ensemble_convert_secret(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	const char *ns = ensemble_resolve_namespace(namespace, opts);

	if (strstr(yaml, "data:") != NULL)
		fprintf(stderr, "warning: Secret '%s': 'data' was not "
		    "converted; add the base64-encoded values to the native "
		    "config\n", name ? name : "unknown");

	char *result;
	asprintf(&result,
	    "# Converted from Ensemble Secret\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "secrets:\n"
	    "  # Note: Secret data must be base64 encoded\n"
	    "  # type: opaque | ensemble.io/tls | etc.\n"
	    "  # data:\n"
	    "  #   key: <base64-encoded-value>\n",
	    ns,
	    name ? name : "unknown",
	    name ? name : "unknown",
	    ns);

	free(name);
	free(namespace);

	if (result == NULL)
		return (CONVERT_MEMORY_ERROR);
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert Ensemble Ingress to native format
 */
int
ensemble_convert_ingress(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	const char *ns = ensemble_resolve_namespace(namespace, opts);

	if (strstr(yaml, "rules:") != NULL || strstr(yaml, "host:") != NULL)
		fprintf(stderr, "warning: Ingress '%s': routing rules were not "
		    "converted; add host/path routes to the network config\n",
		    name ? name : "unknown");

	char *result;
	asprintf(&result,
	    "# Converted from Ensemble Ingress\n"
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
	    ns,
	    name ? name : "unknown",
	    name ? name : "unknown",
	    ns,
	    name ? name : "unknown");

	free(name);
	free(namespace);

	if (result == NULL)
		return (CONVERT_MEMORY_ERROR);
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert Ensemble PVC to native format
 */
int
ensemble_convert_persistentvolumeclaim(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");
	char *namespace = yaml_get_field(yaml, "namespace");
	char *storage_str = yaml_get_field(yaml, "storage");
	const char *ns = ensemble_resolve_namespace(namespace, opts);
	const char *vol_name = name ? name : "data";
	char sizebuf[128];

	/*
	 * Emit the declared storage request. If the claim omits it, do not
	 * invent a size — leave a TODO the operator must fill in, and say so.
	 */
	if (storage_str != NULL) {
		snprintf(sizebuf, sizeof(sizebuf), "    size: %s\n", storage_str);
	} else {
		fprintf(stderr, "warning: PersistentVolumeClaim '%s': no "
		    "storage size declared; set 'size:' in the native config\n",
		    name ? name : "unknown");
		snprintf(sizebuf, sizeof(sizebuf),
		    "    # TODO: set size (no storage request in source)\n");
	}

	char *result;
	asprintf(&result,
	    "# Converted from Ensemble PersistentVolumeClaim\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "volumes:\n"
	    "  - name: %s\n"
	    "    driver: zfs\n"
	    "%s"
	    "    # access_mode: ReadWriteOnce | ReadOnlyMany | ReadWriteMany\n"
	    "    access_mode: ReadWriteOnce\n",
	    ns,
	    name ? name : "unknown",
	    name ? name : "unknown",
	    ns,
	    vol_name,
	    sizebuf);

	free(name);
	free(namespace);
	free(storage_str);

	if (result == NULL)
		return (CONVERT_MEMORY_ERROR);
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert Ensemble Namespace to native format
 */
int
ensemble_convert_namespace(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *name = yaml_get_field(yaml, "name");

	char *result;
	asprintf(&result,
	    "# Converted from Ensemble Namespace\n"
	    "\n"
	    "# Namespace definition\n"
	    "name: %s\n",
	    name ? name : "default");

	free(name);

	if (result == NULL)
		return (CONVERT_MEMORY_ERROR);
	*output = result;
	return (CONVERT_SUCCESS);
}

/*
 * Convert multi-document Ensemble YAML
 */
int
ensemble_convert_multi(const char *yaml, char **output,
    struct convert_options *opts)
{
	char *result = NULL;
	size_t result_cap = 0;
	size_t result_len = 0;
	const char *p;
	int docs = 0;

	/* Add header */
	asprintf(&result,
	    "# Native OCI FreeBSD Configuration\n"
	    "# Converted from Ensemble YAML\n"
	    "# Generated by ocifbsd_convert\n"
	    "\n"
	    "version: \"1.0\"\n"
	    "\n");
	result_len = strlen(result);
	result_cap = result_len + 1;

	/*
	 * Split into documents on the "---" separator. Documents may appear
	 * before the first separator, between separators, and after the last
	 * separator; every non-empty segment must be converted (a naive
	 * "find --- then process what follows" loop drops both the leading
	 * and trailing documents).
	 */
	p = yaml;
	while (*p != '\0') {
		char *doc;
		size_t doc_len;
		const char *doc_start;
		const char *doc_end;
		ensemble_kind_t kind;
		char *converted = NULL;
		int ret;

		/* Skip a leading separator and any surrounding whitespace. */
		if (strncmp(p, "---", 3) == 0)
			p += 3;
		while (*p != '\0' && isspace((unsigned char)*p))
			p++;
		if (*p == '\0')
			break;

		doc_start = p;
		doc_end = strstr(doc_start, "---");
		if (doc_end == NULL)
			doc_end = doc_start + strlen(doc_start);

		doc_len = doc_end - doc_start;
		doc = malloc(doc_len + 1);
		if (doc == NULL)
			continue;

		memcpy(doc, doc_start, doc_len);
		doc[doc_len] = '\0';

		kind = ensemble_detect_kind(doc);

		switch (kind) {
		case ENSEMBLE_DEPLOYMENT:
			ret = ensemble_convert_deployment(doc, &converted, opts);
			break;
		case ENSEMBLE_SERVICE:
			ret = ensemble_convert_service(doc, &converted, opts);
			break;
		case ENSEMBLE_CONFIGMAP:
			ret = ensemble_convert_configmap(doc, &converted, opts);
			break;
		case ENSEMBLE_SECRET:
			ret = ensemble_convert_secret(doc, &converted, opts);
			break;
		case ENSEMBLE_INGRESS:
			ret = ensemble_convert_ingress(doc, &converted, opts);
			break;
		case ENSEMBLE_PVC:
			ret = ensemble_convert_persistentvolumeclaim(doc, &converted, opts);
			break;
		case ENSEMBLE_NAMESPACE:
			ret = ensemble_convert_namespace(doc, &converted, opts);
			break;
		default:
			asprintf(&converted, "# Skipped unknown kind\n");
			ret = CONVERT_SUCCESS;
		}

		if (ret == CONVERT_SUCCESS && converted != NULL) {
			/*
			 * Separate each converted object with a YAML document
			 * marker. Each per-kind converter emits a self-contained
			 * config (its own name:/services:), so without "---" the
			 * concatenation would repeat top-level keys and a parser
			 * would keep only the last object. As documents, all are
			 * preserved.
			 */
			const char *sep = (docs > 0) ? "---\n" : "";
			size_t seplen = strlen(sep);
			size_t clen = strlen(converted);
			size_t needed = result_len + seplen + clen + 64;
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
			if (seplen > 0) {
				memcpy(result + result_len, sep, seplen);
				result_len += seplen;
			}
			memcpy(result + result_len, converted, clen);
			result_len += clen;
			result[result_len] = '\0';
			docs++;
		}

		free(converted);
		free(doc);
		/* Advance to the separator (or end); the top of the loop
		 * consumes the "---" and leading whitespace. */
		p = doc_end;
	}

	if (result == NULL)
		return (CONVERT_MEMORY_ERROR);
	*output = result;
	return (CONVERT_SUCCESS);
}
