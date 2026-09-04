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
 * Orchestration spec ownership: paired deep copy and release.
 *
 * The spec structs are plain aggregates that mix fixed-size character arrays
 * with heap pointers (env[], volumes[], ports, and a handful of scalars such
 * as user/group/failure_policy). Storing one used to be a shallow struct copy
 * -- `memcpy(pod->spec, spec, sizeof(*spec))` and friends -- which duplicates
 * the POINTERS. Caller and callee then both referenced the same strings with
 * no agreement about who owned them, so in practice nobody freed them: the
 * destructors released the array block but never its elements, and a spec
 * whose caller's stack frame had gone was still being read (rolling_update's
 * copies of failure_policy did exactly that).
 *
 * This file supplies the missing half of every allocation: for each spec kind
 * a *_copy() that deep-copies every owned pointer and a *_release() that frees
 * exactly what *_copy() allocated. They are strict duals -- release(copy(x))
 * leaks nothing and double-frees nothing -- so ownership is unambiguous:
 * after a copy the destination owns its own strings and the source is
 * untouched.
 *
 * Copy functions return 0 on success and -1 on allocation failure, and on
 * failure they release whatever they had already built, leaving the
 * destination zeroed. A partially constructed object is never handed back.
 */

#include <sys/param.h>

#include <stdlib.h>
#include <string.h>

#include "orchestration.h"

/*
 * Duplicate a NULL-tolerant string. Returns 0 on success (including the
 * src == NULL case, where *dst becomes NULL), -1 on allocation failure.
 */
static int
dup_opt(char **dst, const char *src)
{

	if (src == NULL) {
		*dst = NULL;
		return (0);
	}
	*dst = strdup(src);
	return (*dst != NULL ? 0 : -1);
}

/*
 * Duplicate the first n entries of a fixed-size array of owned strings.
 * Entries past n are set to NULL so a later release is well defined.
 */
static int
dup_array(char **dst, char *const *src, int n, int cap)
{
	int i;

	for (i = 0; i < cap; i++)
		dst[i] = NULL;
	if (n < 0 || n > cap)
		return (-1);
	for (i = 0; i < n; i++) {
		if (dup_opt(&dst[i], src[i]) != 0)
			return (-1);
	}
	return (0);
}

static void
free_array(char **a, int cap)
{
	int i;

	for (i = 0; i < cap; i++) {
		free(a[i]);
		a[i] = NULL;
	}
}

/*
 * Duplicate an array of port mappings (a pointer-free element type, so a
 * block copy is a deep copy here).
 */
static int
dup_ports(struct port_mapping **dst, const struct port_mapping *src, int n)
{

	*dst = NULL;
	if (n <= 0)
		return (0);
	/*
	 * A count with no array is an inconsistent source spec. Copying it
	 * "successfully" would produce a destination whose nports is positive
	 * while ports is NULL, and every consumer indexes ports[0..nports-1]
	 * without a NULL check -- so accept the failure here instead of
	 * handing back a spec that crashes on use.
	 */
	if (src == NULL)
		return (-1);
	*dst = calloc((size_t)n, sizeof(**dst));
	if (*dst == NULL)
		return (-1);
	memcpy(*dst, src, (size_t)n * sizeof(**dst));
	return (0);
}

/*
 * container_spec
 */
void
container_spec_release(struct container_spec *s)
{

	if (s == NULL)
		return;
	free_array(s->env, (int)(sizeof(s->env) / sizeof(s->env[0])));
	free_array(s->volumes, (int)(sizeof(s->volumes) / sizeof(s->volumes[0])));
	free_array(s->networks, (int)(sizeof(s->networks) / sizeof(s->networks[0])));
	free_array(s->dns_servers,
	    (int)(sizeof(s->dns_servers) / sizeof(s->dns_servers[0])));
	free(s->ports);
	s->ports = NULL;
	free(s->user);
	s->user = NULL;
	free(s->group);
	s->group = NULL;
	free(s->seccomp_profile);
	s->seccomp_profile = NULL;
	free(s->mac_label);
	s->mac_label = NULL;
}

