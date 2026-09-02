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
 * Namespace management implementation
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libutil.h>
#include <login_cap.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sha256.h>

#include "namespace.h"
#include "../include/ocifbsd.h"
#include "../security/rctl.h"	/* struct rctl_limits */

/*
 * Base directory for namespace state. The code referenced OCIFBSD_VAR_DIR,
 * which was never defined (so the module did not compile); alias it to the
 * data dir.
 */
#ifndef OCIFBSD_VAR_DIR
#define OCIFBSD_VAR_DIR OCIFBSD_DATA_DIR
#endif

/* Forward declarations — these helpers are defined lower in the file but used
 * before their definitions (implicit-declaration errors otherwise). */
int ns_compare(struct namespace *a, struct namespace *b);
static int create_namespace_jail(struct namespace *ns);
static int delete_namespace_jail(struct namespace *ns);
static int apply_namespace_rctl(struct namespace *ns);
static int get_pod_rctl(const char *pod_name, struct rctl_limits *rlim);

/*
 * Define the red-black tree functions. namespace.h declares them with
 * RB_PROTOTYPE but no translation unit generated them, so ns_tree_RB_FIND
 * etc. were undefined — the module could never link into a binary. Generate
 * them here.
 */
RB_GENERATE(ns_tree, namespace, entry, ns_compare)

/* Global namespace registry */
static struct ns_tree namespace_registry;
static int namespace_registry_initialized = 0;
static pthread_mutex_t namespace_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_namespace_id = 1;

/*
 * Compare namespaces by name
 */
int
ns_compare(struct namespace *a, struct namespace *b)
{
    return (strcmp(a->name, b->name));
}

/*
 * Initialize namespace registry
 */
static void
init_namespace_registry(void)
{
    if (__sync_fetch_and_add(&namespace_registry_initialized, 0) == 0) {
        pthread_mutex_lock(&namespace_registry_lock);
        if (namespace_registry_initialized == 0) {
            RB_INIT(&namespace_registry);
            __sync_fetch_and_add(&namespace_registry_initialized, 1);
        }
        pthread_mutex_unlock(&namespace_registry_lock);
    }
}

/*
 * Get namespace state file path
 */
static char *
get_ns_state_path(const char *name)
{
    char *path;
    
    asprintf(&path, "%s/namespaces/%s.json", OCIFBSD_VAR_DIR, name);
    return (path);
}

/*
 * Save namespace state to disk
 */
static int
save_namespace_state(struct namespace *ns)
{
    FILE *fp;
    char *path;
    
    init_namespace_registry();
    
    path = get_ns_state_path(ns->name);
    if (path == NULL)
        return (-1);

    /*
     * Ensure the *parent directory* exists. Calling mkdirp on the full
     * path created a directory named "<name>.json", after which fopen
     * failed with EISDIR and state was never saved.
     */
    {
        char *slash = strrchr(path, '/');
        if (slash != NULL) {
            *slash = '\0';
            if (mkdirp(path, 0755) != 0 && errno != EEXIST) {
                free(path);
                return (-1);
            }
            *slash = '/';
        }
    }

    fp = fopen(path, "w");
    free(path);
    
    if (fp == NULL)
        return (-1);
    
    fprintf(fp, "{\n");
    fprintf(fp, "  \"name\": \"%s\",\n", ns->name);
    fprintf(fp, "  \"id\": %u,\n", ns->id);
    fprintf(fp, "  \"state\": %d,\n", ns->state);
    fprintf(fp, "  \"created\": %ld,\n", (long)ns->created);
    fprintf(fp, "  \"updated\": %ld,\n", (long)ns->updated);
    fprintf(fp, "  \"limits\": {\n");
    fprintf(fp, "    \"memory_limit\": %lu,\n", ns->limits.memory_limit);
    fprintf(fp, "    \"memory_reservation\": %lu,\n", ns->limits.memory_reservation);
    fprintf(fp, "    \"cpu_limit\": %lu,\n", ns->limits.cpu_limit);
    fprintf(fp, "    \"cpu_reservation\": %lu,\n", ns->limits.cpu_reservation);
    fprintf(fp, "    \"processes_max\": %lu,\n", ns->limits.processes_max);
    fprintf(fp, "    \"files_max\": %lu,\n", ns->limits.files_max);
    fprintf(fp, "    \"sockets_max\": %lu\n", ns->limits.sockets_max);
    fprintf(fp, "  },\n");
    fprintf(fp, "  \"mac_label\": \"%s\",\n", ns->mac_label);
    fprintf(fp, "  \"pod_limit\": %lu,\n", ns->pod_limit);
    fprintf(fp, "  \"pod_count\": %u,\n", ns->pod_count);
    fprintf(fp, "  \"service_limit\": %lu,\n", ns->service_limit);
    fprintf(fp, "  \"volume_limit\": %lu,\n", ns->volume_limit);
    fprintf(fp, "  \"secret_limit\": %lu\n", ns->secret_limit);
    fprintf(fp, "}\n");
    
    fclose(fp);
    return (0);
}

