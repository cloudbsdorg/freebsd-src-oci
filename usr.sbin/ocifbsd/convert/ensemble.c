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
#include <stdbool.h>

#include "convert.h"
#include "yaml.h"

#ifndef nitems
#define	nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

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

/* Human-readable manifest kind, for diagnostics. */
static const char *
ensemble_kind_str(ensemble_kind_t k)
{

	switch (k) {
	case ENSEMBLE_DEPLOYMENT:	return ("Deployment");
	case ENSEMBLE_SERVICE:		return ("Service");
	case ENSEMBLE_CONFIGMAP:	return ("ConfigMap");
	case ENSEMBLE_SECRET:		return ("Secret");
	case ENSEMBLE_INGRESS:		return ("Ingress");
	case ENSEMBLE_PVC:		return ("PersistentVolumeClaim");
	case ENSEMBLE_NAMESPACE:	return ("Namespace");
	case ENSEMBLE_POD:		return ("Pod");
	case ENSEMBLE_STATEFULSET:	return ("StatefulSet");
	case ENSEMBLE_DAEMONSET:	return ("DaemonSet");
	case ENSEMBLE_JOB:		return ("Job");
	case ENSEMBLE_CRONJOB:		return ("CronJob");
	default:			return ("unknown kind");
	}
}

/*
 * Field extraction, backed by the real indent-aware parser (convert/yaml.c).
 *
 * The previous implementation scanned the raw document for the substring
 * "<key>:". That cannot say WHERE a key lives, so `metadata.name` and a
 * container's `name` were the same query and whichever appeared first in the
 * file won; it also matched a key inside a comment or inside a quoted value.
 *
 * ens_open() parses once per conversion and the lookups below address the
 * document by path. ens_field() remains for the handful of fields whose
 * location the manifest schema genuinely does not pin -- but it now matches a
 * whole key in the parsed tree rather than a substring of the file.
 */
struct ens_doc {
	struct yaml_node	*root;
};

static int
ens_open(struct ens_doc *doc, const char *yaml)
{

	doc->root = yaml_parse(yaml);
	if (doc->root == NULL) {
		fprintf(stderr, "error: manifest is not valid YAML "
		    "(or uses constructs this converter does not support)\n");
		return (-1);
	}
	return (0);
}

static void
ens_close(struct ens_doc *doc)
{

	yaml_free(doc->root);
	doc->root = NULL;
}

/* Value at an exact path, else NULL. Owned by the document. */
static const char *
ens_path(struct ens_doc *doc, const char *path)
{

	return (yaml_get_scalar(doc->root, path));
}

/*
 * Value at the first of several candidate paths that exists. Manifest kinds
 * put the same logical field in different places, and naming each candidate
 * explicitly is honest about that, where a document-wide substring search
 * merely hid it.
 */
static const char *
ens_first(struct ens_doc *doc, const char *const *paths, size_t n)
{
	size_t i;
	const char *v;

	for (i = 0; i < n; i++) {
		v = yaml_get_scalar(doc->root, paths[i]);
		if (v != NULL)
			return (v);
	}
	return (NULL);
}

/* First node anywhere with this key, for genuinely unpinned fields. */
static const char *
ens_field(struct ens_doc *doc, const char *key)
{

	return (yaml_find_scalar(doc->root, key));
}

/*
 * Read a field that must be an integer, enforcing its real range.
 *
 * A field that is supposed to be a number has to BE a number: the old code
 * ran the raw scalar through atoi(), which turns "abc", "", "3.5" and
 * "99999999999999" into a plausible-looking value and substitutes it
 * silently, so a manifest typo became "zero replicas" or a nonsense port
 * instead of a visible error. Absent means "use the default"; present but
 * malformed means the conversion fails.
 */
static int
ens_int(struct ens_doc *doc, const char *const *paths, size_t npaths,
    const char *what, long min, long max, long dfl, long *out)
{
	const char *raw;
	const char *why = NULL;

	raw = ens_first(doc, paths, npaths);
	if (raw == NULL) {
		*out = dfl;
		return (0);
	}
	if (yaml_parse_int(raw, min, max, out, &why) != 0) {
		fprintf(stderr, "error: %s: %s (got \"%s\"; expected an "
		    "integer between %ld and %ld)\n", what,
		    why != NULL ? why : "invalid value", raw, min, max);
		return (-1);
	}
	return (0);
}