int
container_spec_copy(struct container_spec *dst, const struct container_spec *src)
{

	if (dst == NULL || src == NULL)
		return (-1);

	/*
	 * Start from a block copy to carry every fixed-size field and scalar,
	 * then overwrite each owned pointer with a private duplicate. Doing it
	 * in that order means a field added to the struct later is copied by
	 * default; only fields that are actually owned need a line here.
	 */
	memcpy(dst, src, sizeof(*dst));
	dst->ports = NULL;
	memset(dst->env, 0, sizeof(dst->env));
	memset(dst->volumes, 0, sizeof(dst->volumes));
	memset(dst->networks, 0, sizeof(dst->networks));
	memset(dst->dns_servers, 0, sizeof(dst->dns_servers));
	dst->user = dst->group = dst->seccomp_profile = dst->mac_label = NULL;

	if (dup_array(dst->env, src->env, src->nenv,
	    (int)(sizeof(dst->env) / sizeof(dst->env[0]))) != 0 ||
	    dup_array(dst->volumes, src->volumes, src->nvolumes,
	    (int)(sizeof(dst->volumes) / sizeof(dst->volumes[0]))) != 0 ||
	    dup_array(dst->networks, src->networks, src->nnetworks,
	    (int)(sizeof(dst->networks) / sizeof(dst->networks[0]))) != 0 ||
	    dup_array(dst->dns_servers, src->dns_servers, src->ndns_servers,
	    (int)(sizeof(dst->dns_servers) / sizeof(dst->dns_servers[0]))) != 0 ||
	    dup_ports(&dst->ports, src->ports, src->nports) != 0 ||
	    dup_opt(&dst->user, src->user) != 0 ||
	    dup_opt(&dst->group, src->group) != 0 ||
	    dup_opt(&dst->seccomp_profile, src->seccomp_profile) != 0 ||
	    dup_opt(&dst->mac_label, src->mac_label) != 0) {
		container_spec_release(dst);
		memset(dst, 0, sizeof(*dst));
		return (-1);
	}
	return (0);
}

/*
 * pod_spec
 */
void
pod_spec_release(struct pod_spec *s)
{
	int i;

	if (s == NULL)
		return;
	if (s->containers != NULL) {
		for (i = 0; i < s->ncontainers; i++)
			container_spec_release(&s->containers[i]);
		free(s->containers);
		s->containers = NULL;
	}
	s->ncontainers = 0;
	free_array(s->volumes, (int)(sizeof(s->volumes) / sizeof(s->volumes[0])));
	free(s->node_selector);
	s->node_selector = NULL;
	free(s->affinity);
	s->affinity = NULL;
	free(s->service_account);
	s->service_account = NULL;
	free(s->image_pull_secrets);
	s->image_pull_secrets = NULL;
}

int
pod_spec_copy(struct pod_spec *dst, const struct pod_spec *src)
{
	int i;

	if (dst == NULL || src == NULL)
		return (-1);

	memcpy(dst, src, sizeof(*dst));
	dst->containers = NULL;
	dst->ncontainers = 0;
	memset(dst->volumes, 0, sizeof(dst->volumes));
	dst->node_selector = dst->affinity = NULL;
	dst->service_account = dst->image_pull_secrets = NULL;

	if (dup_array(dst->volumes, src->volumes, src->nvolumes,
	    (int)(sizeof(dst->volumes) / sizeof(dst->volumes[0]))) != 0 ||
	    dup_opt(&dst->node_selector, src->node_selector) != 0 ||
	    dup_opt(&dst->affinity, src->affinity) != 0 ||
	    dup_opt(&dst->service_account, src->service_account) != 0 ||
	    dup_opt(&dst->image_pull_secrets, src->image_pull_secrets) != 0)
		goto fail;

	if (src->containers != NULL && src->ncontainers > 0) {
		dst->containers = calloc((size_t)src->ncontainers,
		    sizeof(*dst->containers));
		if (dst->containers == NULL)
			goto fail;
		/*
		 * Bump ncontainers as each element is built, so a failure
		 * partway through releases exactly the elements that exist.
		 */
		for (i = 0; i < src->ncontainers; i++) {
			if (container_spec_copy(&dst->containers[i],
			    &src->containers[i]) != 0)
				goto fail;
			dst->ncontainers = i + 1;
		}
	}
	return (0);

fail:
	pod_spec_release(dst);
	memset(dst, 0, sizeof(*dst));
	return (-1);
}

/*
 * service_spec
 */
void
service_spec_release(struct service_spec *s)
{

	if (s == NULL)
		return;
	free_array(s->env, (int)(sizeof(s->env) / sizeof(s->env[0])));
	free_array(s->volumes, (int)(sizeof(s->volumes) / sizeof(s->volumes[0])));
	free_array(s->networks, (int)(sizeof(s->networks) / sizeof(s->networks[0])));
	free(s->ports);
	s->ports = NULL;
	free(s->update_config.failure_policy);
	s->update_config.failure_policy = NULL;
	free(s->scaling_config.metrics);
	s->scaling_config.metrics = NULL;
	free(s->network_config.session_affinity);
	s->network_config.session_affinity = NULL;
}