/*
 * Load namespace state from disk
 */
static struct namespace *
load_namespace_state(const char *name)
{
    struct namespace *ns;
    char *path;
    FILE *fp;
    char buf[1024];
    
    ns = calloc(1, sizeof(struct namespace));
    if (ns == NULL)
        return (NULL);
    
    strlcpy(ns->name, name, sizeof(ns->name));
    path = get_ns_state_path(name);
    
    if (path == NULL) {
        free(ns);
        return (NULL);
    }
    
    fp = fopen(path, "r");
    free(path);
    
    if (fp == NULL) {
        free(ns);
        return (NULL);
    }
    
    /* Simple state loading - for full JSON parsing, use json_parser */
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        if (strstr(buf, "\"id\":"))
            sscanf(buf, "  \"id\": %u", &ns->id);
        else if (strstr(buf, "\"state\":"))
            sscanf(buf, "  \"state\": %d", &ns->state);
        else if (strstr(buf, "\"created\":"))
            sscanf(buf, "  \"created\": %ld", (long *)&ns->created);
    }
    
    fclose(fp);
    return (ns);
}

/*
 * Validate a namespace name. It is interpolated into jail names, ZFS
 * dataset paths, pf anchors, and (historically) shell commands run as
 * root, so restrict it to a safe DNS-label-like charset. This is the
 * primary defense against command/path injection via the namespace name:
 * with metacharacters rejected, none of the downstream command builders
 * can be subverted. Must be 1..63 chars of [A-Za-z0-9._-] and may not
 * start with '.' or '-'.
 */
static bool
ns_name_is_valid(const char *name)
{
    size_t i, len;

    if (name == NULL)
        return (false);
    len = strlen(name);
    if (len == 0 || len > 63)
        return (false);
    if (name[0] == '.' || name[0] == '-')
        return (false);
    for (i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return (false);
    }
    return (true);
}

/*
 * Create a new namespace
 */