/*
 * Is this a valid resource quantity?
 *
 * The accepted grammar is the one the manifests actually use: an optional
 * sign, digits with an optional decimal fraction, then an optional suffix --
 * binary (Ki, Mi, Gi, Ti, Pi, Ei) or decimal SI (m, k, M, G, T, P, E). This
 * validates the SHAPE against the real specification rather than against a
 * guess: rejecting "10Gi" or accepting "ten gigs" would both be wrong.
 */
static bool
quantity_is_valid(const char *s)
{
	static const char *const suffixes[] = {
		"", "m", "k", "M", "G", "T", "P", "E",
		"Ki", "Mi", "Gi", "Ti", "Pi", "Ei"
	};
	const char *p = s;
	bool digits = false;
	size_t i;

	if (s == NULL || *s == '\0')
		return (false);
	if (*p == '+' || *p == '-')
		p++;
	while (isdigit((unsigned char)*p)) {
		p++;
		digits = true;
	}
	if (*p == '.') {
		p++;
		while (isdigit((unsigned char)*p)) {
			p++;
			digits = true;
		}
	}
	if (!digits)
		return (false);
	for (i = 0; i < nitems(suffixes); i++)
		if (strcmp(p, suffixes[i]) == 0)
			return (true);
	return (false);
}

/* Quote a manifest value for safe emission into the generated document. */
#define	ENS_Q(buf, v)	yaml_quote((v), (buf), sizeof(buf))

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
	/*
	 * Address each field where the Deployment schema actually puts it. The
	 * old code asked the document for "name" and got whichever `name:`
	 * appeared first -- which for a Deployment whose metadata block came
	 * after its pod template was a CONTAINER's name, silently converting
	 * the wrong object.
	 */
	static const char *const name_paths[] = { "metadata.name", "name" };
	static const char *const ns_paths[] = {
		"metadata.namespace", "namespace"
	};
	static const char *const app_paths[] = {
		"metadata.labels.app", "spec.selector.matchLabels.app", "app"
	};
	static const char *const image_paths[] = {
		"spec.template.spec.containers.0.image", "image"
	};
	static const char *const replica_paths[] = {
		"spec.replicas", "replicas"
	};
	static const char *const port_paths[] = {
		"spec.template.spec.containers.0.ports.0.containerPort",
		"containerPort"
	};
	struct ens_doc doc;
	const char *name, *namespace, *app, *image, *svc_name;
	long replicas, port;
	char portbuf[160] = "";
	char qname[512], qns[512], qimage[1024], qorigns[512];
	const char *qsvc;
	char portnum[16];
	char *result;

	if (ens_open(&doc, yaml) != 0)
		return (CONVERT_SYNTAX_ERROR);

	name = ens_first(&doc, name_paths, nitems(name_paths));
	namespace = ens_first(&doc, ns_paths, nitems(ns_paths));
	app = ens_first(&doc, app_paths, nitems(app_paths));
	image = ens_first(&doc, image_paths, nitems(image_paths));

	/* Numbers must be numbers, and a port must be a legal port. */
	if (ens_int(&doc, replica_paths, nitems(replica_paths),
	    "Deployment spec.replicas", 0, 100000, 1, &replicas) != 0) {
		ens_close(&doc);
		return (CONVERT_VALIDATION_ERROR);
	}
	port = -1;
	if (ens_first(&doc, port_paths, nitems(port_paths)) != NULL &&
	    ens_int(&doc, port_paths, nitems(port_paths),
	    "Deployment containerPort", YAML_PORT_MIN, YAML_PORT_MAX, 0,
	    &port) != 0) {
		ens_close(&doc);
		return (CONVERT_VALIDATION_ERROR);
	}

	svc_name = name ? name : (app ? app : "unknown");
	qsvc = ENS_Q(qname, svc_name);

	/*
	 * Emit only fields the manifest actually declares -- do not invent a
	 * default port or image. Every value that came from the manifest is
	 * quoted on the way out, so a value containing a newline or a colon
	 * cannot inject structure into the document we generate.
	 */
	if (port > 0) {
		snprintf(portnum, sizeof(portnum), "%ld", port);
		snprintf(portbuf, sizeof(portbuf),
		    "    ports:\n"
		    "      - container: %s\n"
		    "        protocol: tcp\n",
		    portnum);
	}
	if (image == NULL)
		fprintf(stderr, "warning: Deployment '%s': no container image "
		    "found; set 'image' in the native config\n", svc_name);

	asprintf(&result,
	    "# Converted from Ensemble Deployment\n"
	    "# Original: %s/%s\n"
	    "\n"
	    "name: %s\n"
	    "namespace: %s\n"
	    "services:\n"
	    "  - name: %s\n"
	    "    image: %s\n"
	    "    replicas: %ld\n"
	    "%s",
	    ENS_Q(qorigns, namespace ? namespace : "default"),
	    qsvc,
	    qsvc, ENS_Q(qns, namespace ? namespace : opts->namespace), qsvc,
	    image ? ENS_Q(qimage, image) : "# TODO: set image",
	    replicas,
	    portbuf);

	ens_close(&doc);

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
	static const char *const name_paths[] = { "metadata.name", "name" };
	static const char *const ns_paths[] = {
		"metadata.namespace", "namespace"
	};
	static const char *const sel_paths[] = {
		"spec.selector.app", "spec.selector", "selector"
	};
	static const char *const tport_paths[] = {
		"spec.ports.0.targetPort", "targetPort"
	};
	static const char *const port_paths[] = {
		"spec.ports.0.port", "port"
	};
	struct ens_doc doc;
	const char *name, *namespace, *selector, *svc_name;
	long port;
	char selbuf[256] = "";
	char portbuf[200] = "";
	char qname[512], qns[512], qsel[512], qorigns[512], portnum[16];
	const char *qsvc;
	char *result;

	if (ens_open(&doc, yaml) != 0)
		return (CONVERT_SYNTAX_ERROR);

	name = ens_first(&doc, name_paths, nitems(name_paths));
	namespace = ens_first(&doc, ns_paths, nitems(ns_paths));
	selector = ens_first(&doc, sel_paths, nitems(sel_paths));

	/*
	 * A Service maps a published port to a target (container) port. Prefer
	 * targetPort, fall back to port, and require whichever is present to be
	 * a real port number rather than trusting it downstream.
	 */
	port = -1;
	if (ens_first(&doc, tport_paths, nitems(tport_paths)) != NULL) {
		if (ens_int(&doc, tport_paths, nitems(tport_paths),
		    "Service spec.ports[0].targetPort", YAML_PORT_MIN,
		    YAML_PORT_MAX, 0, &port) != 0) {
			ens_close(&doc);
			return (CONVERT_VALIDATION_ERROR);
		}
	} else if (ens_first(&doc, port_paths, nitems(port_paths)) != NULL) {
		if (ens_int(&doc, port_paths, nitems(port_paths),
		    "Service spec.ports[0].port", YAML_PORT_MIN,
		    YAML_PORT_MAX, 0, &port) != 0) {
			ens_close(&doc);
			return (CONVERT_VALIDATION_ERROR);
		}
	}

	svc_name = name ? name : "unknown";
	qsvc = ENS_Q(qname, svc_name);

	/* Emit only what the Service declares -- nothing invented. */
	if (selector != NULL)
		snprintf(selbuf, sizeof(selbuf), "    selector: %s\n",
		    ENS_Q(qsel, selector));
	if (port > 0) {
		snprintf(portnum, sizeof(portnum), "%ld", port);
		snprintf(portbuf, sizeof(portbuf),
		    "    ports:\n"
		    "      - container: %s\n"
		    "        protocol: tcp\n",
		    portnum);
	}

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
	    ENS_Q(qorigns, namespace ? namespace : "default"),
	    qsvc,
	    qsvc, ENS_Q(qns, namespace ? namespace : opts->namespace), qsvc,
	    selbuf,
	    portbuf);

	ens_close(&doc);

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
	struct ens_doc doc;
	const char *name, *namespace;
	char qname[512], qns[512];
	const char *qname_s, *qns_s;

	if (ens_open(&doc, yaml) != 0)
		return (CONVERT_SYNTAX_ERROR);
	name = ens_path(&doc, "metadata.name");
	if (name == NULL)
		name = ens_path(&doc, "name");	/* flat Ensemble dialect */
	namespace = ens_path(&doc, "metadata.namespace");
	if (namespace == NULL)
		namespace = ens_path(&doc, "namespace");
	qname_s = ENS_Q(qname, name ? name : "unknown");
	const char *ns = ensemble_resolve_namespace(namespace, opts);

	qns_s = ENS_Q(qns, ns);

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
	    qns_s, qname_s, qname_s, qns_s);

	ens_close(&doc);

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
	struct ens_doc doc;
	const char *name, *namespace;
	char qname[512], qns[512];
	const char *qname_s, *qns_s;

	if (ens_open(&doc, yaml) != 0)
		return (CONVERT_SYNTAX_ERROR);
	name = ens_path(&doc, "metadata.name");
	if (name == NULL)
		name = ens_path(&doc, "name");	/* flat Ensemble dialect */
	namespace = ens_path(&doc, "metadata.namespace");
	if (namespace == NULL)
		namespace = ens_path(&doc, "namespace");
	qname_s = ENS_Q(qname, name ? name : "unknown");
	const char *ns = ensemble_resolve_namespace(namespace, opts);

	qns_s = ENS_Q(qns, ns);

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
	    qns_s, qname_s, qname_s, qns_s);

	ens_close(&doc);

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
	struct ens_doc doc;
	const char *name, *namespace;
	char qname[512], qns[512];
	const char *qname_s, *qns_s, *qnet_s;
	char netname[600], qnet[1300];

	if (ens_open(&doc, yaml) != 0)
		return (CONVERT_SYNTAX_ERROR);
	name = ens_path(&doc, "metadata.name");
	if (name == NULL)
		name = ens_path(&doc, "name");	/* flat Ensemble dialect */
	namespace = ens_path(&doc, "metadata.namespace");
	if (namespace == NULL)
		namespace = ens_path(&doc, "namespace");
	qname_s = ENS_Q(qname, name ? name : "unknown");
	const char *ns = ensemble_resolve_namespace(namespace, opts);

	qns_s = ENS_Q(qns, ns);
	snprintf(netname, sizeof(netname), "%s-net", name ? name : "unknown");
	qnet_s = ENS_Q(qnet, netname);

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
	    "  - name: %s\n"
	    "    driver: bridge\n"
	    "    ingress: true\n"
	    "    # Configure ingress rules in network config\n"
	    "    # rules:\n"
	    "    #   - host: example.com\n"
	    "    #     paths:\n"
	    "    #       - path: /\n"
	    "    #         service: myservice\n"
	    "    #         port: 80\n",
	    qns_s, qname_s, qname_s, qns_s,
	    qnet_s);

	ens_close(&doc);

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
	struct ens_doc doc;
	const char *name, *namespace, *storage_str, *ns, *vol_name;
	char sizebuf[192];
	char qname[512], qns[512], qvol[512], qsize[256];
	const char *qname_s, *qns_s;

	if (ens_open(&doc, yaml) != 0)
		return (CONVERT_SYNTAX_ERROR);
	name = ens_path(&doc, "metadata.name");
	if (name == NULL)
		name = ens_path(&doc, "name");	/* flat Ensemble dialect */
	namespace = ens_path(&doc, "metadata.namespace");
	if (namespace == NULL)
		namespace = ens_path(&doc, "namespace");
	qname_s = ENS_Q(qname, name ? name : "unknown");
	storage_str = ens_path(&doc, "spec.resources.requests.storage");
	if (storage_str == NULL)
		storage_str = ens_path(&doc, "storage");
	ns = ensemble_resolve_namespace(namespace, opts);
	qns_s = ENS_Q(qns, ns);
	vol_name = name ? name : "data";

	/*
	 * A storage request is a quantity, not free text: a number followed by
	 * an optional binary (Ki/Mi/Gi/Ti/Pi) or decimal (k/M/G/T/P) suffix.
	 * Copying an unvalidated string through would put something like
	 * "ten gigs" -- or an injected newline -- into the emitted config,
	 * where it fails much later and much less clearly.
	 */
	if (storage_str != NULL && !quantity_is_valid(storage_str)) {
		fprintf(stderr, "error: PersistentVolumeClaim '%s': storage "
		    "request \"%s\" is not a valid quantity (expected e.g. "
		    "512Mi, 10Gi, 1T)\n", name ? name : "unknown", storage_str);
		ens_close(&doc);
		return (CONVERT_VALIDATION_ERROR);
	}

	/*
	 * Emit the declared storage request. If the claim omits it, do not
	 * invent a size — leave a TODO the operator must fill in, and say so.
	 */
	if (storage_str != NULL) {
		snprintf(sizebuf, sizeof(sizebuf), "    size: %s\n",
		    ENS_Q(qsize, storage_str));
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
	    qns_s, qname_s, qname_s, qns_s,
	    ENS_Q(qvol, vol_name),
	    sizebuf);

	ens_close(&doc);

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
	struct ens_doc doc;
	const char *name;
	char qname[512];
	char *result;

	if (ens_open(&doc, yaml) != 0)
		return (CONVERT_SYNTAX_ERROR);
	name = ens_path(&doc, "metadata.name");

	asprintf(&result,
	    "# Converted from Ensemble Namespace\n"
	    "\n"
	    "# Namespace definition\n"
	    "name: %s\n",
	    ENS_Q(qname, name ? name : "default"));

	ens_close(&doc);

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
	int failed = 0;
	int first_error = CONVERT_SUCCESS;

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
		/*
		 * A document separator is a "---" that BEGINS a line. Matching
		 * it anywhere chopped documents apart at prose: this project's
		 * own example manifest contains the comment
		 * "# --- Stateless web tier ---", and splitting there handed
		 * the converter fragments of a Stack that it then converted as
		 * though each were a standalone Deployment.
		 */
		doc_end = NULL;
		{
			const char *q = doc_start;

			while ((q = strstr(q, "---")) != NULL) {
				if (q == yaml || q[-1] == '\n') {
					doc_end = q;
					break;
				}
				q += 3;
			}
		}
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
			/*
			 * Name the kind that was skipped. "unknown kind" told
			 * the author nothing about which document was dropped
			 * or why -- and a `kind: Stack` manifest is not a
			 * migration input at all, it is deployed directly.
			 */
			{
				const char *ks = ensemble_kind_str(kind);

				if (kind == ENSEMBLE_UNKNOWN)
					fprintf(stderr, "warning: document %d "
					    "has no recognised 'kind'; "
					    "skipped\n", docs + failed + 1);
				else
					fprintf(stderr, "warning: document %d: "
					    "'%s' is not a supported conversion "
					    "source; skipped\n",
					    docs + failed + 1, ks);
				if (kind == ENSEMBLE_UNKNOWN)
					asprintf(&converted, "# Skipped: no "
					    "recognised 'kind' in this "
					    "document\n");
				else
					asprintf(&converted, "# Skipped "
					    "unsupported kind: %s\n", ks);
			}
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

		else if (ret != CONVERT_SUCCESS) {
			/*
			 * Record the first failure and keep going, so one run
			 * reports every bad document rather than making the
			 * author fix them one at a time. The conversion as a
			 * whole still FAILS: silently dropping a document
			 * would hand back a partial config that looks
			 * complete, and a partial config is the one outcome
			 * worse than an error.
			 */
			fprintf(stderr, "error: document %d (%s) was not "
			    "converted\n", docs + failed + 1,
			    ensemble_kind_str(kind));
			if (first_error == CONVERT_SUCCESS)
				first_error = ret;
			failed++;
		}

		free(converted);
		free(doc);
		/* Advance to the separator (or end); the top of the loop
		 * consumes the "---" and leading whitespace. */
		p = doc_end;
	}

	if (result == NULL)
		return (CONVERT_MEMORY_ERROR);
	if (first_error != CONVERT_SUCCESS) {
		fprintf(stderr, "error: %d of %d document(s) failed to "
		    "convert; no output written\n", failed, docs + failed);
		free(result);
		return (first_error);
	}
	*output = result;
	return (CONVERT_SUCCESS);
}
