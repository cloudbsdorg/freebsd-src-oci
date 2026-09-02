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
 * FreeBSD OCI Runtime - Networking
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <net/if.h>
#include <net/if_var.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sysexits.h>
#include <uuid.h>

extern int mkdirp(const char *path, mode_t mode);

#include "network.h"
#include "../include/ocifbsd.h"	/* ocifbsd_realloc_grow */

/*
 * Network state directory
 */
#define OCIFBSD_NETWORK_STATE_DIR	"/var/run/ocifbsd/networks"

/*
 * A network id becomes a path component under the root-owned state dir
 * (<dir>/<id>.json) that is fopen'd and unlink'd, so it MUST NOT contain '/',
 * "..", NUL, or a leading '.'. Without this, a network_id like "../../etc/rc"
 * is arbitrary root file read/overwrite/unlink (path traversal) in
 * network_get/network_delete/network_connect/etc.
 */
static bool
net_id_is_valid(const char *id)
{
	size_t i, len;

	if (id == NULL)
		return (false);
	len = strlen(id);
	if (len == 0 || len > 128)
		return (false);
	if (id[0] == '.')
		return (false);
	for (i = 0; i < len; i++) {
		char c = id[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
			return (false);
	}
	return (true);
}

/*
 * Run a command and return status
 */
static int
run_cmd(int argc, ...)
{
	va_list ap;
	char **argv;
	pid_t pid;
	int status;

	argv = calloc(argc + 1, sizeof(char *));
	if (argv == NULL)
		return (-1);

	va_start(ap, argc);
	for (int i = 0; i < argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);
	argv[argc] = NULL;

	pid = fork();
	if (pid < 0) {
		free(argv);
		return (-1);
	}

	if (pid == 0) {
		/*
		 * run_cmd is for side-effecting commands (ifconfig up, name,
		 * addm). Some of them echo to stdout (e.g. `ifconfig <if> name
		 * <new>` prints the new name), which would corrupt the CLI's
		 * own stdout — the `network create` id line in particular. Send
		 * the child's stdout to /dev/null; stderr is left attached so
		 * real errors remain visible. Output that must be read back is
		 * captured separately via run_cmd_output / net_capture_argv.
		 */
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			(void)dup2(devnull, STDOUT_FILENO);
			if (devnull > STDERR_FILENO)
				(void)close(devnull);
		}
		closefrom(STDERR_FILENO + 1);
		execvp(argv[0], argv);
		_exit(127);
	}

	free(argv);
	if (waitpid(pid, &status, 0) < 0)
		return (-1);

	if (WIFEXITED(status))
		return (WEXITSTATUS(status));

	return (-1);
}

/*
 * Run a command with output capture
 */
static int
run_cmd_output(char **output, int argc, ...)
{
	va_list ap;
	char **argv;
	FILE *fp;
	int status;
	char buf[1024];
	char *result = NULL;
	size_t result_len = 0;

	argv = calloc(argc + 1, sizeof(char *));
	if (argv == NULL)
		return (-1);

	va_start(ap, argc);
	for (int i = 0; i < argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);
	argv[argc] = NULL;

	/*
	 * Execute via fork/exec with the argv directly — never through a
	 * shell. Interface, jail, and endpoint names flow into these
	 * commands; joining them into a popen() string allowed root command
	 * injection (e.g. a name containing ';', '$(...)', or backticks).
	 */
	int fds[2];
	pid_t pid;

	if (pipe(fds) != 0) {
		free(argv);
		return (-1);
	}

	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		free(argv);
		return (-1);
	}
	if (pid == 0) {
		close(fds[0]);
		if (dup2(fds[1], STDOUT_FILENO) < 0)
			_exit(127);
		/*
		 * Merge stderr into the captured pipe so a command's diagnostics
		 * (which ifconfig writes to stderr) are captured with its output
		 * instead of leaking to the user's terminal. Callers that parse a
		 * created interface name read stdout, which ifconfig writes only
		 * on success, so the merge is harmless for them.
		 */
		if (dup2(fds[1], STDERR_FILENO) < 0)
			_exit(127);
		if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO)
			close(fds[1]);
		execvp(argv[0], argv);
		_exit(127);
	}

	close(fds[1]);
	(void)fp;
	for (;;) {
		ssize_t n = read(fds[0], buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		char *newp = realloc(result, result_len + (size_t)n + 1);
		if (newp == NULL) {
			free(result);
			result = NULL;
			result_len = 0;
			break;
		}
		result = newp;
		memcpy(result + result_len, buf, (size_t)n);
		result_len += (size_t)n;
		result[result_len] = '\0';
	}
	close(fds[0]);
	free(argv);

	if (waitpid(pid, &status, 0) < 0)
		status = -1;

	if (output != NULL && result != NULL) {
		*output = result;
	} else {
		free(result);
	}

	if (WIFEXITED(status))
		return (WEXITSTATUS(status));

	return (-1);
}

/*
 * Run a command (NULL-terminated argv) via fork/exec and capture its
 * stdout into *output (caller frees). No shell is involved, so arguments
 * containing shell metacharacters are safe. Returns the child exit code,
 * or -1 on failure. Shared with vnet.c/bridge.c to replace popen().
 */