struct namespace *
ns_create(const char *name)
{
    struct namespace *ns, *existing;

    init_namespace_registry();

    if (!ns_name_is_valid(name)) {
        errno = EINVAL;
        return (NULL);
    }
    
    /* Check if namespace already exists */
    pthread_mutex_lock(&namespace_registry_lock);
    ns = calloc(1, sizeof(struct namespace));
    if (ns == NULL) {
        pthread_mutex_unlock(&namespace_registry_lock);
        return (NULL);
    }
    
    strlcpy(ns->name, name, sizeof(ns->name));
    
    existing = RB_FIND(ns_tree, &namespace_registry, ns);
    if (existing != NULL) {
        free(ns);
        pthread_mutex_unlock(&namespace_registry_lock);
        errno = EEXIST;
        return (NULL);
    }
    
    /* Initialize namespace */
    ns->id = __sync_fetch_and_add(&next_namespace_id, 1);
    ns->state = NS_STATE_ACTIVE;
    ns->created = time(NULL);
    ns->updated = time(NULL);
    
    /* Default limits */
    ns->limits.memory_limit = 0;          /* unlimited */
    ns->limits.memory_reservation = 0;
    ns->limits.cpu_limit = 0;
    ns->limits.cpu_reservation = 0;
    ns->limits.processes_max = 0;
    ns->limits.files_max = 0;
    ns->limits.sockets_max = 0;
    
    /* Default quotas */
    ns->pod_limit = 100;
    ns->service_limit = 50;
    ns->volume_limit = 50;
    ns->secret_limit = 100;
    
    /* Default MAC label */
    snprintf(ns->mac_label, sizeof(ns->mac_label), "ocifbsd/%s/*", name);
    
    /* Default network policy */
    ns->net_policy.allow_external = true;
    ns->net_policy.allow_internal = true;
    ns->net_policy.egress_limit = false;
    ns->net_policy.egress_rate = 0;
    
    /* Default volume policy */
    ns->vol_policy.allow_shared = true;
    ns->vol_policy.allow_exclusive = true;
    ns->vol_policy.allow_readonly = true;
    
    pthread_mutex_init(&ns->lock, NULL);
    
    RB_INSERT(ns_tree, &namespace_registry, ns);
    pthread_mutex_unlock(&namespace_registry_lock);
    
    /* Save to disk */
    save_namespace_state(ns);
    
    /* Create namespace directory in /var/run */
    char nsdir[PATH_MAX];
    snprintf(nsdir, sizeof(nsdir), "%s/namespaces/%s", OCIFBSD_VAR_DIR, name);
    mkdirp(nsdir, 0755);
    
    /* Create jail for namespace isolation */
    create_namespace_jail(ns);
    
    return (ns);
}

/*
 * Delete a namespace
 */
int
ns_delete(struct namespace *ns)
{
    int ret = 0;
    
    if (ns == NULL)
        return (-1);
    
    pthread_mutex_lock(&ns->lock);
    
    if (ns->pod_count > 0) {
        fprintf(stderr, "namespace '%s' has %u pods, cannot delete\n",
            ns->name, ns->pod_count);
        pthread_mutex_unlock(&ns->lock);
        errno = EBUSY;
        return (-1);
    }
    
    ns->state = NS_STATE_TERMINATING;
    pthread_mutex_unlock(&ns->lock);
    
    /* Remove jail */
    delete_namespace_jail(ns);
    
    /* Remove from registry */
    pthread_mutex_lock(&namespace_registry_lock);
    RB_REMOVE(ns_tree, &namespace_registry, ns);
    pthread_mutex_unlock(&namespace_registry_lock);
    
    /* Remove state file */
    char *path = get_ns_state_path(ns->name);
    if (path != NULL) {
        unlink(path);
        free(path);
    }
    
    /* Remove namespace directory */
    char nsdir[PATH_MAX];
    snprintf(nsdir, sizeof(nsdir), "%s/namespaces/%s", OCIFBSD_VAR_DIR, ns->name);
    rmdir(nsdir);
    
    pthread_mutex_destroy(&ns->lock);
    free(ns);
    
    return (ret);
}

/*
 * Get namespace by name
 */
struct namespace *
ns_get(const char *name)
{
    struct namespace ns_find;
    
    init_namespace_registry();
    
    if (name == NULL)
        return (NULL);
    
    strlcpy(ns_find.name, name, sizeof(ns_find.name));
    
    pthread_mutex_lock(&namespace_registry_lock);
    struct namespace *ns = RB_FIND(ns_tree, &namespace_registry, &ns_find);
    pthread_mutex_unlock(&namespace_registry_lock);
    
    if (ns == NULL) {
        /* Try loading from disk */
        ns = load_namespace_state(name);
        if (ns != NULL) {
            pthread_mutex_lock(&namespace_registry_lock);
            RB_INSERT(ns_tree, &namespace_registry, ns);
            pthread_mutex_unlock(&namespace_registry_lock);
        }
    }
    
    return (ns);
}

