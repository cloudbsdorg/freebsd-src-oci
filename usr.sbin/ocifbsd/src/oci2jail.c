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
 * OCI Runtime Specification to FreeBSD Jail Parameter Translation
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/jail.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syslimits.h>

#include <ctype.h>
#include <errno.h>
#include <jail.h>
#include <json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>

#include "ocifbsd.h"
#include "network/netcfg.h"

/*
 * JSON parsing helpers
 */

static char *
json_get_string(struct json_object *val, const char *key)
{
	struct json_object *obj;

	if (val == NULL || json_object_get_type(val) != json_type_object)
		return (NULL);

	obj = (val);
	if (obj == NULL)
		return (NULL);

	val = json_object_object_get(obj, key);
	if (val == NULL || json_object_get_type(val) != json_type_string)
		return (NULL);

	return (strdup(json_object_get_string(val)));
}

static int
json_get_bool(struct json_object *val, const char *key, bool defval)
{
	struct json_object *obj;

	if (val == NULL || json_object_get_type(val) != json_type_object)
		return (defval);

	obj = (val);
	if (obj == NULL)
		return (defval);

	val = json_object_object_get(obj, key);
	if (val == NULL || json_object_get_type(val) != json_type_boolean)
		return (defval);

	return (json_object_get_boolean(val) ? 1 : 0);
}

static int
json_get_int(struct json_object *val, const char *key, int defval)
{
	struct json_object *obj;

	if (val == NULL || json_object_get_type(val) != json_type_object)
		return (defval);

	obj = (val);
	if (obj == NULL)
		return (defval);

	val = json_object_object_get(obj, key);
	if (val == NULL || json_object_get_type(val) != json_type_int)
		return (defval);

	return (json_object_get_int(val));
}

static char **
json_get_string_array(struct json_object *val, const char *key, int *nitems)
{
	struct json_object *obj;
	struct json_object *elem;
	char **result;
	size_t i, len;

	*nitems = 0;

	if (val == NULL || json_object_get_type(val) != json_type_object)
		return (NULL);

	obj = (val);
	if (obj == NULL)
		return (NULL);

	val = json_object_object_get(obj, key);
	if (val == NULL || json_object_get_type(val) != json_type_array)
		return (NULL);

	len = json_object_array_length(val);
	result = malloc((len + 1) * sizeof(char *));
	if (result == NULL)
		return (NULL);

	for (i = 0; i < len; i++) {
		elem = json_object_array_get_idx(val, i);
		if (json_object_get_type(elem) == json_type_string)
			result[i] = strdup(json_object_get_string(elem));
		else
			result[i] = NULL;
	}
	result[len] = NULL;
	*nitems = (int)len;

	return (result);
}

/*
 * Parse FreeBSD-specific extension from OCI config
 */
static struct oci_freebsd *
parse_freebsd_ext(struct json_object *val)
{
	struct oci_freebsd *fbsd;
	struct json_object *obj;

	if (val == NULL || json_object_get_type(val) != json_type_object)
		return (NULL);

	obj = (val);
	if (obj == NULL)
		return (NULL);

	fbsd = calloc(1, sizeof(*fbsd));
	if (fbsd == NULL)
		return (NULL);

	fbsd->vnet = json_get_bool(val, "vnet", 0);
	fbsd->hostname = json_get_string(val, "hostname");
	fbsd->domainname = json_get_string(val, "domainname");
	fbsd->mac_label = json_get_string(val, "macLabel");
	fbsd->ip4 = json_get_string_array(val, "ip4", &fbsd->n_ip4);
	fbsd->ip6 = json_get_string_array(val, "ip6", &fbsd->n_ip6);
	fbsd->dns = json_get_string_array(val, "dns", &fbsd->n_dns);

	return (fbsd);
}

/*
 * Join a JSON string array with commas (for OCI mount options).
 */
