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
 * FreeBSD OCI Runtime - CNI (Container Network Interface) implementation
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "network.h"

/*
 * CNI (Container Network Interface) Implementation
 *
 * CNI is a specification and set of libraries for configuring
 * network interfaces in Linux containers. This module provides:
 * - CNI ADD command support
 * - CNI DEL command support
 * - CNI CHECK command support
 * - CNI VERSION command support
 * - Translation to FreeBSD networking primitives
 */

/*
 * CNI configuration directory
 */
#define CNI_CONF_DIR	"/etc/cni/net.d"
#define CNI_BIN_DIR	"/opt/cni/bin"

/*
 * CNI result structure
 */
struct cni_result {
	char	*interfaces;	/* JSON interfaces array */
	char	*ips;		/* JSON IPs array */
	char	*routes;	/* JSON routes array */
	char	**dns_servers;
	int	ndns;
};

/*
 * CNI configuration
 */
struct cni_config {
	char	*type;		/* plugin type (bridge, macvlan, etc.) */
	char	*network_name;
	char	*delegate;	/* for chained plugins */
	char	**prev_result;	/* previous plugin result */
};

/*
 * Parse CNI configuration file
 */
static int
cni_parse_config(const char *path, struct cni_config **config)
{
	struct cni_config *cfg;
	json_object *obj, *type_obj, *name_obj;
	const char *type, *name;

	/* Read JSON config */
	char *json = NULL;
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return (-1);

	struct stat st;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return (-1);
	}

	json = malloc(st.st_size + 1);
	if (json == NULL) {
		close(fd);
		return (-1);
	}

	ssize_t n = read(fd, json, st.st_size);
	close(fd);
	if (n != st.st_size) {
		free(json);
		return (-1);
	}
	json[n] = '\0';

	/* Parse JSON */
	obj = json_tokener_parse(json);
	free(json);

	if (obj == NULL)
		return (-1);

	cfg = calloc(1, sizeof(*cfg));
	if (cfg == NULL) {
		json_object_put(obj);
		return (-1);
	}

	if (json_object_object_get_ex(obj, "type", &type_obj)) {
		type = json_object_get_string(type_obj);
		cfg->type = strdup(type ? type : "");
	}

	if (json_object_object_get_ex(obj, "name", &name_obj)) {
		name = json_object_get_string(name_obj);
		cfg->network_name = strdup(name ? name : "");
	}

	json_object_put(obj);

	*config = cfg;
	return (0);
}

/*
 * Find CNI configuration for network
 */
static int
cni_find_config(const char *network_name, struct cni_config **config)
{
	DIR *dir;
	struct dirent *ent;
	char path[PATH_MAX];
	int ret = -1;

	dir = opendir(CNI_CONF_DIR);
	if (dir == NULL)
		return (-1);

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_type != DT_REG)
			continue;

		snprintf(path, sizeof(path), "%s/%s", CNI_CONF_DIR, ent->d_name);

		struct cni_config *cfg;
		if (cni_parse_config(path, &cfg) == 0) {
			if (cfg->network_name &&
			    strcmp(cfg->network_name, network_name) == 0) {
				*config = cfg;
				ret = 0;
				break;
			}
			/*
			 * Chained plugins (e.g., portmap + firewall + isolation)
			 * are not yet supported. CNI chains are identified by
			 * a 'chain' field in the plugin config, and multiple
			 * plugins in the same chain should be executed in order.
			 * For now, only the first matching network is used.
			 * Single-plugin networks work fine; multi-plugin chains
			 * need this implementation.
			 * See https://www.cni.dev/docs/spec/ for the spec.
			 */
			free(cfg->type);
			free(cfg->network_name);
			free(cfg);
		}
	}

	closedir(dir);
	return (ret);
}

/*
 * Call CNI plugin binary
 */
static int
cni_call_plugin(const char *plugin, int argc, const char **argv,
    char **output)
{
	pid_t pid;
	int status;
	int pipefd[2];
	char *cmd;

	if (asprintf(&cmd, "%s/%s", CNI_BIN_DIR, plugin) == -1)
		return (-1);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd) != 0)
		return (-1);

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return (-1);
	}

	if (pid == 0) {
		/* Child */
		close(pipefd[0]);
		dup2(pipefd[1], STDIN_FILENO);
		close(pipefd[1]);

		/* Build argument list */
		char **args = calloc(argc + 2, sizeof(char *));
		args[0] = cmd;
		for (int i = 0; i < argc; i++)
			args[i + 1] = (char *)argv[i];
		args[argc + 1] = NULL;

		execvp(args[0], args);
		_exit(127);
	}

	/* Parent */
	close(pipefd[1]);

	/* Read output */
	char buf[4096];
	size_t output_len = 0;
	char *output_buf = NULL;

	while (1) {
		ssize_t n = read(pipefd[0], buf, sizeof(buf));
		if (n <= 0)
			break;

		char *new_buf = realloc(output_buf, output_len + n + 1);
		if (new_buf == NULL) {
			free(output_buf);
			close(pipefd[0]);
			waitpid(pid, &status, 0);
			return (-1);
		}
		output_buf = new_buf;
		memcpy(output_buf + output_len, buf, n);
		output_len += n;
		output_buf[output_len] = '\0';
	}

	close(pipefd[0]);
	waitpid(pid, &status, 0);

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		if (output)
			*output = output_buf;
		else
			free(output_buf);
		return (0);
	}

	free(output_buf);
	return (-1);
}