/*
 * List all namespaces
 */
struct namespace **
ns_list(int *count)
{
    struct namespace **result;
    struct namespace *ns;
    int alloc = 16;
    int n = 0;
    
    init_namespace_registry();
    
    *count = 0;
    result = calloc(alloc, sizeof(struct namespace *));
    if (result == NULL)
        return (NULL);
    
    pthread_mutex_lock(&namespace_registry_lock);
    RB_FOREACH(ns, ns_tree, &namespace_registry) {
        if (n >= alloc) {
            alloc *= 2;
            void *_new = realloc(result, alloc * sizeof(struct namespace *));
            if (_new == NULL) {
                pthread_mutex_unlock(&namespace_registry_lock);
                *count = 0;
                return (NULL);
            }
            result = _new;
        }
        result[n++] = ns;
    }
    pthread_mutex_unlock(&namespace_registry_lock);
    
    *count = n;
    return (result);
}

/*
 * Update namespace
 */
int
ns_update(struct namespace *ns)
{
    if (ns == NULL)
        return (-1);
    
    pthread_mutex_lock(&ns->lock);
    ns->updated = time(NULL);
    pthread_mutex_unlock(&ns->lock);
    
    return (save_namespace_state(ns));
}

/*
 * Set resource limits for namespace
 */
int
ns_set_limits(struct namespace *ns, struct ns_resource_limits *limits)
{
    if (ns == NULL || limits == NULL)
        return (-1);
    
    pthread_mutex_lock(&ns->lock);
    
    memcpy(&ns->limits, limits, sizeof(struct ns_resource_limits));
    ns->updated = time(NULL);
    
    pthread_mutex_unlock(&ns->lock);
    
    /* Apply limits via rctl */
    return (apply_namespace_rctl(ns));
}

/*
 * Get resource usage for namespace
 */
int
ns_get_usage(struct namespace *ns, struct ns_resource_limits *usage)
{
    uint64_t total_memory = 0;
    uint64_t total_cpu = 0;
    uint64_t total_procs = 0;
    uint64_t total_files = 0;

    if (ns == NULL || usage == NULL)
        return (-1);

    memset(usage, 0, sizeof(struct ns_resource_limits));

    /*
     * Aggregate per-pod resource figures. get_pod_rctl() is currently a stub
     * that zeroes the rctl_limits, so these totals are 0 until real RACCT
     * usage collection is wired; sum the fields that struct rctl_limits
     * actually defines (the code previously read non-existent *_usage members
     * and so never compiled).
     */
    char **pods;
    int pod_count;

    if (ns_list_pods(ns, &pods, &pod_count) == 0) {
        for (int i = 0; i < pod_count; i++) {
            struct rctl_limits pod_rctl;
            if (get_pod_rctl(pods[i], &pod_rctl) == 0) {
                total_memory += pod_rctl.memory_limit;
                total_cpu += pod_rctl.cpu_quota;
                total_procs += pod_rctl.proc_limit;
                total_files += pod_rctl.file_limit;
            }
            free(pods[i]);
        }
        free(pods);
    }
    
    usage->memory_limit = ns->limits.memory_limit;
    usage->memory_reservation = total_memory;
    usage->cpu_limit = ns->limits.cpu_limit;
    usage->cpu_reservation = total_cpu;
    usage->processes_max = total_procs;
    usage->files_max = total_files;
    
    return (0);
}

/*
 * Check quota for resource
 */
