/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * $FreeBSD$
 *
 * Container network configuration model: parse, validate, mutate, and
 * serialize the per-container network settings managed by
 * `ocifbsd network list|set`. See netcfg.h for the interface contract.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json.h>

#include "netcfg.h"

/* ------------------------------------------------------------------ */
/* Validation							      */
/* ------------------------------------------------------------------ */

static bool
valid_addr(const char *s, int af)
{
	unsigned char buf[sizeof(struct in6_addr)];

	if (s == NULL || s[0] == '\0')
		return (false);
	return (inet_pton(af, s, buf) == 1);
}

/*
 * Validate <addr>/<prefix>. The prefix must be a non-empty run of digits
 * with no sign or trailing characters, within [0, maxprefix], and the
 * address portion must parse for the given family.
 */
static bool
valid_cidr(const char *s, int af, int maxprefix)
{
	const char *slash;
	char addr[INET6_ADDRSTRLEN];
	size_t alen;
	long prefix;
	char *end;

	if (s == NULL)
		return (false);
	slash = strchr(s, '/');
	if (slash == NULL || slash == s)
		return (false);
	alen = (size_t)(slash - s);
	if (alen >= sizeof(addr))
		return (false);
	memcpy(addr, s, alen);
	addr[alen] = '\0';
	if (!valid_addr(addr, af))
		return (false);

	/* Prefix: digits only, no leading '+'/'-'/space, no trailing junk. */
	if (slash[1] == '\0' || !isdigit((unsigned char)slash[1]))
		return (false);
	errno = 0;
	prefix = strtol(slash + 1, &end, 10);
	if (errno != 0 || *end != '\0')
		return (false);
	return (prefix >= 0 && prefix <= maxprefix);
}

bool
netcfg_valid_ip4_cidr(const char *s)
{
	return (valid_cidr(s, AF_INET, 32));
}

bool
netcfg_valid_ip6_cidr(const char *s)
{
	return (valid_cidr(s, AF_INET6, 128));
}

bool
netcfg_valid_ip4_addr(const char *s)
{
	return (valid_addr(s, AF_INET));
}

bool
netcfg_valid_ip6_addr(const char *s)
{
	return (valid_addr(s, AF_INET6));
}

/* ------------------------------------------------------------------ */
/* Lifecycle							      */
/* ------------------------------------------------------------------ */

void
netcfg_init(struct netcfg *nc)
{
	if (nc == NULL)
		return;
	memset(nc, 0, sizeof(*nc));
	nc->vnet = -1;			/* unset */
}

static void
str_array_free(char **arr, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(arr[i]);
	free(arr);
}

void
netcfg_free(struct netcfg *nc)
{
	if (nc == NULL)
		return;
	str_array_free(nc->ip4, nc->n_ip4);
	str_array_free(nc->ip6, nc->n_ip6);
	str_array_free(nc->dns, nc->n_dns);
	free(nc->gateway4);
	free(nc->gateway6);
	free(nc->bridge);
	netcfg_init(nc);
}

/* ------------------------------------------------------------------ */
/* Mutators							      */
/* ------------------------------------------------------------------ */