static char *
json_join_string_array(struct json_object *arr)
{
	size_t i, n, len = 0;
	char *out, *p;

	if (arr == NULL || json_object_get_type(arr) != json_type_array)
		return (NULL);

	n = json_object_array_length(arr);
	if (n == 0)
		return (strdup(""));

	for (i = 0; i < n; i++) {
		struct json_object *elem = json_object_array_get_idx(arr, i);

		if (elem != NULL &&
		    json_object_get_type(elem) == json_type_string)
			len += strlen(json_object_get_string(elem)) + 1;
	}
	out = malloc(len + 1);
	if (out == NULL)
		return (NULL);
	p = out;
	*p = '\0';
	for (i = 0; i < n; i++) {
		struct json_object *elem = json_object_array_get_idx(arr, i);
		const char *s;

		if (elem == NULL ||
		    json_object_get_type(elem) != json_type_string)
			continue;
		s = json_object_get_string(elem);
		if (p != out)
			*p++ = ',';
		while (*s != '\0')
			*p++ = *s++;
		*p = '\0';
	}
	return (out);
}

/*
 * Return true if `token` appears as a whole comma-separated option in the
 * mount option string `opts` (e.g. "ro" in "nosuid,ro,noexec"), rather than
 * merely as a substring of some larger option.
 */
static bool
option_token_present(const char *opts, const char *token)
{
	size_t tlen = strlen(token);
	const char *p = opts;

	while (p != NULL && *p != '\0') {
		const char *comma = strchr(p, ',');
		size_t seglen = (comma != NULL) ? (size_t)(comma - p)
		    : strlen(p);

		if (seglen == tlen && strncmp(p, token, tlen) == 0)
			return (true);
		if (comma == NULL)
			break;
		p = comma + 1;
	}
	return (false);
}

/*
 * Parse OCI mounts array. options may be a string or string array.
 */
static struct oci_mount *
parse_mounts(struct json_object *val, int *n_mounts)
{
	struct json_object *obj;
	struct json_object *arr;
	struct json_object *elem;
	struct json_object *opt;
	struct oci_mount *mounts;
	struct oci_mount *m;
	size_t i;

	*n_mounts = 0;

	if (val == NULL || json_object_get_type(val) != json_type_object)
		return (NULL);

	obj = (val);
	if (obj == NULL)
		return (NULL);

	val = json_object_object_get(obj, "mounts");
	if (val == NULL || json_object_get_type(val) != json_type_array)
		return (NULL);

	arr = (val);
	mounts = calloc(json_object_array_length(arr) + 1, sizeof(*mounts));
	if (mounts == NULL)
		return (NULL);

	for (i = 0; i < json_object_array_length(arr); i++) {
		m = &mounts[i];
		elem = json_object_array_get_idx(arr, i);
		if (json_object_get_type(elem) != json_type_object)
			continue;

		obj = (elem);
		m->source = json_get_string(elem, "source");
		m->destination = json_get_string(elem, "destination");
		m->type = json_get_string(elem, "type");
		opt = json_object_object_get(elem, "options");
		if (opt != NULL &&
		    json_object_get_type(opt) == json_type_string)
			m->options = strdup(json_object_get_string(opt));
		else if (opt != NULL &&
		    json_object_get_type(opt) == json_type_array)
			m->options = json_join_string_array(opt);
		else
			m->options = NULL;
		m->readonly = json_get_bool(elem, "readonly", false);
		/*
		 * OCI often encodes ro in options rather than readonly. Match
		 * "ro" as a whole comma-separated option token, not a substring
		 * — otherwise options like "errors=..." or "proto=tcp" would
		 * falsely force a writable mount to read-only.
		 */
		if (!m->readonly && m->options != NULL &&
		    option_token_present(m->options, "ro"))
			m->readonly = true;
	}
	*n_mounts = (int)json_object_array_length(arr);

	return (mounts);
}

/*
 * Parse OCI hooks
 */