int
service_spec_copy(struct service_spec *dst, const struct service_spec *src)
{

	if (dst == NULL || src == NULL)
		return (-1);

	memcpy(dst, src, sizeof(*dst));
	dst->ports = NULL;
	memset(dst->env, 0, sizeof(dst->env));
	memset(dst->volumes, 0, sizeof(dst->volumes));
	memset(dst->networks, 0, sizeof(dst->networks));
	dst->update_config.failure_policy = NULL;
	dst->scaling_config.metrics = NULL;
	dst->network_config.session_affinity = NULL;

	if (dup_array(dst->env, src->env, src->nenv,
	    (int)(sizeof(dst->env) / sizeof(dst->env[0]))) != 0 ||
	    dup_array(dst->volumes, src->volumes, src->nvolumes,
	    (int)(sizeof(dst->volumes) / sizeof(dst->volumes[0]))) != 0 ||
	    dup_array(dst->networks, src->networks, src->nnetworks,
	    (int)(sizeof(dst->networks) / sizeof(dst->networks[0]))) != 0 ||
	    dup_ports(&dst->ports, src->ports, src->nports) != 0 ||
	    dup_opt(&dst->update_config.failure_policy,
	    src->update_config.failure_policy) != 0 ||
	    dup_opt(&dst->scaling_config.metrics,
	    src->scaling_config.metrics) != 0 ||
	    dup_opt(&dst->network_config.session_affinity,
	    src->network_config.session_affinity) != 0) {
		service_spec_release(dst);
		memset(dst, 0, sizeof(*dst));
		return (-1);
	}
	return (0);
}

/*
 * stack_spec
 */
void
stack_spec_release(struct stack_spec *s)
{
	int i;

	if (s == NULL)
		return;
	if (s->services != NULL) {
		for (i = 0; i < s->nservices; i++)
			service_spec_release(&s->services[i]);
		free(s->services);
		s->services = NULL;
	}
	s->nservices = 0;
	free_array(s->networks, (int)(sizeof(s->networks) / sizeof(s->networks[0])));
	free_array(s->volumes, (int)(sizeof(s->volumes) / sizeof(s->volumes[0])));
	free_array(s->configs, (int)(sizeof(s->configs) / sizeof(s->configs[0])));
	free_array(s->secrets, (int)(sizeof(s->secrets) / sizeof(s->secrets[0])));
	free(s->depends_on);
	s->depends_on = NULL;
}

int
stack_spec_copy(struct stack_spec *dst, const struct stack_spec *src)
{
	int i;

	if (dst == NULL || src == NULL)
		return (-1);

	memcpy(dst, src, sizeof(*dst));
	dst->services = NULL;
	dst->nservices = 0;
	memset(dst->networks, 0, sizeof(dst->networks));
	memset(dst->volumes, 0, sizeof(dst->volumes));
	memset(dst->configs, 0, sizeof(dst->configs));
	memset(dst->secrets, 0, sizeof(dst->secrets));
	dst->depends_on = NULL;

	if (dup_array(dst->networks, src->networks, src->nnetworks,
	    (int)(sizeof(dst->networks) / sizeof(dst->networks[0]))) != 0 ||
	    dup_array(dst->volumes, src->volumes, src->nvolumes,
	    (int)(sizeof(dst->volumes) / sizeof(dst->volumes[0]))) != 0 ||
	    dup_array(dst->configs, src->configs, src->nconfigs,
	    (int)(sizeof(dst->configs) / sizeof(dst->configs[0]))) != 0 ||
	    dup_array(dst->secrets, src->secrets, src->nsecrets,
	    (int)(sizeof(dst->secrets) / sizeof(dst->secrets[0]))) != 0 ||
	    dup_opt(&dst->depends_on, src->depends_on) != 0)
		goto fail;

	if (src->services != NULL && src->nservices > 0) {
		dst->services = calloc((size_t)src->nservices,
		    sizeof(*dst->services));
		if (dst->services == NULL)
			goto fail;
		for (i = 0; i < src->nservices; i++) {
			if (service_spec_copy(&dst->services[i],
			    &src->services[i]) != 0)
				goto fail;
			dst->nservices = i + 1;
		}
	}
	return (0);

fail:
	stack_spec_release(dst);
	memset(dst, 0, sizeof(*dst));
	return (-1);
}

/*
 * scheduling_decision
 *
 * scheduler_select_node() returns an OWNED decision whose failed_reason is a
 * strdup on the no-node-available path. Callers freed the struct but not the
 * string; give them one call that does both.
 */
void
scheduling_decision_free(struct scheduling_decision *d)
{

	if (d == NULL)
		return;
	free(d->failed_reason);
	free(d);
}
