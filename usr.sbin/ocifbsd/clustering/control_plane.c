/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Control-plane state machine: a deterministic transition function over the
 * text commands committed to the Raft log. See control_plane.h.
 */

#include <stdlib.h>
#include <string.h>

#include "control_plane.h"

struct cp_service {
	char	name[128];
	int	replicas;
	char	image[512];
	char	vip[64];
};

struct cp_placement {
	char	service[128];
	int	replica_id;
	char	node[256];
	char	ip[64];
};

struct cp_state {
	struct cp_service	*svcs;
	int			 nsvcs, csvcs;
	struct cp_placement	*pls;
	int			 npls, cpls;
};

struct cp_state *
cp_new(void)
{
	return (calloc(1, sizeof(struct cp_state)));
}

void
cp_free(struct cp_state *st)
{
	if (st == NULL)
		return;
	free(st->svcs);
	free(st->pls);
	free(st);
}

static struct cp_service *
find_service(const struct cp_state *st, const char *name)
{
	for (int i = 0; i < st->nsvcs; i++)
		if (strcmp(st->svcs[i].name, name) == 0)
			return (&st->svcs[i]);
	return (NULL);
}

static struct cp_placement *
find_placement(const struct cp_state *st, const char *svc, int rid)
{
	for (int i = 0; i < st->npls; i++)
		if (st->pls[i].replica_id == rid &&
		    strcmp(st->pls[i].service, svc) == 0)
			return (&st->pls[i]);
	return (NULL);
}

/* Parse an integer token strictly (no trailing junk). Returns 0 on success. */
static int
parse_int(const char *s, int *out)
{
	char *end;
	long v;

	if (s == NULL || *s == '\0')
		return (-1);
	v = strtol(s, &end, 10);
	if (*end != '\0' || v < 0 || v > 100000)
		return (-1);
	*out = (int)v;
	return (0);
}

static int
svc_upsert(struct cp_state *st, const char *name, int replicas,
	const char *image)
{
	struct cp_service *s = find_service(st, name);

	if (s == NULL) {
		if (st->nsvcs >= st->csvcs) {
			int nc = st->csvcs ? st->csvcs * 2 : 8;
			struct cp_service *g = realloc(st->svcs,
			    (size_t)nc * sizeof(*g));
			if (g == NULL)
				return (-1);
			st->svcs = g;
			st->csvcs = nc;
		}
		s = &st->svcs[st->nsvcs++];
		memset(s, 0, sizeof(*s));
		strlcpy(s->name, name, sizeof(s->name));
	}
	s->replicas = replicas;
	if (image != NULL)
		strlcpy(s->image, image, sizeof(s->image));
	return (0);
}

static void
placements_remove_service(struct cp_state *st, const char *svc)
{
	int w = 0;

	for (int i = 0; i < st->npls; i++)
		if (strcmp(st->pls[i].service, svc) != 0)
			st->pls[w++] = st->pls[i];
	st->npls = w;
}

static int
svc_delete(struct cp_state *st, const char *name)
{
	int w = 0, found = 0;

	for (int i = 0; i < st->nsvcs; i++) {
		if (strcmp(st->svcs[i].name, name) == 0)
			found = 1;
		else
			st->svcs[w++] = st->svcs[i];
	}
	st->nsvcs = w;
	if (found)
		placements_remove_service(st, name);
	return (found ? 0 : -1);
}

static int
placement_assign(struct cp_state *st, const char *svc, int rid,
	const char *node)
{
	struct cp_placement *p;

	if (find_service(st, svc) == NULL)
		return (-1);
	p = find_placement(st, svc, rid);
	if (p == NULL) {
		if (st->npls >= st->cpls) {
			int nc = st->cpls ? st->cpls * 2 : 16;
			struct cp_placement *g = realloc(st->pls,
			    (size_t)nc * sizeof(*g));
			if (g == NULL)
				return (-1);
			st->pls = g;
			st->cpls = nc;
		}
		p = &st->pls[st->npls++];
		memset(p, 0, sizeof(*p));
		strlcpy(p->service, svc, sizeof(p->service));
		p->replica_id = rid;
	}
	strlcpy(p->node, node, sizeof(p->node));
	return (0);
}

static int
placement_unassign(struct cp_state *st, const char *svc, int rid)
{
	int w = 0, found = 0;

	for (int i = 0; i < st->npls; i++) {
		if (st->pls[i].replica_id == rid &&
		    strcmp(st->pls[i].service, svc) == 0)
			found = 1;
		else
			st->pls[w++] = st->pls[i];
	}
	st->npls = w;
	return (found ? 0 : -1);
}