static struct oci_hooks *
parse_hooks(struct json_object *val)
{
	struct oci_hooks *hooks;
	struct json_object *obj;
	struct json_object *arr;
	struct json_object *elem;
	struct oci_hook **h;
	size_t i;

	if (val == NULL || json_object_get_type(val) != json_type_object)
		return (NULL);

	obj = json_object_object_get(val, "hooks");
	if (obj == NULL || json_object_get_type(obj) != json_type_object)
		return (NULL);

	hooks = calloc(1, sizeof(*hooks));
	if (hooks == NULL)
		return (NULL);

	/*
	 * Look each hook array up from the stable "hooks" object (obj).
	 * The previous version reassigned the shared cursor to the prestart
	 * array, after which "poststart"/"poststop" were never found.
	 */
#define PARSE_HOOK_ARRAY(hook_type, field, count_field) do {			\
	arr = json_object_object_get(obj, hook_type);			\
	if (arr != NULL && json_object_get_type(arr) == json_type_array) {			\
		hooks->count_field = (int)json_object_array_length(arr);			\
		hooks->field = calloc(json_object_array_length(arr) + 1, sizeof(*h));	\
		if (hooks->field == NULL)					\
			goto cleanup;						\
		for (i = 0; i < json_object_array_length(arr); i++) {				\
			elem = json_object_array_get_idx(arr, i);					\
			if (json_object_get_type(elem) != json_type_object)			\
				continue;					\
			h = &hooks->field[i];					\
			*h = calloc(1, sizeof(**h));				\
			if (*h == NULL)						\
				continue;					\
			(*h)->path = json_get_string(elem, "path");		\
			(*h)->args = json_get_string_array(elem, "args",	\
			    &(int){0});					\
			(*h)->env = json_get_string_array(elem, "env",	\
			    &(int){0});					\
			(*h)->timeout = json_get_string(elem, "timeout");	\
		}								\
	}									\
} while (0)

	PARSE_HOOK_ARRAY("prestart", prestart, n_prestart);
	PARSE_HOOK_ARRAY("poststart", poststart, n_poststart);
	PARSE_HOOK_ARRAY("poststop", poststop, n_poststop);

#undef PARSE_HOOK_ARRAY

	return (hooks);

cleanup:
	if (hooks->prestart)
		free(hooks->prestart);
	if (hooks->poststart)
		free(hooks->poststart);
	if (hooks->poststop)
		free(hooks->poststop);
	free(hooks);
	return (NULL);
}

/*
 * Parse OCI Runtime Specification from config.json
 */