int
ns_check_quota(struct namespace *ns, const char *resource, uint32_t count)
{
    if (ns == NULL || resource == NULL)
        return (-1);
    
    pthread_mutex_lock(&ns->lock);
    
    int current = 0;
    uint64_t limit = 0;
    
    if (strcmp(resource, "pods") == 0) {
        current = ns->pod_count;
        limit = ns->pod_limit;
    } else if (strcmp(resource, "services") == 0) {
        current = ns->service_count;
        limit = ns->service_limit;
    } else if (strcmp(resource, "volumes") == 0) {
        current = ns->volume_count;
        limit = ns->volume_limit;
    } else if (strcmp(resource, "secrets") == 0) {
        current = ns->secret_count;
        limit = ns->secret_limit;
    } else {
        pthread_mutex_unlock(&ns->lock);
        errno = EINVAL;
        return (-1);
    }
    
    pthread_mutex_unlock(&ns->lock);
    
    if (limit > 0 && current + count > limit) {
        errno = EDQUOT;
        return (-1);
    }
    
    return (0);
}

/*
 * Add pod to namespace
 */
int
ns_add_pod(struct namespace *ns, const char *pod_name)
{
    if (ns == NULL || pod_name == NULL)
        return (-1);
    
    pthread_mutex_lock(&ns->lock);
    ns->pod_count++;
    ns->updated = time(NULL);
    pthread_mutex_unlock(&ns->lock);
    
    return (ns_update(ns));
}

/*
 * Remove pod from namespace
 */
int
ns_remove_pod(struct namespace *ns, const char *pod_name)
{
    if (ns == NULL || pod_name == NULL)
        return (-1);
    
    pthread_mutex_lock(&ns->lock);
    if (ns->pod_count > 0)
        ns->pod_count--;
    ns->updated = time(NULL);
    pthread_mutex_unlock(&ns->lock);
    
    return (ns_update(ns));
}

/*
 * List pods in namespace
 */
int
ns_list_pods(struct namespace *ns, char ***pods, int *count)
{
    char path[PATH_MAX];
    DIR *dir;
    struct dirent *ent;
    char **result = NULL;
    int alloc = 0;
    int n = 0;
    
    if (ns == NULL || pods == NULL || count == NULL)
        return (-1);
    
    *pods = NULL;
    *count = 0;
    
    snprintf(path, sizeof(path), "%s/namespaces/%s/pods",
        OCIFBSD_VAR_DIR, ns->name);
    
    dir = opendir(path);
    if (dir == NULL)
        return (-1);
    
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_REG)
            continue;
        
        if (n >= alloc) {
            alloc = alloc ? alloc * 2 : 16;
            void *_new = realloc(result, alloc * sizeof(char *));
            if (_new == NULL) {
                closedir(dir);
                return (-1);
            }
            result = _new;
        }
        
        result[n] = strdup(ent->d_name);
        if (result[n] != NULL)
            n++;
    }
    
    closedir(dir);
    
    *pods = result;
    *count = n;
    return (0);
}

/*
 * Configure network policy for namespace
 */
int
ns_configure_network(struct namespace *ns, struct ns_network_policy *policy)
{
    if (ns == NULL || policy == NULL)
        return (-1);
    
    pthread_mutex_lock(&ns->lock);
    memcpy(&ns->net_policy, policy, sizeof(struct ns_network_policy));
    ns->updated = time(NULL);
    pthread_mutex_unlock(&ns->lock);
    
    /* Apply firewall rules */
    return (ns_apply_firewall_rules(ns));
}

/*
 * Apply firewall rules for namespace
 */