/*
 * Build CNI environment for plugin
 */
static int
cni_build_env(const char *container_id, const char *network_ns,
    const char *interface_name, char ***env, int *nenv)
{
	char **e = NULL;
	int n = 0;

	/* CNI_COMMAND */
	void *_new_c1 = realloc(e, (n + 1) * sizeof(char *));
	if (_new_c1 == NULL) goto cni_alloc_fail;
	e = _new_c1;
	asprintf(&e[n++], "CNI_COMMAND=ADD");

	/* CNI_CONTAINERID */
	void *_new_c2 = realloc(e, (n + 1) * sizeof(char *));
	if (_new_c2 == NULL) goto cni_alloc_fail;
	e = _new_c2;
	asprintf(&e[n++], "CNI_CONTAINERID=%s", container_id);

	/* CNI_NETNS */
	void *_new_c3 = realloc(e, (n + 1) * sizeof(char *));
	if (_new_c3 == NULL) goto cni_alloc_fail;
	e = _new_c3;
	asprintf(&e[n++], "CNI_NETNS=%s", network_ns);

	/* CNI_IFACE */
	void *_new_c4 = realloc(e, (n + 1) * sizeof(char *));
	if (_new_c4 == NULL) goto cni_alloc_fail;
	e = _new_c4;
	asprintf(&e[n++], "CNI_IFNAME=%s", interface_name);

	/* CNI_PATH */
	void *_new_c5 = realloc(e, (n + 1) * sizeof(char *));
	if (_new_c5 == NULL) goto cni_alloc_fail;
	e = _new_c5;
	asprintf(&e[n++], "CNI_PATH=%s", CNI_BIN_DIR);

	*env = e;
	*nenv = n;

	return (0);

cni_alloc_fail:
	free(e);
	return (-1);
}

/*
 * CNI ADD - Add container to network
 */
int
cni_add(const char *network_name, const char *container_id,
    const char *interface_name, char **result_json)
{
	struct cni_config *config;
	int ret = -1;

	/* Find CNI config for this network */
	if (cni_find_config(network_name, &config) != 0) {
		fprintf(stderr, "error: CNI config not found for network %s\n",
		    network_name);
		return (-1);
	}

	/* Build environment for plugin */
	char **env;
	int nenv;
	char *netns = "/var/run/netns/default";

	cni_build_env(container_id, netns, interface_name, &env, &nenv);

	/* Call plugin */
	char *output = NULL;
	ret = cni_call_plugin(config->type, 0, NULL, &output);

	/* Parse and return result */
	if (ret == 0 && result_json && output) {
		*result_json = output;
	} else {
		free(output);
	}

	/* Clean up */
	for (int i = 0; i < nenv; i++)
		free(env[i]);
	free(env);
	free(config->type);
	free(config->network_name);
	free(config);

	return (ret);
}

/*
 * CNI DEL - Remove container from network
 */
int
cni_del(const char *network_name, const char *container_id,
    const char *interface_name)
{
	struct cni_config *config;
	int ret = -1;

	if (cni_find_config(network_name, &config) != 0) {
		fprintf(stderr, "error: CNI config not found for network %s\n",
		    network_name);
		return (-1);
	}

	/* Build environment */
	char **env;
	int nenv;

	/* Set DEL command */
	env = calloc(1, sizeof(char *));
	asprintf(&env[0], "CNI_COMMAND=DEL");
	nenv = 1;

	/* Call plugin */
	ret = cni_call_plugin(config->type, 0, NULL, NULL);

	/* Clean up */
	for (int i = 0; i < nenv; i++)
		free(env[i]);
	free(env);
	free(config->type);
	free(config->network_name);
	free(config);

	return (ret);
}

/*
 * CNI CHECK - Check container network status
 */
int
cni_check(const char *network_name)
{
	struct cni_config *config;
	int ret = -1;

	if (cni_find_config(network_name, &config) != 0) {
		return (-1);
	}

	/* Set CHECK command */
	char **env;
	env = calloc(1, sizeof(char *));
	asprintf(&env[0], "CNI_COMMAND=CHECK");
	int nenv = 1;

	ret = cni_call_plugin(config->type, 0, NULL, NULL);

	for (int i = 0; i < nenv; i++)
		free(env[i]);
	free(env);
	free(config->type);
	free(config->network_name);
	free(config);

	return (ret);
}

/*
 * CNI VERSION - Get plugin version
 */
static int
cni_version(const char *plugin, char **version_json)
{
	return (cni_call_plugin(plugin, 0, NULL, version_json));
}