struct oci_runtime_spec *
oci_parse_config(const char *config_path)
{
	struct oci_runtime_spec *spec;
	struct json_object *root;
	struct json_object *obj;
	char *json_str;
	FILE *f;
	struct stat sb;

	if (config_path == NULL) {
		errno = EINVAL;
		return (NULL);
	}

	/* Read file */
	f = fopen(config_path, "r");
	if (f == NULL)
		return (NULL);

	if (fstat(fileno(f), &sb) != 0) {
		fclose(f);
		return (NULL);
	}

	json_str = malloc(sb.st_size + 1);
	if (json_str == NULL) {
		fclose(f);
		return (NULL);
	}

	if (fread(json_str, 1, sb.st_size, f) != (size_t)sb.st_size) {
		free(json_str);
		fclose(f);
		return (NULL);
	}
	json_str[sb.st_size] = '\0';
	fclose(f);

	/* Parse JSON */
	root = json_tokener_parse(json_str);
	free(json_str);

	if (root == NULL || json_object_get_type(root) != json_type_object) {
		if (root)
			json_object_put(root);
		errno = EINVAL;
		return (NULL);
	}

	obj = (root);

	/* Allocate spec */
	spec = calloc(1, sizeof(*spec));
	if (spec == NULL) {
		json_object_put(root);
		return (NULL);
	}

	/* Parse root */
	struct json_object *root_val = json_object_object_get(obj, "root");
	if (root_val != NULL && json_object_get_type(root_val) == json_type_object) {
		spec->root.path = json_get_string(root_val, "path");
		spec->root.readonly = json_get_bool(root_val, "readonly", false);
	} else {
		/* Default root to "rootfs" relative to bundle */
		char *dir = strdup(config_path);
		char *p = strrchr(dir, '/');
		if (p) {
			*p = '\0';
			/* Go up from config.json */
			p = strrchr(dir, '/');
			if (p) {
				size_t len;
				*p = '\0';
				len = strlen(p + 1) + strlen("/rootfs") + 1;
				spec->root.path = malloc(len);
				if (spec->root.path != NULL)
					snprintf(spec->root.path, len, "%s/rootfs", p + 1);
			} else {
				spec->root.path = strdup("rootfs");
			}
		} else {
			spec->root.path = strdup("rootfs");
		}
		free(dir);
	}

	/* Parse process */
	struct json_object *proc_val = json_object_object_get(obj, "process");
	if (proc_val != NULL && json_object_get_type(proc_val) == json_type_object) {
		int nargs = 0, nenv = 0;
		struct json_object *user_val;

		spec->process.cwd = json_get_string(proc_val, "cwd");
		spec->process.tty = json_get_bool(proc_val, "tty", 0);
		spec->process.terminal = json_get_bool(proc_val, "terminal", 0);
		spec->process.args = json_get_string_array(proc_val, "args",
		    &nargs);
		spec->process.env = json_get_string_array(proc_val, "env",
		    &nenv);
		/*
		 * OCI process.user is an object { "uid": N, "gid": N }.
		 * Also accept a bare integer as uid=gid for robustness.
		 */
		user_val = json_object_object_get(proc_val, "user");
		if (user_val != NULL &&
		    json_object_get_type(user_val) == json_type_object) {
			spec->process.uid = (uid_t)json_get_int(user_val,
			    "uid", 0);
			spec->process.gid = (gid_t)json_get_int(user_val,
			    "gid", 0);
		} else if (user_val != NULL &&
		    json_object_get_type(user_val) == json_type_int) {
			spec->process.uid =
			    (uid_t)json_object_get_int(user_val);
			spec->process.gid = (gid_t)spec->process.uid;
		} else {
			spec->process.uid = 0;
			spec->process.gid = 0;
		}
		/* process.rlimits: [ { type, hard, soft }, ... ] */
		{
			struct json_object *rlarr, *elem;
			size_t ri, rn;

			rlarr = json_object_object_get(proc_val, "rlimits");
			if (rlarr != NULL &&
			    json_object_get_type(rlarr) == json_type_array) {
				rn = json_object_array_length(rlarr);
				spec->process.rlimits = calloc(rn + 1,
				    sizeof(*spec->process.rlimits));
				if (spec->process.rlimits != NULL) {
					spec->process.n_rlimits = (int)rn;
					for (ri = 0; ri < rn; ri++) {
						elem = json_object_array_get_idx(
						    rlarr, ri);
						if (elem == NULL ||
						    json_object_get_type(elem) !=
						    json_type_object)
							continue;
						spec->process.rlimits[ri].type =
						    json_get_string(elem,
						    "type");
						spec->process.rlimits[ri].hard =
						    (rlim_t)json_get_int(elem,
						    "hard", 0);
						spec->process.rlimits[ri].soft =
						    (rlim_t)json_get_int(elem,
						    "soft", 0);
					}
				}
			}
		}
		(void)nargs;
		(void)nenv;
	}

	/* Parse hostname */
	spec->hostname = json_get_string(root, "hostname");
	spec->domainname = json_get_string(root, "domainname");

	/* Parse mounts */
	spec->mounts = parse_mounts(root, &spec->n_mounts);

	/* Parse hooks */
	spec->hooks = parse_hooks(root);

	/* Parse FreeBSD extensions */
	struct json_object *fbsd_val = json_object_object_get(obj, "freebsd");
	spec->freebsd = parse_freebsd_ext(fbsd_val);

	json_object_put(root);

	return (spec);
}