int
ns_apply_firewall_rules(struct namespace *ns)
{
    char anchor_name[256];
    char rules_file[PATH_MAX];
    FILE *fp;
    
    if (ns == NULL)
        return (-1);
    
    /* Create per-namespace pf anchor */
    snprintf(anchor_name, sizeof(anchor_name), "ocifbsd-ns-%s", ns->name);
    snprintf(rules_file, sizeof(rules_file), "%s/namespaces/%s/pf.rules",
        OCIFBSD_VAR_DIR, ns->name);
    
    fp = fopen(rules_file, "w");
    if (fp == NULL)
        return (-1);
    
    /* Write pf rules for namespace */
    fprintf(fp, "# Namespace %s firewall rules\n", ns->name);
    fprintf(fp, "set skip on lo\n");
    
    /* Internal namespace traffic */
    if (ns->net_policy.allow_internal) {
        fprintf(fp, "# Allow internal namespace traffic\n");
        fprintf(fp, "pass quick on { ocifbsd%d } all\n", ns->id);
    }
    
    /* External traffic */
    if (!ns->net_policy.allow_external) {
        fprintf(fp, "# Block external traffic\n");
        fprintf(fp, "block quick all\n");
    }
    
    /* Egress limiting */
    if (ns->net_policy.egress_limit) {
        fprintf(fp, "# Egress rate limiting\n");
        fprintf(fp, "queue egress bandwidth %lubps\n", ns->net_policy.egress_rate);
    }
    
    /* Allowed networks */
    if (ns->net_policy.allowed_networks) {
        fprintf(fp, "# Allowed networks\n");
        fprintf(fp, "pass out quick to %s\n", ns->net_policy.allowed_networks);
    }
    
    /* Denied networks */
    if (ns->net_policy.denied_networks) {
        fprintf(fp, "# Denied networks\n");
        fprintf(fp, "block out quick to %s\n", ns->net_policy.denied_networks);
    }
    
    fclose(fp);
    
    /* Load rules into pf */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "pfctl -a '%s' -f '%s' 2>/dev/null", anchor_name, rules_file);
    system(cmd);
    
    return (0);
}

/*
 * Configure volume policy for namespace
 */
int
ns_configure_volumes(struct namespace *ns, struct ns_volume_policy *policy)
{
    if (ns == NULL || policy == NULL)
        return (-1);
    
    pthread_mutex_lock(&ns->lock);
    memcpy(&ns->vol_policy, policy, sizeof(struct ns_volume_policy));
    ns->updated = time(NULL);
    pthread_mutex_unlock(&ns->lock);
    
    return (ns_update(ns));
}

/*
 * Check volume access for namespace
 */
int
ns_check_volume_access(struct namespace *ns, const char *dataset)
{
    char **allowed, **denied;
    int i;
    
    if (ns == NULL || dataset == NULL)
        return (-1);
    
    pthread_mutex_lock(&ns->lock);
    
    denied = ns->vol_policy.denied_datasets;
    if (denied != NULL) {
        for (i = 0; denied[i] != NULL; i++) {
            if (strncmp(dataset, denied[i], strlen(denied[i])) == 0) {
                pthread_mutex_unlock(&ns->lock);
                errno = EACCES;
                return (-1);
            }
        }
    }
    
    allowed = ns->vol_policy.allowed_datasets;
    if (allowed != NULL && allowed[0] != NULL) {
        for (i = 0; allowed[i] != NULL; i++) {
            if (strncmp(dataset, allowed[i], strlen(allowed[i])) == 0) {
                pthread_mutex_unlock(&ns->lock);
                return (0);
            }
        }
        pthread_mutex_unlock(&ns->lock);
        errno = EACCES;
        return (-1);
    }
    
    pthread_mutex_unlock(&ns->lock);
    return (0);
}

/*
 * Set MAC label for namespace
 */
/*
 * Validate a MAC label before it is stored and later interpolated into a
 * jail(8) command line. ns_apply_mac_label() runs "jail -m label=<label> ..."
 * via system(3), so a label containing shell metacharacters (e.g.
 * "x; touch /pwned") would be command injection running as root. Accept only
 * the characters real MAC labels use — alphanumerics and / - _ . : , — which
 * excludes every shell metacharacter, whitespace, and quoting.
 */
static bool
ns_mac_label_is_valid(const char *label)
{
    size_t i, len;

    if (label == NULL)
        return (false);
    len = strlen(label);
    if (len == 0 || len >= 128)
        return (false);
    for (i = 0; i < len; i++) {
        char c = label[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' ||
            c == '_' || c == '.' || c == ':' || c == ','))
            return (false);
    }
    return (true);
}