int
cp_apply(struct cp_state *st, const char *cmd)
{
	char buf[1024];
	char *save = NULL, *op, *a1, *a2, *a3;
	int rid, rep;

	if (st == NULL || cmd == NULL)
		return (-1);
	if (strlcpy(buf, cmd, sizeof(buf)) >= sizeof(buf))
		return (-1);

	op = strtok_r(buf, " \t", &save);
	if (op == NULL)
		return (-1);

	if (strcmp(op, "CREATE") == 0) {
		a1 = strtok_r(NULL, " \t", &save);	/* service */
		a2 = strtok_r(NULL, " \t", &save);	/* replicas */
		a3 = strtok_r(NULL, " \t", &save);	/* image */
		if (a1 == NULL || a2 == NULL || a3 == NULL)
			return (-1);
		if (parse_int(a2, &rep) != 0)
			return (-1);
		return (svc_upsert(st, a1, rep, a3));
	}
	if (strcmp(op, "SCALE") == 0) {
		a1 = strtok_r(NULL, " \t", &save);
		a2 = strtok_r(NULL, " \t", &save);
		if (a1 == NULL || a2 == NULL || parse_int(a2, &rep) != 0)
			return (-1);
		if (find_service(st, a1) == NULL)
			return (-1);
		return (svc_upsert(st, a1, rep, NULL));
	}
	if (strcmp(op, "DELETE") == 0) {
		a1 = strtok_r(NULL, " \t", &save);
		if (a1 == NULL)
			return (-1);
		return (svc_delete(st, a1));
	}
	if (strcmp(op, "ASSIGN") == 0) {
		a1 = strtok_r(NULL, " \t", &save);	/* service */
		a2 = strtok_r(NULL, " \t", &save);	/* replica_id */
		a3 = strtok_r(NULL, " \t", &save);	/* node */
		if (a1 == NULL || a2 == NULL || a3 == NULL)
			return (-1);
		if (parse_int(a2, &rid) != 0)
			return (-1);
		return (placement_assign(st, a1, rid, a3));
	}
	if (strcmp(op, "UNASSIGN") == 0) {
		a1 = strtok_r(NULL, " \t", &save);
		a2 = strtok_r(NULL, " \t", &save);
		if (a1 == NULL || a2 == NULL || parse_int(a2, &rid) != 0)
			return (-1);
		return (placement_unassign(st, a1, rid));
	}
	if (strcmp(op, "VIP") == 0) {
		struct cp_service *sv;

		a1 = strtok_r(NULL, " \t", &save);	/* service */
		a2 = strtok_r(NULL, " \t", &save);	/* vip */
		if (a1 == NULL || a2 == NULL)
			return (-1);
		sv = find_service(st, a1);
		if (sv == NULL)
			return (-1);
		strlcpy(sv->vip, a2, sizeof(sv->vip));
		return (0);
	}
	if (strcmp(op, "ENDPOINT") == 0) {
		struct cp_placement *p;

		a1 = strtok_r(NULL, " \t", &save);	/* service */
		a2 = strtok_r(NULL, " \t", &save);	/* replica_id */
		a3 = strtok_r(NULL, " \t", &save);	/* ip */
		if (a1 == NULL || a2 == NULL || a3 == NULL ||
		    parse_int(a2, &rid) != 0)
			return (-1);
		p = find_placement(st, a1, rid);
		if (p == NULL)
			return (-1);		/* endpoint for an unplaced replica */
		strlcpy(p->ip, a3, sizeof(p->ip));
		return (0);
	}
	return (-1);
}

int
cp_service_count(const struct cp_state *st)
{
	return (st != NULL ? st->nsvcs : 0);
}

const char *
cp_service_name(const struct cp_state *st, int i)
{
	if (st == NULL || i < 0 || i >= st->nsvcs)
		return (NULL);
	return (st->svcs[i].name);
}

int
cp_service_placements(const struct cp_state *st, const char *svc, int *ids,
	int max)
{
	int n = 0;

	if (st == NULL || svc == NULL || ids == NULL || max < 0)
		return (-1);
	for (int i = 0; i < st->npls && n < max; i++)
		if (strcmp(st->pls[i].service, svc) == 0)
			ids[n++] = st->pls[i].replica_id;
	return (n);
}

int
cp_service_replicas(const struct cp_state *st, const char *svc)
{
	struct cp_service *s;

	if (st == NULL)
		return (-1);
	s = find_service(st, svc);
	return (s != NULL ? s->replicas : -1);
}

const char *
cp_service_image(const struct cp_state *st, const char *svc)
{
	struct cp_service *s;

	if (st == NULL)
		return (NULL);
	s = find_service(st, svc);
	return (s != NULL ? s->image : NULL);
}

const char *
cp_service_vip(const struct cp_state *st, const char *svc)
{
	struct cp_service *s;

	if (st == NULL)
		return (NULL);
	s = find_service(st, svc);
	return ((s != NULL && s->vip[0] != '\0') ? s->vip : NULL);
}

const char *
cp_replica_node(const struct cp_state *st, const char *svc, int replica_id)
{
	struct cp_placement *p;

	if (st == NULL)
		return (NULL);
	p = find_placement(st, svc, replica_id);
	return (p != NULL ? p->node : NULL);
}

const char *
cp_replica_endpoint(const struct cp_state *st, const char *svc, int replica_id)
{
	struct cp_placement *p;

	if (st == NULL)
		return (NULL);
	p = find_placement(st, svc, replica_id);
	return (p != NULL && p->ip[0] != '\0') ? p->ip : NULL;
}

int
cp_service_endpoints(const struct cp_state *st, const char *svc,
	char ips[][64], int max)
{
	int n = 0;

	if (st == NULL || svc == NULL || ips == NULL)
		return (0);
	for (int i = 0; i < st->npls && n < max; i++)
		if (strcmp(st->pls[i].service, svc) == 0 &&
		    st->pls[i].ip[0] != '\0')
			strlcpy(ips[n++], st->pls[i].ip, 64);
	return (n);
}

int
cp_node_replica_count(const struct cp_state *st, const char *node)
{
	int n = 0;

	if (st == NULL)
		return (0);
	for (int i = 0; i < st->npls; i++)
		if (strcmp(st->pls[i].node, node) == 0)
			n++;
	return (n);
}

int
cp_placement_count(const struct cp_state *st)
{
	return (st != NULL ? st->npls : 0);
}

int
cp_placement_at(const struct cp_state *st, int i, char *svc, size_t svclen,
	int *id, char *node, size_t nodelen)
{
	if (st == NULL || i < 0 || i >= st->npls)
		return (-1);
	if (svc != NULL)
		strlcpy(svc, st->pls[i].service, svclen);
	if (id != NULL)
		*id = st->pls[i].replica_id;
	if (node != NULL)
		strlcpy(node, st->pls[i].node, nodelen);
	return (0);
}