/*
 * Free OCI spec
 */
void
oci_free_spec(struct oci_runtime_spec *spec)
{
	int i;

	if (spec == NULL)
		return;

	free(spec->root.path);
	free(spec->process.cwd);

	if (spec->process.args) {
		for (i = 0; spec->process.args[i]; i++)
			free(spec->process.args[i]);
		free(spec->process.args);
	}

	if (spec->process.env) {
		for (i = 0; spec->process.env[i]; i++)
			free(spec->process.env[i]);
		free(spec->process.env);
	}

	if (spec->process.rlimits != NULL) {
		for (i = 0; i < spec->process.n_rlimits; i++)
			free(spec->process.rlimits[i].type);
		free(spec->process.rlimits);
	}

	if (spec->mounts) {
		for (i = 0; i < spec->n_mounts; i++) {
			free(spec->mounts[i].source);
			free(spec->mounts[i].destination);
			free(spec->mounts[i].type);
			free(spec->mounts[i].options);
		}
		free(spec->mounts);
	}

#define FREE_HOOKS(arr, count) do {						\
	if (arr) {								\
		for (i = 0; i < count && arr[i]; i++) {			\
			free(arr[i]->path);					\
			if (arr[i]->args) {					\
				for (j = 0; arr[i]->args[j]; j++)		\
					free(arr[i]->args[j]);			\
				free(arr[i]->args);				\
			}							\
			if (arr[i]->env) {					\
				for (j = 0; arr[i]->env[j]; j++)		\
					free(arr[i]->env[j]);			\
				free(arr[i]->env);				\
			}							\
			free(arr[i]->timeout);				\
			free(arr[i]);					\
		}								\
		free(arr);							\
	}									\
} while (0)

	if (spec->hooks) {
		int j;
		FREE_HOOKS(spec->hooks->prestart, spec->hooks->n_prestart);
		FREE_HOOKS(spec->hooks->poststart, spec->hooks->n_poststart);
		FREE_HOOKS(spec->hooks->poststop, spec->hooks->n_poststop);
		free(spec->hooks);
	}

	if (spec->freebsd) {
		free(spec->freebsd->hostname);
		free(spec->freebsd->domainname);
		free(spec->freebsd->mac_label);
		if (spec->freebsd->ip4) {
			for (i = 0; spec->freebsd->ip4[i]; i++)
				free(spec->freebsd->ip4[i]);
			free(spec->freebsd->ip4);
		}
		if (spec->freebsd->ip6) {
			for (i = 0; spec->freebsd->ip6[i]; i++)
				free(spec->freebsd->ip6[i]);
			free(spec->freebsd->ip6);
		}
		if (spec->freebsd->dns) {
			for (i = 0; spec->freebsd->dns[i]; i++)
				free(spec->freebsd->dns[i]);
			free(spec->freebsd->dns);
		}
		free(spec->freebsd);
	}

	free(spec->hostname);
	free(spec->domainname);
	free(spec);
}

/*
 * Default the spec hostname when the image/bundle config did not set one.
 *
 * A container should come up with a hostname even when its OCI config omits
 * one — a bare FreeBSD base image, for instance, carries no hostname, which
 * otherwise leaves kern.hostname empty inside the jail. The fallback (the
 * container name, or its id) is only applied when neither the top-level
 * hostname nor a FreeBSD-extension hostname is present, so an explicit
 * hostname from either source always wins. A NULL or empty fallback leaves
 * the hostname unset rather than inventing one.
 */
void
oci_spec_default_hostname(struct oci_runtime_spec *spec, const char *fallback)
{
	if (spec == NULL || fallback == NULL || fallback[0] == '\0')
		return;
	if (spec->hostname != NULL)
		return;
	if (spec->freebsd != NULL && spec->freebsd->hostname != NULL)
		return;
	spec->hostname = strdup(fallback);
}