int
ns_set_mac_label(struct namespace *ns, const char *label)
{
    if (ns == NULL || label == NULL)
        return (-1);
    if (!ns_mac_label_is_valid(label)) {
        errno = EINVAL;
        return (-1);
    }

    pthread_mutex_lock(&ns->lock);
    strlcpy(ns->mac_label, label, sizeof(ns->mac_label));
    ns->updated = time(NULL);
    pthread_mutex_unlock(&ns->lock);

    return (ns_update(ns));
}

/*
 * Apply MAC label to namespace
 */
int
ns_apply_mac_label(struct namespace *ns)
{
    char cmd[512];
    
    if (ns == NULL)
        return (-1);
    
    /* Set MAC label on namespace jail */
    snprintf(cmd, sizeof(cmd),
        "jail -m label=%s %s 2>/dev/null || true", ns->mac_label, ns->name);
    system(cmd);
    
    return (0);
}

/*
 * Get namespace stats
 */
int
ns_get_stats(struct namespace *ns, FILE *fp)
{
    struct ns_resource_limits usage;
    
    if (ns == NULL || fp == NULL)
        return (-1);
    
    ns_get_usage(ns, &usage);
    
    fprintf(fp, "Namespace: %s (id=%u)\n", ns->name, ns->id);
    fprintf(fp, "State: %s\n",
        ns->state == NS_STATE_ACTIVE ? "active" :
        ns->state == NS_STATE_SUSPENDED ? "suspended" : "terminating");
    fprintf(fp, "Created: %s", ctime(&ns->created));
    fprintf(fp, "Updated: %s", ctime(&ns->updated));
    fprintf(fp, "\nResource Limits:\n");
    fprintf(fp, "  Memory Limit: %lu bytes\n", ns->limits.memory_limit);
    fprintf(fp, "  Memory Usage: %lu bytes\n", usage.memory_reservation);
    fprintf(fp, "  CPU Limit: %lu%%\n", ns->limits.cpu_limit / 100);
    fprintf(fp, "  CPU Usage: %lu%%\n", usage.cpu_reservation / 100);
    fprintf(fp, "\nQuotas:\n");
    fprintf(fp, "  Pods: %u/%lu\n", ns->pod_count, ns->pod_limit);
    fprintf(fp, "  Services: %u/%lu\n", ns->service_count, ns->service_limit);
    fprintf(fp, "  Volumes: %u/%lu\n", ns->volume_count, ns->volume_limit);
    fprintf(fp, "  Secrets: %u/%lu\n", ns->secret_count, ns->secret_limit);
    fprintf(fp, "\nMAC Label: %s\n", ns->mac_label);
    
    return (0);
}

/*
 * Create namespace jail for isolation
 */
static int
create_namespace_jail(struct namespace *ns)
{
    char cmd[1024];
    int ret;
    
    /* Create ZFS dataset for namespace */
    snprintf(cmd, sizeof(cmd),
        "zfs create -o mountpoint=/namespaces/%s "
        " -o quota=%luG "
        " -o recordsize=128K "
        " ocifbsd/namespaces/%s 2>/dev/null || true",
        ns->name,
        ns->pod_limit * 10, /* 10GB per pod estimate */
        ns->name);
    system(cmd);
    
    /* Create jail configuration */
    snprintf(cmd, sizeof(cmd),
        "cat > /etc/jail.conf.d/ocifbsd-%s.conf << EOF\n"
        "ocifbsd-%s {\n"
        "  name = ocifbsd-%s;\n"
        "  path = /namespaces/%s;\n"
        "  host.hostname = ns-%s;\n"
        "  vnet;\n"
        "  vnet.interface = epair%d_create();\n"
        "  allow.raw_sockets = 1;\n"
        "  allow.chflags = 0;\n"
        "  allow.mount = 1;\n"
        "  enforce_statfs = 1;\n"
        "  children.cur max = %lu;\n"
        "  persist;\n"
        "}\n"
        "EOF\n",
        ns->name, ns->name, ns->name, ns->name, ns->name,
        ns->id, ns->pod_limit);
    
    ret = system(cmd);
    
    return (ret == 0 ? 0 : -1);
}