int
net_capture_argv(char **output, char *const argv[])
{
	int fds[2];
	pid_t pid;
	int status;
	char buf[1024];
	char *result = NULL;
	size_t result_len = 0;

	if (output != NULL)
		*output = NULL;
	if (pipe(fds) != 0)
		return (-1);
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return (-1);
	}
	if (pid == 0) {
		close(fds[0]);
		if (dup2(fds[1], STDOUT_FILENO) < 0)
			_exit(127);
		/*
		 * Merge stderr into the captured pipe so a command's diagnostics
		 * (which ifconfig writes to stderr) are captured with its output
		 * instead of leaking to the user's terminal. Callers that parse a
		 * created interface name read stdout, which ifconfig writes only
		 * on success, so the merge is harmless for them.
		 */
		if (dup2(fds[1], STDERR_FILENO) < 0)
			_exit(127);
		if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO)
			close(fds[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	for (;;) {
		ssize_t n = read(fds[0], buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		char *newp = realloc(result, result_len + (size_t)n + 1);
		if (newp == NULL) {
			free(result);
			result = NULL;
			result_len = 0;
			break;
		}
		result = newp;
		memcpy(result + result_len, buf, (size_t)n);
		result_len += (size_t)n;
		result[result_len] = '\0';
	}
	close(fds[0]);
	if (waitpid(pid, &status, 0) < 0)
		status = -1;
	if (output != NULL && result != NULL)
		*output = result;
	else
		free(result);
	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	return (-1);
}

/*
 * Network initialization
 */
int
network_init(void)
{
	/* Ensure network state directory exists */
	if (mkdir(OCIFBSD_NETWORK_STATE_DIR, 0755) != 0 && errno != EEXIST)
		return (-1);

	return (0);
}

int
network_cleanup(void)
{
	/* Clean up any stale state */
	return (0);
}

/*
 * Bridge management
 */
int
bridge_create(const char *name)
{
	char *out = NULL;
	char created[IFNAMSIZ];
	size_t len;

	/* Check if bridge already exists */
	if (bridge_exists(name))
		return (0);  /* Already exists, OK */

	/*
	 * `ifconfig bridge create <name>` does not name the bridge — the unit is
	 * kernel-assigned and a bare name argument is rejected ("bad value").
	 * Create an auto-assigned bridge, read its name, then rename it to the
	 * requested name. (The previous code used the rejected form, so creating
	 * a bridge always failed.)
	 */
	if (net_capture_argv(&out, (char *const[]){ "ifconfig", "bridge",
	    "create", NULL }) != 0 || out == NULL) {
		free(out);
		return (-1);
	}
	len = strcspn(out, " \t\r\n");
	if (len == 0 || len >= sizeof(created)) {
		free(out);
		return (-1);
	}
	memcpy(created, out, len);
	created[len] = '\0';
	free(out);

	if (strcmp(created, name) != 0 &&
	    run_cmd(4, "ifconfig", created, "name", name) != 0) {
		(void)run_cmd(3, "ifconfig", created, "destroy");
		return (-1);
	}

	/* Bring it up */
	if (run_cmd(3, "ifconfig", name, "up") != 0)
		return (-1);

	return (0);
}

int
bridge_delete(const char *name)
{
	/* Bring interface down first */
	run_cmd(3, "ifconfig", name, "down");

	/*
	 * Destroy is `ifconfig <name> destroy` — the interface name is the
	 * first argument. The previous form `ifconfig bridge destroy <name>`
	 * made ifconfig treat the literal "bridge" as the interface ("interface
	 * bridge does not exist") and leaked every bridge network_delete tried
	 * to remove.
	 */
	return (run_cmd(3, "ifconfig", name, "destroy"));
}

int
bridge_add_interface(const char *bridge, const char *iface)
{
	return (run_cmd(4, "ifconfig", bridge, "addm", iface));
}

int
bridge_remove_interface(const char *bridge, const char *iface)
{
	return (run_cmd(4, "ifconfig", bridge, "deletem", iface));
}

int
bridge_set_mtu(const char *bridge, int mtu)
{
	char mtu_str[16];
	/* "mtu" and the value must be separate argv elements. */
	snprintf(mtu_str, sizeof(mtu_str), "%d", mtu);
	return (run_cmd(4, "ifconfig", bridge, "mtu", mtu_str));
}

int
bridge_list_interfaces(const char *bridge, char ***interfaces, int *ninterfaces)
{
	char *output = NULL;
	char **ifaces = NULL;
	int nifaces = 0;
	char *line, *save;

	*interfaces = NULL;
	*ninterfaces = 0;

	if (run_cmd_output(&output, 2, "ifconfig", bridge) != 0) {
		free(output);
		return (-1);
	}

	/* Parse output for member interfaces */
	line = strtok_r(output, "\n", &save);
	while (line != NULL) {
		if (strstr(line, "member:")) {
			char *p = strchr(line, ':');
			if (p) {
				p++; /* Skip ':' */
				while (*p == ' ')
					p++;
				char *end = p + strlen(p) - 1;
				while (end > p && (*end == '\n' || *end == ' '))
					*end-- = '\0';

				if (*p) {
					if (ocifbsd_realloc_grow((void **)&ifaces, (nifaces + 1) * sizeof(char *)) != 0)
						continue;
					ifaces[nifaces++] = strdup(p);
				}
			}
		}
		line = strtok_r(NULL, "\n", &save);
	}

	free(output);

	*interfaces = ifaces;
	*ninterfaces = nifaces;

	return (0);
}

bool
bridge_exists(const char *name)
{
	/*
	 * An interface exists iff the kernel maps its name to an index. This is
	 * a single getifaddrs-backed lookup in libc — no fork/exec of ifconfig
	 * and no parsing its human-readable output for "does not exist" (which
	 * would also misfire on any interface whose config text happened to
	 * contain that phrase).
	 */
	if (name == NULL)
		return (false);
	return (if_nametoindex(name) != 0);
}

/*
 * epair management
 */

/*
 * Given the 'a' side of an epair (as printed by `ifconfig epair create`,
 * e.g. "epair0a"), write the peer 'b' name into buf. Returns 0 on success,
 * -1 if aname is empty, does not end in 'a', or does not fit. Pure and
 * root-free so the naming contract can be unit-tested.
 */
int
epair_peer_name(const char *aname, char *buf, size_t buflen)
{
	size_t len;

	if (aname == NULL || buf == NULL)
		return (-1);
	len = strlen(aname);
	if (len == 0 || len + 1 > buflen)
		return (-1);
	if (aname[len - 1] != 'a')
		return (-1);
	memcpy(buf, aname, len);
	buf[len - 1] = 'b';
	buf[len] = '\0';
	return (0);
}

int
epair_create(const char *prefix, char **side_a, char **side_b)
{
	char *out = NULL;
	char name[IFNAMSIZ];
	char *a, *b;
	size_t len;
	int rc;

	/*
	 * epair units are assigned by the kernel; a caller-chosen name is not
	 * honored. `ifconfig epair create` creates the next free pair and prints
	 * the 'a' side (e.g. "epair0a"); the peer is the same name ending in
	 * 'b'. The previous code ran `ifconfig epair create <fabricated-name>`
	 * (which the kernel ignores) and then returned names like "ocifbsda0"
	 * that never existed, so nothing downstream could find the interface.
	 * The prefix is retained in the API for source compatibility but unused.
	 */
	(void)prefix;

	rc = net_capture_argv(&out, (char *const[]){ "ifconfig", "epair",
	    "create", NULL });
	if (rc != 0 || out == NULL) {
		free(out);
		return (-1);
	}

	/* Take the first whitespace-delimited token as the interface name. */
	len = strcspn(out, " \t\r\n");
	if (len == 0 || len >= sizeof(name)) {
		free(out);
		return (-1);
	}
	memcpy(name, out, len);
	name[len] = '\0';
	free(out);

	a = malloc(IFNAMSIZ);
	b = malloc(IFNAMSIZ);
	if (a == NULL || b == NULL) {
		free(a);
		free(b);
		return (-1);
	}
	/* The created side must end in 'a'; derive the 'b' peer from it. */
	strlcpy(a, name, IFNAMSIZ);
	if (epair_peer_name(name, b, IFNAMSIZ) != 0) {
		free(a);
		free(b);
		return (-1);
	}

	*side_a = a;
	*side_b = b;

	return (0);
}

int
epair_delete(const char *epair)
{
	return (run_cmd(3, "ifconfig", epair, "destroy"));
}

int
epair_set_mtu(const char *epair, int mtu)
{
	char mtu_str[16];
	char a_name[IFNAMSIZ], b_name[IFNAMSIZ];
	size_t l;

	/*
	 * The two epair interfaces differ only in the trailing 'a'/'b'
	 * (e.g. epair0a / epair0b). Derive the peer from the LAST character;
	 * strchr(a_name, 'a') matched the 'a' inside the "epair" prefix and
	 * produced a bogus peer name.
	 */
	strlcpy(a_name, epair, sizeof(a_name));
	strlcpy(b_name, epair, sizeof(b_name));
	l = strlen(b_name);
	if (l == 0)
		return (-1);
	if (b_name[l - 1] == 'a')
		b_name[l - 1] = 'b';
	else if (b_name[l - 1] == 'b')
		b_name[l - 1] = 'a';
	else
		return (-1);

	snprintf(mtu_str, sizeof(mtu_str), "%d", mtu);

	/* Set MTU on both sides (mtu and value are separate argv elements). */
	if (run_cmd(4, "ifconfig", a_name, "mtu", mtu_str) != 0)
		return (-1);

	return (run_cmd(4, "ifconfig", b_name, "mtu", mtu_str));
}

bool
epair_exists(const char *epair)
{
	char *output = NULL;
	bool exists = false;

	if (run_cmd_output(&output, 2, "ifconfig", epair) == 0) {
		if (strstr(output, "does not exist") == NULL)
			exists = true;
	}

	free(output);
	return (exists);
}

/*
 * IP address management
 */
static uint32_t ipam_allocated = 0;

int
ipam_alloc(struct ipam_range *range, struct in_addr *addr)
{
	uint32_t start, end, current;

	if (range == NULL || addr == NULL)
		return (-1);

	start = ntohl(range->start.s_addr);
	end = ntohl(range->end.s_addr);

	/* Skip gateway (first address) */
	if (ipam_allocated == 0)
		ipam_allocated = start + 1;

	/* Find next available address */
	current = ipam_allocated;
	while (current <= end) {
		if ((ipam_allocated & (1 << (current % 32))) == 0) {
			/* Not allocated, use it */
			ipam_allocated |= (1 << (current % 32));
			addr->s_addr = htonl(current);
			return (0);
		}
		current++;
	}

	errno = ENOSPC;
	return (-1);
}

int
ipam_release(struct ipam_range *range __unused, struct in_addr *addr)
{
	uint32_t ip;

	if (addr == NULL)
		return (-1);

	ip = ntohl(addr->s_addr);
	ipam_allocated &= ~(1 << (ip % 32));

	return (0);
}

/*
 * Network creation
 */
int
network_create(struct network_config *config)
{
	char bridge_name[64];
	int ret = -1;

	if (config == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/* Generate bridge name */
	snprintf(bridge_name, sizeof(bridge_name), "ocifbsd%s",
	    config->name[0] ? config->name : "br");

	/* Create bridge for bridge networks */
	if (config->type == NETWORK_TYPE_BRIDGE) {
		if (bridge_create(bridge_name) != 0) {
			fprintf(stderr, "error: failed to create bridge %s\n",
			    bridge_name);
			return (-1);
		}

		/* Set MTU if specified */
		if (config->mtu) {
			bridge_set_mtu(bridge_name, atoi(config->mtu));
		}

		/*
		 * The network's gateway address lives on the bridge itself, not
		 * on a dangling epair. Per-container epairs are created and
		 * attached later by network_connect(); creating one here left a
		 * stranded interface (with a duplicated gateway IP) that
		 * network_delete could not identify or reap — a leak on every
		 * create/rm cycle. Assign the gateway to the bridge and stop.
		 */
		if (config->gateway) {
			run_cmd(6, "ifconfig", bridge_name, config->gateway,
			    "netmask", "255.255.255.0", "up");
		}
	}

	/* Save network configuration */
	char state_file[PATH_MAX];
	snprintf(state_file, sizeof(state_file), "%s/%s.json",
	    OCIFBSD_NETWORK_STATE_DIR, config->id);

	FILE *f = fopen(state_file, "w");
	if (f) {
		fprintf(f, "{\n");
		fprintf(f, "  \"name\": \"%s\",\n", config->name);
		fprintf(f, "  \"id\": \"%s\",\n", config->id);
		fprintf(f, "  \"type\": %d,\n", config->type);
		fprintf(f, "  \"driver\": \"%s\",\n",
		    config->driver ? config->driver : "bridge");
		fprintf(f, "  \"bridge\": \"%s\",\n", bridge_name);
		fprintf(f, "  \"subnet\": \"%s\",\n",
		    config->subnet ? config->subnet : "");
		fprintf(f, "  \"gateway\": \"%s\",\n",
		    config->gateway ? config->gateway : "");
		fprintf(f, "  \"internal\": %s,\n",
		    config->internal ? "true" : "false");
		fprintf(f, "  \"dns_servers\": []\n");
		fprintf(f, "}\n");
		fclose(f);
		ret = 0;
	}

	return (ret);
}

int
network_delete(const char *network_id)
{
	if (!net_id_is_valid(network_id))
		return (-1);
	char state_file[PATH_MAX];
	char bridge_name[64] = "";	/* must be empty if not parsed below */

	snprintf(state_file, sizeof(state_file), "%s/%s.json",
	    OCIFBSD_NETWORK_STATE_DIR, network_id);

	/* Get bridge name from state file */
	FILE *f = fopen(state_file, "r");
	if (f) {
		char buf[256];
		while (fgets(buf, sizeof(buf), f)) {
			if (strstr(buf, "bridge")) {
				char *p = strchr(buf, ':');
				if (p) {
					p++;
					while (*p == ' ' || *p == '"')
						p++;
					char *end = strchr(p, '"');
					if (end)
						*end = '\0';
					strlcpy(bridge_name, p, sizeof(bridge_name));
				}
			}
		}
		fclose(f);
	}

	/* Delete state file */
	unlink(state_file);

	/* Delete bridge */
	if (bridge_name[0]) {
		bridge_delete(bridge_name);
	}

	return (0);
}

int
network_connect(const char *network_id, const char *container_id,
	struct endpoint **ep)
{
	if (!net_id_is_valid(network_id))
		return (-1);
	struct endpoint *endpoint = NULL;
	char state_file[PATH_MAX];
	char bridge_name[64] = "";
	char *side_a = NULL, *side_b = NULL;

	if (network_id == NULL || container_id == NULL || ep == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/* Read network state */
	snprintf(state_file, sizeof(state_file), "%s/%s.json",
	    OCIFBSD_NETWORK_STATE_DIR, network_id);

	FILE *f = fopen(state_file, "r");
	if (f) {
		char buf[256];
		while (fgets(buf, sizeof(buf), f)) {
			if (strstr(buf, "bridge")) {
				char *p = strchr(buf, ':');
				if (p) {
					p++;
					while (*p == ' ' || *p == '"')
						p++;
					char *end = strchr(p, '"');
					if (end)
						*end = '\0';
					strlcpy(bridge_name, p, sizeof(bridge_name));
				}
			}
		}
		fclose(f);
	}

	if (bridge_name[0] == '\0') {
		fprintf(stderr, "error: network %s not found\n", network_id);
		return (-1);
	}

	/* Create endpoint */
	endpoint = calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return (-1);

	uuid_t uuid;
	uint32_t status;
	char *uuid_str = NULL;

	uuid_create(&uuid, &status);
	if (status == uuid_s_ok) {
		uuid_to_string(&uuid, &uuid_str, &status);
	}
	endpoint->id = uuid_str;
	endpoint->network_id = strdup(network_id);
	endpoint->container_id = strdup(container_id);
	endpoint->interface_name = strdup("eth0");

	/* Create epair */
	if (epair_create("ocifbsd", &side_a, &side_b) == 0) {
		/* Add host side to bridge */
		bridge_add_interface(bridge_name, side_a);

		/* Jail side goes to container */
		endpoint->interface_name = side_b;  /* jail will use this */

		/*
		 * Proper IPAM (IP Address Management) is not yet implemented.
		 * The current code does not assign an IP from a pool; the
		 * caller is expected to configure it externally. A real
		 * implementation needs to:
		 *
		 *   1. Track which IP ranges are in use per network
		 *      (e.g., 10.0.0.0/24 split into /30 subnets)
		 *   2. Allocate a free /30 (2 usable addresses) on request
		 *   3. Persist the allocation in the network state JSON
		 *   4. Free the allocation when the endpoint is deleted
		 *
		 * This is a NETWORK CORRECTNESS issue: without IPAM, two
		 * endpoints can be assigned the same IP, breaking
		 * connectivity. See MIGRATION.md for the full plan.
		 */
		(void)0;

		free(side_a);
	}

	*ep = endpoint;
	return (0);
}

int
network_disconnect(const char *network_id, const char *container_id)
{
	char state_file[PATH_MAX];

	/*
	 * Endpoint tracking is not yet implemented. For now, just
	 * remove the per-container state file if it exists. When
	 * endpoint tracking is added (see endpoint_find()), this
	 * should also remove the endpoint from the bridge and
	 * destroy the epair interface.
	 */
	(void)network_id;	/* unused for now */
	(void)container_id;	/* unused for now */

	snprintf(state_file, sizeof(state_file),
	    "%s/endpoint_%s_%s.json",
	    OCIFBSD_NETWORK_STATE_DIR, network_id, container_id);
	if (unlink(state_file) == -1 && errno != ENOENT) {
		/* State file doesn't exist (or can't be removed) - not fatal */
		return (0);
	}
	return (0);
}

/*
 * Extract a JSON string field from one line of the form
 *   "key": "value"
 * as written by network_create(). Returns true and copies the (possibly
 * empty) value into out when the key is present on this line. This is a
 * deliberately small line-oriented reader matched to our own writer, not a
 * general JSON parser.
 */
static bool
json_str_field(const char *line, const char *key, char *out, size_t outlen)
{
	char needle[64];
	const char *p, *start, *end;
	size_t n;

	snprintf(needle, sizeof(needle), "\"%s\"", key);
	p = strstr(line, needle);
	if (p == NULL)
		return (false);
	p += strlen(needle);
	p = strchr(p, ':');		/* move past the key to the colon */
	if (p == NULL)
		return (false);
	start = strchr(p, '"');		/* opening quote of the value */
	if (start == NULL)
		return (false);
	start++;
	end = strchr(start, '"');	/* closing quote */
	if (end == NULL)
		return (false);
	n = (size_t)(end - start);
	if (n >= outlen)
		n = outlen - 1;
	memcpy(out, start, n);
	out[n] = '\0';
	return (true);
}

struct network_config *
network_get(const char *network_id)
{
	if (!net_id_is_valid(network_id))
		return (NULL);
	struct network_config *config = NULL;
	char state_file[PATH_MAX];

	snprintf(state_file, sizeof(state_file), "%s/%s.json",
	    OCIFBSD_NETWORK_STATE_DIR, network_id);

	FILE *f = fopen(state_file, "r");
	if (f) {
		config = calloc(1, sizeof(*config));
		if (config) {
			char buf[512];
			while (fgets(buf, sizeof(buf), f)) {
				char val[256];

				if (json_str_field(buf, "name", val,
				    sizeof(val)))
					config->name = strdup(val);
				else if (json_str_field(buf, "id", val,
				    sizeof(val)))
					config->id = strdup(val);
				else if (json_str_field(buf, "driver", val,
				    sizeof(val)))
					config->driver = strdup(val);
				else if (json_str_field(buf, "bridge", val,
				    sizeof(val)))
					config->bridge = strdup(val);
				else if (json_str_field(buf, "subnet", val,
				    sizeof(val)) && val[0] != '\0')
					config->subnet = strdup(val);
				else if (json_str_field(buf, "gateway", val,
				    sizeof(val)) && val[0] != '\0')
					config->gateway = strdup(val);
				else if (strstr(buf, "\"type\"") != NULL) {
					int t;
					if (sscanf(buf, " \"type\": %d", &t)
					    == 1)
						config->type =
						    (network_type_t)t;
				} else if (strstr(buf, "\"internal\"") != NULL) {
					config->internal =
					    (strstr(buf, "true") != NULL);
				}
			}
			/* A network with no id parsed is not usable. */
			if (config->id == NULL) {
				config->id = strdup(network_id);
			}
		}
		fclose(f);
	}

	return (config);
}

int
network_list(struct network_config ***networks, int *nnetworks)
{
	DIR *dir;
	struct dirent *ent;
	struct network_config **list = NULL;
	int count = 0;

	*networks = NULL;
	*nnetworks = 0;

	dir = opendir(OCIFBSD_NETWORK_STATE_DIR);
	if (dir == NULL)
		return (-1);

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_type == DT_REG && strstr(ent->d_name, ".json")) {
			char network_id[256];
			strlcpy(network_id, ent->d_name, sizeof(network_id));
			*strstr(network_id, ".json") = '\0';

			struct network_config *config = network_get(network_id);
			if (config) {
				if (ocifbsd_realloc_grow((void **)&list, (count + 1) * sizeof(*list)) != 0)
					continue;
				list[count++] = config;
			}
		}
	}

	closedir(dir);

	*networks = list;
	*nnetworks = count;

	return (0);
}

/*
 * Wire a VNET jail's connectivity: create an epair, move its 'b' side into the
 * jail (identified by jid), configure the container's IPv4 address and optional
 * default gateway inside the jail, and, when a bridge is given, attach the host
 * 'a' side to it. The host-side interface name is returned in *out_side_a
 * (caller frees) so the caller can persist it and destroy the epair on delete
 * — the kernel does NOT reclaim the host side when the jail is removed.
 *
 * Returns 0 on success. On any failure the epair is torn down before returning
 * so a failed start leaves no dangling interface. ip4cidr may be NULL (no
 * address configured); gw4 and bridge may be NULL.
 */
int
vnet_wire_jail(int jid, const char *ip4cidr, const char *gw4,
	const char *bridge, char **out_side_a)
{
	char *side_a = NULL, *side_b = NULL;
	char jidstr[16];

	if (out_side_a != NULL)
		*out_side_a = NULL;

	if (epair_create("ocifbsd", &side_a, &side_b) != 0)
		return (-1);

	snprintf(jidstr, sizeof(jidstr), "%d", jid);

	/*
	 * Bring the host side up and move the peer into the jail's vnet. Moving
	 * the interface is the essential step and is done entirely from the
	 * host; if it fails, tear the epair down and report failure.
	 */
	(void)run_cmd(3, "ifconfig", side_a, "up");
	if (run_cmd(4, "ifconfig", side_b, "vnet", jidstr) != 0)
		goto fail;

	/*
	 * The remaining steps run tools INSIDE the jail (jexec), which requires
	 * ifconfig/route in the container's rootfs. A minimal OCI image may not
	 * ship them, so these are best-effort: on failure the interface still
	 * exists in the jail for the container (or the operator) to configure,
	 * and we keep it rather than failing the whole start.
	 */
	(void)run_cmd(5, "jexec", jidstr, "ifconfig", "lo0", "up");
	if (ip4cidr != NULL && ip4cidr[0] != '\0') {
		if (run_cmd(7, "jexec", jidstr, "ifconfig", side_b, "inet",
		    ip4cidr, "up") != 0)
			fprintf(stderr, "warning: could not set %s in jail %s "
			    "(no ifconfig in the container?); interface is "
			    "present but unconfigured\n", ip4cidr, jidstr);
	}
	if (gw4 != NULL && gw4[0] != '\0')
		(void)run_cmd(6, "jexec", jidstr, "route", "add", "default",
		    gw4);

	/* Optional bridge attach on the host side for external reachability. */
	if (bridge != NULL && bridge[0] != '\0') {
		(void)bridge_create(bridge);
		(void)bridge_add_interface(bridge, side_a);
	}

	free(side_b);
	if (out_side_a != NULL)
		*out_side_a = side_a;
	else
		free(side_a);
	return (0);

fail:
	/* Destroying either side removes the pair (and any bridge membership). */
	(void)epair_delete(side_a);
	free(side_a);
	free(side_b);
	return (-1);
}

/*
 * VNET management
 */
int
vnet_create_jail(const char *jail_name, struct vnet_config *config)
{
	int ret;

	/* Check if vnet is supported */
	ret = sysctlbyname("security.jail.vnet_enabled", NULL, NULL, NULL, 0);
	if (ret != 0) {
		fprintf(stderr, "error: vnet not available\n");
		return (-1);
	}

	/* Create epair for jail */
	if (config->epair_prefix) {
		char *side_a, *side_b;
		if (epair_create(config->epair_prefix, &side_a, &side_b) == 0) {
			/* Add host side to bridge if specified */
			if (config->bridge_to && config->bridge_name) {
				bridge_add_interface(config->bridge_name, side_a);
			}
			free(side_a);
			free(side_b);
		}
	}

	return (0);
}

int
vnet_delete_jail(const char *jail_name)
{
	/*
	 * Epair cleanup is not yet implemented. To clean up epairs
	 * associated with a jail, the system needs to:
	 *
	 *   1. Track which epairs were created for which jail
	 *      (currently the epair_create function is called but
	 *      the mapping is not persisted)
	 *   2. On jail delete, look up the epairs for this jail
	 *   3. Destroy each epair with 'ifconfig <epair> destroy'
	 *   4. Remove the bridge member mappings
	 *
	 * This is a RESOURCE LEAK: without cleanup, deleted jails
	 * leave their epair interfaces in the kernel, eventually
	 * exhausting interface slots. See MIGRATION.md for the plan.
	 */
	(void)jail_name;
	return (0);
}

/*
 * NAT setup using pf
 */
static const char *pf_rules = ""
	"# ocifbsd NAT rules\n"
	"nat on %s from %s to any -> (%s)\n";

int
nat_enable(const char *jail_name, const char *external_iface)
{
	FILE *f;
	char rule[1024];

	/* Append NAT rule to pf.conf */
	f = fopen("/etc/pf.conf.ocifbsd", "a");
	if (f == NULL) {
		fprintf(stderr, "error: cannot open pf rules file\n");
		return (-1);
	}

	snprintf(rule, sizeof(rule), pf_rules,
	    external_iface, "10.0.0.0/24", external_iface);
	fprintf(f, "%s", rule);

	fclose(f);

	/* Reload pf rules */
	return (run_cmd(3, "pfctl", "-f", "/etc/pf.conf"));
}

int
nat_disable(const char *jail_name)
{
	char pf_ocifbsd[PATH_MAX];
	char buf[8192];
	FILE *in, *out;
	size_t len = 0;
	int in_ocifbsd_section = 0;
	int removed = 0;

	(void)jail_name;

	snprintf(pf_ocifbsd, sizeof(pf_ocifbsd), "/etc/pf.conf.ocifbsd");

	in = fopen(pf_ocifbsd, "r");
	if (in == NULL) {
		return (run_cmd(3, "pfctl", "-f", "/etc/pf.conf"));
	}

	out = fopen("/etc/pf.conf.ocifbsd.tmp", "w");
	if (out == NULL) {
		fclose(in);
		return (-1);
	}

	while (len < sizeof(buf) - 1 &&
	    fgets(buf + len, (int)(sizeof(buf) - len), in) != NULL) {
		len = strlen(buf);
	}
	fclose(in);

	if (len == 0) {
		fclose(out);
		unlink("/etc/pf.conf.ocifbsd.tmp");
		return (run_cmd(3, "pfctl", "-f", "/etc/pf.conf"));
	}

	const char *p = buf;
	while (*p != '\0') {
		const char *eol = strchr(p, '\n');
		size_t llen = eol ? (size_t)(eol - p) + 1 : strlen(p);

		if (in_ocifbsd_section) {
			removed = 1;
			if (eol == NULL || llen == 0)
				break;
		} else {
			if (llen >= strlen("# ocifbsd NAT rules") &&
			    strncmp(p, "# ocifbsd NAT rules", llen > 22 ? 22 : llen) == 0) {
				in_ocifbsd_section = 1;
				removed = 1;
			} else {
				fwrite(p, 1, llen, out);
			}
		}
		p += llen;
	}
	fclose(out);

	if (removed) {
		if (rename("/etc/pf.conf.ocifbsd.tmp", pf_ocifbsd) != 0) {
			unlink("/etc/pf.conf.ocifbsd.tmp");
			return (-1);
		}
	} else {
		unlink("/etc/pf.conf.ocifbsd.tmp");
	}

	return (run_cmd(3, "pfctl", "-f", "/etc/pf.conf.ocifbsd"));
}

int
nat_check(void)
{
	int ret;
	ret = run_cmd(3, "pfctl", "-s", "nat");
	return (ret);
}

/*
 * DNS configuration
 */
static int
read_state_file(const char *path, char *buf, size_t bufsz, size_t *len_out)
{
	FILE *f = fopen(path, "r");
	size_t len = 0;

	if (f == NULL)
		return (-1);

	while (len < bufsz - 1 &&
	    fgets(buf + len, (int)(bufsz - len), f) != NULL) {
		len = strlen(buf);
	}
	fclose(f);

	if (len == 0)
		return (-1);

	*len_out = len;
	return (0);
}

static char *
find_quoted_in_range(const char *start, const char *limit, const char *needle)
{
	char pattern[256];
	char *p;

	if (snprintf(pattern, sizeof(pattern), "\"%s\"", needle) >=
	    (int)sizeof(pattern))
		return (NULL);

	p = strstr(start, pattern);
	if (p == NULL || p >= limit)
		return (NULL);

	return (p);
}

int
dns_add_server(const char *network_id, const char *server)
{
	if (!net_id_is_valid(network_id))
		return (-1);
	char state_file[PATH_MAX];
	char buf[4096];
	char new_buf[5120];
	size_t len = 0;
	char *dns_key, *arr, *end, *existing;
	char server_entry[256];
	int is_empty;

	if (network_id == NULL || server == NULL) {
		errno = EINVAL;
		return (-1);
	}

	snprintf(state_file, sizeof(state_file), "%s/%s.json",
	    OCIFBSD_NETWORK_STATE_DIR, network_id);

	if (read_state_file(state_file, buf, sizeof(buf), &len) != 0)
		return (-1);

	dns_key = strstr(buf, "\"dns_servers\":");
	if (dns_key != NULL) {
		arr = strchr(dns_key, '[');
		end = strchr(dns_key, ']');
		if (arr == NULL || end == NULL)
			return (-1);

		existing = find_quoted_in_range(arr, end, server);
		if (existing != NULL)
			return (0);

		is_empty = (end == arr + 1);
		snprintf(server_entry, sizeof(server_entry),
		    is_empty ? "\"%s\"" : ", \"%s\"", server);

		if ((size_t)(end - buf) + strlen(server_entry) +
		    strlen(end) + 1 > sizeof(new_buf))
			return (-1);

		snprintf(new_buf, sizeof(new_buf), "%.*s%s%s",
		    (int)(end - buf), buf, server_entry, end);
	} else {
		char *brace = strrchr(buf, '}');
		char *trim;
		int has_trailing_comma;

		if (brace == NULL)
			return (-1);

		trim = brace;
		while (trim > buf && (trim[-1] == ' ' || trim[-1] == '\n' ||
		    trim[-1] == '\r' || trim[-1] == '\t'))
			trim--;

		has_trailing_comma = (trim > buf && trim[-1] == ',');
		snprintf(new_buf, sizeof(new_buf),
		    "%.*s%s\"dns_servers\": [\"%s\"]\n}\n",
		    (int)(trim - buf), buf,
		    has_trailing_comma ? "" : ", ",
		    server);
	}

	FILE *f = fopen(state_file, "w");
	if (f == NULL)
		return (-1);
	fputs(new_buf, f);
	fclose(f);

	return (0);
}

int
dns_remove_server(const char *network_id, const char *server)
{
	if (!net_id_is_valid(network_id))
		return (-1);
	char state_file[PATH_MAX];
	char buf[4096];
	char new_buf[5120];
	size_t len = 0;
	char *dns_key, *arr, *end, *match;
	size_t prefix_len, suffix_len, remove_len;

	if (network_id == NULL || server == NULL) {
		errno = EINVAL;
		return (-1);
	}

	snprintf(state_file, sizeof(state_file), "%s/%s.json",
	    OCIFBSD_NETWORK_STATE_DIR, network_id);

	if (read_state_file(state_file, buf, sizeof(buf), &len) != 0)
		return (0);

	dns_key = strstr(buf, "\"dns_servers\":");
	if (dns_key == NULL)
		return (0);

	arr = strchr(dns_key, '[');
	end = strchr(dns_key, ']');
	if (arr == NULL || end == NULL)
		return (-1);

	match = find_quoted_in_range(arr, end, server);
	if (match == NULL)
		return (0);

	prefix_len = (size_t)(match - buf);
	remove_len = strlen(server) + 2;
	if (match[remove_len] == ',') {
		remove_len++;
		if (match[remove_len] == ' ')
			remove_len++;
	} else if (match > arr + 1 && match[-1] == ',') {
		prefix_len--;
		remove_len++;
		if (match[remove_len] == ' ') {
			prefix_len--;
			remove_len++;
		}
	} else if (match > arr + 1 && match[-1] == ' ') {
		prefix_len--;
		remove_len++;
	}

	const char *suffix = match + remove_len;
	suffix_len = strlen(suffix);

	if (prefix_len + suffix_len + 1 > sizeof(new_buf))
		return (-1);

	memcpy(new_buf, buf, prefix_len);
	memcpy(new_buf + prefix_len, suffix, suffix_len + 1);

	FILE *f = fopen(state_file, "w");
	if (f == NULL)
		return (-1);
	fputs(new_buf, f);
	fclose(f);

	return (0);
}

int
dns_set_resolver(const char *jail_name, char **servers, int nservers)
{
	char resolv_conf[PATH_MAX];
	FILE *f;
	int i;

	snprintf(resolv_conf, sizeof(resolv_conf),
	    "/var/run/ocifbsd/jails/%s/etc/resolv.conf", jail_name);

	/* Ensure directory exists */
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "/var/run/ocifbsd/jails/%s/etc", jail_name);
	mkdirp(dir, 0755);

	f = fopen(resolv_conf, "w");
	if (f == NULL)
		return (-1);

	for (i = 0; i < nservers; i++) {
		fprintf(f, "nameserver %s\n", servers[i]);
	}

	fclose(f);

	return (0);
}

/*
 * Network monitoring
 */
static int
parse_iface_stats(const char *iface, uint64_t *rx_bytes, uint64_t *tx_bytes)
{
	char *output = NULL;
	char *line, *save;
	int found_rx = 0, found_tx = 0;

	*rx_bytes = 0;
	*tx_bytes = 0;

	if (iface == NULL || run_cmd_output(&output, 2, "ifconfig", iface) != 0) {
		free(output);
		return (-1);
	}

	line = strtok_r(output, "\n", &save);
	while (line != NULL) {
		const char *bytes_str;

		if (!found_rx && (bytes_str = strstr(line, "input packets")) != NULL) {
			bytes_str = strstr(bytes_str, "bytes ");
			if (bytes_str != NULL) {
				*rx_bytes = strtoull(bytes_str + 6, NULL, 10);
				found_rx = 1;
			}
		} else if (!found_tx && (bytes_str = strstr(line, "output packets")) != NULL) {
			bytes_str = strstr(bytes_str, "bytes ");
			if (bytes_str != NULL) {
				*tx_bytes = strtoull(bytes_str + 6, NULL, 10);
				found_tx = 1;
			}
		}
		line = strtok_r(NULL, "\n", &save);
	}

	free(output);
	return (0);
}

int
network_stats(const char *network_id, uint64_t *rx_bytes, uint64_t *tx_bytes)
{
	if (!net_id_is_valid(network_id))
		return (-1);
	char state_file[PATH_MAX];
	char bridge[64] = {0};
	char *fgets_ret;
	FILE *f;

	*rx_bytes = 0;
	*tx_bytes = 0;

	if (network_id == NULL)
		return (-1);

	snprintf(state_file, sizeof(state_file), "%s/%s.json",
	    OCIFBSD_NETWORK_STATE_DIR, network_id);

	f = fopen(state_file, "r");
	if (f == NULL)
		return (-1);

	char buf[4096];
	while ((fgets_ret = fgets(buf, sizeof(buf), f)) != NULL) {
		char *p = strstr(buf, "\"bridge\":");
		if (p != NULL) {
			p = strchr(p, '"');
			if (p != NULL) {
				p++;
				char *end = strchr(p, '"');
				if (end != NULL) {
					size_t len = (size_t)(end - p);
					if (len >= sizeof(bridge))
						len = sizeof(bridge) - 1;
					memcpy(bridge, p, len);
					bridge[len] = '\0';
				}
			}
			break;
		}
	}
	fclose(f);

	if (bridge[0] == '\0')
		return (-1);

	return (parse_iface_stats(bridge, rx_bytes, tx_bytes));
}

int
endpoint_stats(const char *endpoint_id, uint64_t *rx_bytes, uint64_t *tx_bytes)
{
	*rx_bytes = 0;
	*tx_bytes = 0;

	if (endpoint_id == NULL)
		return (-1);

	return (parse_iface_stats(endpoint_id, rx_bytes, tx_bytes));
}

/*
 * Network inspection (JSON output)
 */
int
network_inspect(const char *network_id, char **json_output)
{
	if (!net_id_is_valid(network_id))
		return (-1);
	struct network_config *config;
	char *json;
	size_t len;

	config = network_get(network_id);
	if (config == NULL)
		return (-1);

	/* Generate JSON */
	len = 4096;
	json = malloc(len);
	if (json == NULL) {
		return (-1);
	}

	snprintf(json, len,
	    "{\n"
	    "  \"name\": \"%s\",\n"
	    "  \"id\": \"%s\",\n"
	    "  \"type\": \"%s\",\n"
	    "  \"driver\": \"%s\",\n"
	    "  \"bridge\": \"%s\",\n"
	    "  \"subnet\": \"%s\",\n"
	    "  \"gateway\": \"%s\",\n"
	    "  \"internal\": %s\n"
	    "}\n",
	    config->name ? config->name : "",
	    config->id ? config->id : "",
	    config->type == NETWORK_TYPE_BRIDGE ? "bridge" : "unknown",
	    config->driver ? config->driver : "",
	    config->bridge ? config->bridge : "",
	    config->subnet ? config->subnet : "",
	    config->gateway ? config->gateway : "",
	    config->internal ? "true" : "false");

	*json_output = json;

	return (0);
}