/*
 * Duplicate a NUL-terminated-on-count string array into a fresh, NULL-
 * terminated array of exactly n+1 entries. Returns NULL on allocation
 * failure (partial allocations are rolled back).
 */
/*
 * Copy the address portion of an "addr" or "addr/prefix" string into buf,
 * dropping any CIDR prefix. buf is always NUL-terminated.
 */
static void
addr_without_prefix(const char *cidr, char *buf, size_t buflen)
{
	const char *slash;
	size_t n;

	if (buflen == 0)
		return;
	slash = strchr(cidr, '/');
	n = (slash != NULL) ? (size_t)(slash - cidr) : strlen(cidr);
	if (n >= buflen)
		n = buflen - 1;
	memcpy(buf, cidr, n);
	buf[n] = '\0';
}

/*
 * Join the prefix-stripped addresses of a NULL-terminated CIDR array into a
 * single comma-separated string, the form the ip4.addr/ip6.addr jail
 * parameters expect for multiple addresses (they are array parameters, so
 * passing the option name more than once keeps only the last value). Returns
 * a malloc'd string the caller frees, or NULL if there are no addresses.
 */
static char *
join_addrs_no_prefix(char **arr, int n)
{
	char *out;
	size_t cap;
	int i, count = 0;

	for (i = 0; i < n && arr[i] != NULL; i++)
		count++;
	if (count == 0)
		return (NULL);
	/* Worst case: every entry is a full address plus a comma. */
	cap = (size_t)count * (INET6_ADDRSTRLEN + 1) + 1;
	out = malloc(cap);
	if (out == NULL)
		return (NULL);
	out[0] = '\0';
	for (i = 0; i < count; i++) {
		char a[INET6_ADDRSTRLEN];

		if (i > 0)
			strlcat(out, ",", cap);
		addr_without_prefix(arr[i], a, sizeof(a));
		strlcat(out, a, cap);
	}
	return (out);
}

/*
 * Append a jail parameter whose value is a heap string the caller hands off:
 * the value is freed on every path (success or failure). On failure the whole
 * parameter array is freed and *pp is set to NULL, mirroring ADD_PARAM's
 * fail-closed contract; the caller returns NULL. Used for values built on the
 * heap (comma-joined address lists) that ADD_PARAM's early return would leak.
 */
static int
add_param_free_value(struct jailparam **pp, size_t *np, size_t *capp,
    const char *name, char *value)
{
	struct jailparam *params = *pp;
	size_t n = *np, capacity = *capp;

	if (n >= capacity) {
		struct jailparam *tmp =
		    realloc(params, capacity * 2 * sizeof(*params));

		if (tmp == NULL)
			goto fail;
		params = tmp;
		capacity *= 2;
	}
	if (jailparam_init(&params[n], name) != 0)
		goto fail;
	if (jailparam_import(&params[n], value) != 0) {
		jailparam_free(&params[n], 1);
		goto fail;
	}
	n++;
	free(value);
	*pp = params;
	*np = n;
	*capp = capacity;
	return (0);
fail:
	free(value);
	jailparam_free(params, n);
	free(params);
	*pp = NULL;
	return (-1);
}

static char **
dup_str_array(char **src, size_t n)
{
	char **dst;
	size_t i;

	dst = calloc(n + 1, sizeof(*dst));
	if (dst == NULL)
		return (NULL);
	for (i = 0; i < n; i++) {
		dst[i] = strdup(src[i]);
		if (dst[i] == NULL) {
			while (i > 0)
				free(dst[--i]);
			free(dst);
			return (NULL);
		}
	}
	return (dst);
}

static void
free_str_array_z(char **arr)
{
	size_t i;

	if (arr == NULL)
		return;
	for (i = 0; arr[i] != NULL; i++)
		free(arr[i]);
	free(arr);
}