/*
 * Delete namespace jail
 */
static int
delete_namespace_jail(struct namespace *ns)
{
    char cmd[256];
    
    /* Remove jail configuration */
    snprintf(cmd, sizeof(cmd), "rm -f /etc/jail.conf.d/ocifbsd-%s.conf", ns->name);
    system(cmd);
    
    /* Destroy ZFS dataset */
    snprintf(cmd, sizeof(cmd), "zfs destroy -r ocifbsd/namespaces/%s 2>/dev/null || true", ns->name);
    system(cmd);
    
    return (0);
}

/*
 * Apply RCTL limits to namespace
 */
static int
apply_namespace_rctl(struct namespace *ns)
{
    char cmd[1024];
    
    if (ns == NULL)
        return (-1);
    
    /* Memory limits */
    if (ns->limits.memory_limit > 0) {
        snprintf(cmd, sizeof(cmd),
            "rctl -a jail:ocifbsd-%s:vmemoryuse:=%lu",
            ns->name, ns->limits.memory_limit);
        system(cmd);
    }
    
    /* Process limits */
    if (ns->limits.processes_max > 0) {
        snprintf(cmd, sizeof(cmd),
            "rctl -a jail:ocifbsd-%s:maxproc:=%lu",
            ns->name, ns->limits.processes_max);
        system(cmd);
    }
    
    /* File descriptor limits */
    if (ns->limits.files_max > 0) {
        snprintf(cmd, sizeof(cmd),
            "rctl -a jail:ocifbsd-%s:openfiles:=%lu",
            ns->name, ns->limits.files_max);
        system(cmd);
    }
    
    return (0);
}

/*
 * Get pod RCTL usage
 */
static int
get_pod_rctl(const char *pod_name, struct rctl_limits *rlim)
{
    if (pod_name == NULL || rlim == NULL)
        return (-1);

    /*
     * pod_name is a readdir(3) filename from the namespace's pods/
     * directory — untrusted. Reject anything that is not a safe label so
     * it cannot inject into the rctl invocation, and run rctl via
     * fork/exec (no shell).
     */
    if (!ns_name_is_valid(pod_name))
        return (-1);

    memset(rlim, 0, sizeof(struct rctl_limits));

    char filter[128];
    char buf[256];
    int fds[2];
    pid_t pid;
    FILE *fp;

    snprintf(filter, sizeof(filter), "jail:%s", pod_name);

    if (pipe(fds) != 0)
        return (-1);
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return (-1);
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        if (fds[1] != STDOUT_FILENO)
            close(fds[1]);
        execlp("rctl", "rctl", "-h", filter, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    fp = fdopen(fds[0], "r");
    if (fp == NULL) {
        close(fds[0]);
        waitpid(pid, NULL, 0);
        return (-1);
    }

    /*
     * Parse rctl(8) usage output into the rctl_limits fields (the struct has
     * no separate *_usage members; these fields carry the observed values,
     * summed the same way by ns_get_usage).
     */
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        if (strstr(buf, "vmemoryuse")) {
            sscanf(buf, "%*s %*s %lu", &rlim->memory_limit);
        } else if (strstr(buf, "pcpu")) {
            sscanf(buf, "%*s %*s %lu", &rlim->cpu_quota);
        } else if (strstr(buf, "nproc")) {
            sscanf(buf, "%*s %*s %lu", &rlim->proc_limit);
        } else if (strstr(buf, "openfiles")) {
            sscanf(buf, "%*s %*s %lu", &rlim->file_limit);
        }
    }

    fclose(fp);
    waitpid(pid, NULL, 0);
    return (0);
}