static int
str_array_add(char ***arrp, size_t *np, const char *val)
{
	char **narr;
	char *dup;

	dup = strdup(val);
	if (dup == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	narr = realloc(*arrp, (*np + 1) * sizeof(*narr));
	if (narr == NULL) {
		free(dup);
		errno = ENOMEM;
		return (-1);
	}
	narr[*np] = dup;
	*arrp = narr;
	(*np)++;
	return (0);
}

int
netcfg_set_vnet(struct netcfg *nc, bool on)
{
	if (nc == NULL) {
		errno = EINVAL;
		return (-1);
	}
	nc->vnet = on ? 1 : 0;
	return (0);
}

int
netcfg_add_ip4(struct netcfg *nc, const char *cidr)
{
	if (nc == NULL || !netcfg_valid_ip4_cidr(cidr)) {
		errno = EINVAL;
		return (-1);
	}
	return (str_array_add(&nc->ip4, &nc->n_ip4, cidr));
}

int
netcfg_add_ip6(struct netcfg *nc, const char *cidr)
{
	if (nc == NULL || !netcfg_valid_ip6_cidr(cidr)) {
		errno = EINVAL;
		return (-1);
	}
	return (str_array_add(&nc->ip6, &nc->n_ip6, cidr));
}

int
netcfg_add_dns(struct netcfg *nc, const char *ns)
{
	if (nc == NULL || ns == NULL ||
	    (!netcfg_valid_ip4_addr(ns) && !netcfg_valid_ip6_addr(ns))) {
		errno = EINVAL;
		return (-1);
	}
	return (str_array_add(&nc->dns, &nc->n_dns, ns));
}

static int
set_gateway(char **slot, const char *gw, int af)
{
	char *dup;

	if (!valid_addr(gw, af)) {
		errno = EINVAL;
		return (-1);
	}
	dup = strdup(gw);
	if (dup == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	free(*slot);
	*slot = dup;
	return (0);
}

int
netcfg_set_gateway4(struct netcfg *nc, const char *gw)
{
	if (nc == NULL) {
		errno = EINVAL;
		return (-1);
	}
	return (set_gateway(&nc->gateway4, gw, AF_INET));
}

int
netcfg_set_gateway6(struct netcfg *nc, const char *gw)
{
	if (nc == NULL) {
		errno = EINVAL;
		return (-1);
	}
	return (set_gateway(&nc->gateway6, gw, AF_INET6));
}

int
netcfg_set_bridge(struct netcfg *nc, const char *bridge)
{
	char *dup;

	if (nc == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (bridge == NULL || bridge[0] == '\0') {
		free(nc->bridge);
		nc->bridge = NULL;
		return (0);
	}
	dup = strdup(bridge);
	if (dup == NULL)
		return (-1);
	free(nc->bridge);
	nc->bridge = dup;
	return (0);
}

void
netcfg_clear_ip4(struct netcfg *nc)
{
	if (nc == NULL)
		return;
	str_array_free(nc->ip4, nc->n_ip4);
	nc->ip4 = NULL;
	nc->n_ip4 = 0;
}

void
netcfg_clear_ip6(struct netcfg *nc)
{
	if (nc == NULL)
		return;
	str_array_free(nc->ip6, nc->n_ip6);
	nc->ip6 = NULL;
	nc->n_ip6 = 0;
}

void
netcfg_clear_dns(struct netcfg *nc)
{
	if (nc == NULL)
		return;
	str_array_free(nc->dns, nc->n_dns);
	nc->dns = NULL;
	nc->n_dns = 0;
}

/* ------------------------------------------------------------------ */
/* Serialization						      */
/* ------------------------------------------------------------------ */

static struct json_object *
str_array_to_json(char **arr, size_t n)
{
	struct json_object *a;
	size_t i;

	a = json_object_new_array();
	if (a == NULL)
		return (NULL);
	for (i = 0; i < n; i++)
		json_object_array_add(a, json_object_new_string(arr[i]));
	return (a);
}

char *
netcfg_to_json(const struct netcfg *nc)
{
	struct json_object *root;
	const char *rendered;
	char *out;

	if (nc == NULL)
		return (NULL);
	root = json_object_new_object();
	if (root == NULL)
		return (NULL);

	if (nc->vnet != -1)
		json_object_object_add(root, "vnet",
		    json_object_new_boolean(nc->vnet ? 1 : 0));
	if (nc->n_ip4 > 0)
		json_object_object_add(root, "ip4",
		    str_array_to_json(nc->ip4, nc->n_ip4));
	if (nc->n_ip6 > 0)
		json_object_object_add(root, "ip6",
		    str_array_to_json(nc->ip6, nc->n_ip6));
	if (nc->gateway4 != NULL)
		json_object_object_add(root, "gateway4",
		    json_object_new_string(nc->gateway4));
	if (nc->gateway6 != NULL)
		json_object_object_add(root, "gateway6",
		    json_object_new_string(nc->gateway6));
	if (nc->n_dns > 0)
		json_object_object_add(root, "dns",
		    str_array_to_json(nc->dns, nc->n_dns));
	if (nc->bridge != NULL)
		json_object_object_add(root, "bridge",
		    json_object_new_string(nc->bridge));

	rendered = json_object_to_json_string_ext(root,
	    JSON_C_TO_STRING_PRETTY);
	out = (rendered != NULL) ? strdup(rendered) : NULL;
	json_object_put(root);
	return (out);
}

static void
parse_str_array(struct json_object *root, const char *key,
	char ***arrp, size_t *np)
{
	struct json_object *arr, *el;
	size_t i, len;

	if (!json_object_object_get_ex(root, key, &arr) ||
	    json_object_get_type(arr) != json_type_array)
		return;
	len = json_object_array_length(arr);
	for (i = 0; i < len; i++) {
		el = json_object_array_get_idx(arr, i);
		if (el != NULL && json_object_get_type(el) == json_type_string)
			(void)str_array_add(arrp, np,
			    json_object_get_string(el));
	}
}

int
netcfg_parse(const char *json, struct netcfg *out)
{
	struct json_object *root, *v;

	if (out == NULL) {
		errno = EINVAL;
		return (-1);
	}
	netcfg_init(out);
	if (json == NULL) {
		errno = EINVAL;
		return (-1);
	}
	root = json_tokener_parse(json);
	if (root == NULL || json_object_get_type(root) != json_type_object) {
		if (root != NULL)
			json_object_put(root);
		errno = EINVAL;
		return (-1);
	}

	if (json_object_object_get_ex(root, "vnet", &v) &&
	    json_object_get_type(v) == json_type_boolean)
		out->vnet = json_object_get_boolean(v) ? 1 : 0;
	parse_str_array(root, "ip4", &out->ip4, &out->n_ip4);
	parse_str_array(root, "ip6", &out->ip6, &out->n_ip6);
	parse_str_array(root, "dns", &out->dns, &out->n_dns);
	if (json_object_object_get_ex(root, "gateway4", &v) &&
	    json_object_get_type(v) == json_type_string)
		out->gateway4 = strdup(json_object_get_string(v));
	if (json_object_object_get_ex(root, "gateway6", &v) &&
	    json_object_get_type(v) == json_type_string)
		out->gateway6 = strdup(json_object_get_string(v));
	if (json_object_object_get_ex(root, "bridge", &v) &&
	    json_object_get_type(v) == json_type_string)
		out->bridge = strdup(json_object_get_string(v));

	json_object_put(root);
	return (0);
}