/*
 * Overlay a persisted per-container network configuration onto an OCI spec
 * before its jail parameters are built, so that `ocifbsd network set` takes
 * effect the next time the container's jail is created. Only fields the
 * configuration actually sets are applied; each replaces (rather than
 * appends to) the corresponding spec field so the stored configuration is
 * authoritative. Gateways and DNS are recorded in the configuration for the
 * network-setup path but are not jail parameters, so they are not copied
 * here. On any allocation failure the spec is left unchanged for that field.
 */
void
netcfg_apply_to_spec(const struct netcfg *nc, struct oci_runtime_spec *spec)
{
	struct oci_freebsd *fb;
	char **tmp;

	if (nc == NULL || spec == NULL)
		return;
	if (spec->freebsd == NULL) {
		spec->freebsd = calloc(1, sizeof(*spec->freebsd));
		if (spec->freebsd == NULL)
			return;
	}
	fb = spec->freebsd;

	if (nc->vnet != -1)
		fb->vnet = (nc->vnet == 1);
	if (nc->n_ip4 > 0 && (tmp = dup_str_array(nc->ip4, nc->n_ip4)) != NULL) {
		free_str_array_z(fb->ip4);
		fb->ip4 = tmp;
		fb->n_ip4 = (int)nc->n_ip4;
	}
	if (nc->n_ip6 > 0 && (tmp = dup_str_array(nc->ip6, nc->n_ip6)) != NULL) {
		free_str_array_z(fb->ip6);
		fb->ip6 = tmp;
		fb->n_ip6 = (int)nc->n_ip6;
	}
	if (nc->n_dns > 0 && (tmp = dup_str_array(nc->dns, nc->n_dns)) != NULL) {
		free_str_array_z(fb->dns);
		fb->dns = tmp;
		fb->n_dns = (int)nc->n_dns;
	}
}

/*
 * Translate OCI Runtime Spec to FreeBSD jail parameters
 */
struct jailparam *
oci_spec_to_jail_params(const struct oci_runtime_spec *spec, size_t *nparams)
{
	struct jailparam *params;
	size_t n, capacity;

	if (spec == NULL || nparams == NULL)
		return (NULL);

	/* Initial capacity - will grow as needed */
	capacity = 32;
	params = calloc(capacity, sizeof(*params));
	if (params == NULL)
		return (NULL);
	n = 0;

/*
 * jailparam_import (not _raw) converts the string value according to the
 * parameter's kernel type. This matters for typed parameters such as vnet,
 * which is a "jailsys" enum whose keyword ("new"/"inherit"/"disable") must be
 * mapped to an integer — passing the literal bytes via jailparam_import_raw
 * makes jail_set(2) reject it with EINVAL. String parameters (path, hostname,
 * name, ...) import as a plain copy, so this is correct for every parameter.
 */
/*
 * Fail closed: any allocation, init, or import failure frees everything built
 * so far and returns NULL, so oci_spec_to_jail_params never yields a partial
 * parameter list (which could, e.g., drop "path" while keeping "persist" and
 * create a persistent jail rooted at /). Values reaching here are already
 * validated (netcfg CIDRs, the vnet keyword, etc.), so a failure is a genuine
 * resource error, not a bad value to skip over.
 */
#define ADD_PARAM(name, value) do {						\
	if (n >= capacity) {							\
		struct jailparam *_np;						\
		_np = realloc(params, capacity * 2 * sizeof(*params));		\
		if (_np == NULL) {						\
			jailparam_free(params, n);				\
			free(params);						\
			return (NULL);						\
		}								\
		params = _np;							\
		capacity *= 2;							\
	}									\
	if (jailparam_init(&params[n], name) != 0) {				\
		jailparam_free(params, n);					\
		free(params);							\
		return (NULL);							\
	}									\
	if (jailparam_import(&params[n], value) != 0) {				\
		jailparam_free(params, n + 1);				\
		free(params);							\
		return (NULL);							\
	}									\
	n++;									\
} while (0)

	/*
	 * Root filesystem — required. Refuse to build a parameter set with no
	 * path: the kernel would default a pathless jail to "/", so emitting
	 * persist/name without a path would leave a persistent jail rooted at
	 * the host filesystem. Fail closed instead.
	 */
	if (spec->root.path == NULL || spec->root.path[0] == '\0') {
		jailparam_free(params, n);
		free(params);
		errno = EINVAL;
		return (NULL);
	}
	ADD_PARAM("path", spec->root.path);

	/* Hostname */
	if (spec->hostname) {
		ADD_PARAM("host.hostname", spec->hostname);
	} else if (spec->freebsd && spec->freebsd->hostname) {
		ADD_PARAM("host.hostname", spec->freebsd->hostname);
	}

	/* Domainname */
	if (spec->domainname) {
		ADD_PARAM("host.domainname", spec->domainname);
	} else if (spec->freebsd && spec->freebsd->domainname) {
		ADD_PARAM("host.domainname", spec->freebsd->domainname);
	}

	/* VNET - network isolation */
	if (spec->freebsd && spec->freebsd->vnet) {
		ADD_PARAM("vnet", "new");
	}

	/*
	 * IP addresses as jail parameters apply only to non-VNET jails, which
	 * share the host network stack and are restricted to these addresses.
	 * A VNET jail has its own stack and configures addresses on its own
	 * interfaces (epair), so ip4.addr/ip6.addr are invalid there and would
	 * make jail creation fail with EINVAL.
	 */
	if (spec->freebsd && !spec->freebsd->vnet) {
		char *joined;

		/*
		 * ip4.addr/ip6.addr are array parameters: all addresses go in a
		 * single comma-separated value (a repeated option name would
		 * keep only the last). Each address is emitted without its CIDR
		 * prefix, which is jail(8) syntax that libjail's parser rejects.
		 */
		joined = join_addrs_no_prefix(spec->freebsd->ip4,
		    spec->freebsd->n_ip4);
		if (joined != NULL && add_param_free_value(&params, &n,
		    &capacity, "ip4.addr", joined) != 0)
			return (NULL);
		joined = join_addrs_no_prefix(spec->freebsd->ip6,
		    spec->freebsd->n_ip6);
		if (joined != NULL && add_param_free_value(&params, &n,
		    &capacity, "ip6.addr", joined) != 0)
			return (NULL);
	}

	/* MAC label */
	if (spec->freebsd && spec->freebsd->mac_label) {
		ADD_PARAM("security.mac.label", spec->freebsd->mac_label);
		ADD_PARAM("allow.chflags", "1");
	}

	/*
	 * Persist without processes so create→start works as two steps.
	 * Without this, JAIL_CREATE with no attached process does not keep
	 * a usable jail for a later container_start().
	 */
	ADD_PARAM("persist", "true");

	/*
	 * Jail name is set by the caller via jailparam after this helper
	 * when a unique name is known. Default here is only a placeholder;
	 * container_create overrides "name" after generating the container
	 * id. Keep a stable default for direct unit tests of this helper.
	 */
	ADD_PARAM("name", "ocifbsd-container");

#undef ADD_PARAM

	*nparams = n;
	return (params);
}

/*
 * Validate OCI spec for basic requirements
 */
int
oci_validate_spec(const struct oci_runtime_spec *spec)
{
	if (spec == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/* Must have root path */
	if (spec->root.path == NULL || spec->root.path[0] == '\0') {
		fprintf(stderr, "error: missing root path in OCI config\n");
		errno = EINVAL;
		return (-1);
	}

	/* Validate root path exists */
	struct stat sb;
	if (stat(spec->root.path, &sb) != 0) {
		fprintf(stderr, "error: root path does not exist: %s\n",
		    spec->root.path);
		return (-1);
	}

	/* Validate process has command */
	if (spec->process.args == NULL || spec->process.args[0] == NULL) {
		fprintf(stderr, "error: missing command in OCI config\n");
		errno = EINVAL;
		return (-1);
	}

	return (0);
}
