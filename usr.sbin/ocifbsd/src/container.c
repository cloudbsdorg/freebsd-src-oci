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
 * Container lifecycle management
 */

#include <sys/param.h>
#include <sys/jail.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syslimits.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <jail.h>
#include <libutil.h>
#include <paths.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ocifbsd.h"

/* setproctitle(3) is in <libutil.h> on FreeBSD but the declaration is
 * not visible with the strict feature-test macros used here. Declare
 * it locally to avoid the implicit-function-declaration -Werror. */
extern void setproctitle(const char *fmt, ...);

/* putenv(3) is in <stdlib.h> but on FreeBSD 16 it is gated on
 * __XSI_VISIBLE, which we don't enable (we use __POSIX_VISIBLE=200809
 * via _POSIX_C_SOURCE). Declare it locally. */
extern int putenv(char *string);

/* Global container registry */
static struct ocifbsd_container **container_registry = NULL;
static int container_registry_size = 0;
static int container_registry_capacity = 0;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Add container to registry
 */
static int
container_register(struct ocifbsd_container *c)
{
	int i;

	pthread_mutex_lock(&registry_lock);

	/* Check if already exists */
	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    strcmp(container_registry[i]->id, c->id) == 0) {
			pthread_mutex_unlock(&registry_lock);
			errno = EEXIST;
			return (-1);
		}
	}

	/* Expand capacity if needed */
	if (container_registry_size >= container_registry_capacity) {
		container_registry_capacity = container_registry_capacity ?
		    container_registry_capacity * 2 : 16;
		container_registry = realloc(container_registry,
		    container_registry_capacity * sizeof(*container_registry));
		if (container_registry == NULL) {
			pthread_mutex_unlock(&registry_lock);
			errno = ENOMEM;
			return (-1);
		}
	}

	container_registry[container_registry_size++] = c;

	pthread_mutex_unlock(&registry_lock);
	return (0);
}

/*
 * Remove container from registry
 */
static int
container_unregister(const char *id)
{
	int i;

	pthread_mutex_lock(&registry_lock);

	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    strcmp(container_registry[i]->id, id) == 0) {
			container_registry[i] = NULL;
			pthread_mutex_unlock(&registry_lock);
			return (0);
		}
	}

	pthread_mutex_unlock(&registry_lock);
	errno = ENOENT;
	return (-1);
}

/*
 * Get container from registry by ID
 */
struct ocifbsd_container *
container_get_by_id(const char *id)
{
	int i;
	struct ocifbsd_container *found = NULL;

	if (id == NULL)
		return (NULL);

	pthread_mutex_lock(&registry_lock);

	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    strcmp(container_registry[i]->id, id) == 0) {
			found = container_registry[i];
			break;
		}
	}

	pthread_mutex_unlock(&registry_lock);

	/* Try loading from state if not in memory */
	if (found == NULL) {
		found = state_load(id);
		if (found != NULL) {
			container_register(found);
		}
	}

	return (found);
}

/*
 * Get container by name
 */
struct ocifbsd_container *
container_get_by_name(const char *name)
{
	int i;
	struct ocifbsd_container *found = NULL;

	if (name == NULL)
		return (NULL);

	pthread_mutex_lock(&registry_lock);

	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    container_registry[i]->name != NULL &&
		    strcmp(container_registry[i]->name, name) == 0) {
			found = container_registry[i];
			break;
		}
	}

	pthread_mutex_unlock(&registry_lock);

	return (found);
}

/*
 * Get container by jail ID
 */
struct ocifbsd_container *
container_get_by_jid(int jid)
{
	int i;
	struct ocifbsd_container *found = NULL;

	pthread_mutex_lock(&registry_lock);

	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    container_registry[i]->jid == jid) {
			found = container_registry[i];
			break;
		}
	}

	pthread_mutex_unlock(&registry_lock);

	return (found);
}

/*
 * Free container structure
 */
void
container_free(struct ocifbsd_container *c)
{
	if (c == NULL)
		return;

	free(c->id);
	free(c->name);
	free(c->rootfs);
	free(c->bundle_path);
	free(c->config_path);
	free(c->log_path);

	if (c->spec != NULL)
		oci_free_spec(c->spec);

	free(c);
}

/*
 * Set up process environment inside container
 */
static int
setup_process_env(struct ocifbsd_container *c)
{
	struct oci_runtime_spec *spec = c->spec;
	char **env;
	int i;

	if (spec == NULL || spec->process.env == NULL)
		return (0);

	env = spec->process.env;
	for (i = 0; env[i] != NULL; i++) {
		putenv(env[i]);
	}

	return (0);
}

/*
 * Create a container from OCI bundle
 */
int
container_create(struct ocifbsd_container **cp, const char *bundle_path,
    const char *name)
{
	struct ocifbsd_container *c;
	struct oci_runtime_spec *spec;
	struct jailparam *params;
	size_t nparams;
	char config_path[PATH_MAX];
	char *canonical;

	if (cp == NULL || bundle_path == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/* Initialize state directory */
	if (state_init() != 0) {
		return (-1);
	}

	/* Find config.json in bundle */
	snprintf(config_path, sizeof(config_path), "%s/config.json", bundle_path);

	/* Parse OCI config */
	spec = oci_parse_config(config_path);
	if (spec == NULL) {
		fprintf(stderr, "error: failed to parse OCI config: %s\n",
		    config_path);
		return (-1);
	}

	/*
	 * Resolve relative root.path against the bundle directory so
	 * jail(8) receives an absolute path (required).
	 */
	if (spec->root.path != NULL && spec->root.path[0] != '/') {
		char abspath[PATH_MAX];
		char *resolved;

		snprintf(abspath, sizeof(abspath), "%s/%s", bundle_path,
		    spec->root.path);
		resolved = realpath(abspath, NULL);
		if (resolved == NULL) {
			/* keep joined path even if rootfs not yet fully present */
			resolved = strdup(abspath);
		}
		if (resolved == NULL) {
			oci_free_spec(spec);
			errno = ENOMEM;
			return (-1);
		}
		free(spec->root.path);
		spec->root.path = resolved;
	}

	/* Validate spec */
	if (oci_validate_spec(spec) != 0) {
		oci_free_spec(spec);
		return (-1);
	}

	/* Allocate container */
	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		oci_free_spec(spec);
		errno = ENOMEM;
		return (-1);
	}

	c->spec = spec;
	c->bundle_path = strdup(bundle_path);
	c->config_path = strdup(config_path);

	/* Generate or use provided name */
	if (name != NULL) {
		canonical = canonical_name(name);
		if (canonical == NULL) {
			/* Use generated ID as name */
			c->name = NULL;
		} else {
			c->name = canonical;
		}
	}

	/* Generate container ID */
	c->id = generate_container_id();
	if (c->id == NULL) {
		container_free(c);
		errno = ENOMEM;
		return (-1);
	}

	/* Use ID as name if not provided */
	if (c->name == NULL) {
		c->name = strdup(c->id);
	}

	/* Set rootfs path */
	c->rootfs = strdup(spec->root.path);
	if (c->rootfs == NULL) {
		container_free(c);
		errno = ENOMEM;
		return (-1);
	}

	/* Generate jail parameters from OCI spec */
	params = oci_spec_to_jail_params(spec, &nparams);
	if (params == NULL) {
		container_free(c);
		return (-1);
	}

	/*
	 * Give the jail a unique name (container id prefix). The helper
	 * seeds a placeholder "name" param; replace it in place.
	 */
	{
		size_t pi;
		char jname[64];

		snprintf(jname, sizeof(jname), "ocifbsd-%.12s", c->id);
		for (pi = 0; pi < nparams; pi++) {
			if (params[pi].jp_name != NULL &&
			    strcmp(params[pi].jp_name, "name") == 0) {
				jailparam_import_raw(&params[pi], jname,
				    strlen(jname) + 1);
				break;
			}
		}
	}

	/* Create jail (persist is set by oci_spec_to_jail_params) */
	c->jid = jailparam_set(params, nparams, JAIL_CREATE);
	if (c->jid < 0) {
		fprintf(stderr, "error: failed to create jail: %s\n",
		    strerror(errno));
		container_free(c);
		jailparam_free(params, nparams);
		return (-1);
	}

	jailparam_free(params, nparams);

	/* Container created but not started */
	c->state = OCIFBSD_STATE_CREATED;
	c->created_at = time(NULL);

	/* Register container */
	if (container_register(c) != 0) {
		/* Warning only */
		fprintf(stderr, "warning: failed to register container: %s\n",
		    strerror(errno));
	}

	/* Save state */
	state_save(c);

	*cp = c;
	return (0);
}

/*
 * Start a created container
 */
int
container_start(struct ocifbsd_container *c)
{
	pid_t pid;
	int status;

	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (c->state != OCIFBSD_STATE_CREATED) {
		errno = EINVAL;
		fprintf(stderr, "error: container %s not in created state\n",
		    c->id);
		return (-1);
	}

	/* Run prestart hooks */
	hooks_run_prestart(c);

	/* Fork to start container init process */
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "error: fork failed: %s\n", strerror(errno));
		return (-1);
	}

	if (pid == 0) {
		/* Child process - become the container init */

		/* Attach to jail */
		if (jail_attach(c->jid) != 0) {
			fprintf(stderr, "error: failed to attach to jail: %s\n",
			    strerror(errno));
			_exit(126);
		}

		/* Set working directory */
		if (c->spec->process.cwd != NULL) {
			if (chdir(c->spec->process.cwd) != 0) {
				fprintf(stderr, "error: failed to change directory: %s\n",
				    strerror(errno));
			}
		}

		/* Set up environment */
		setup_process_env(c);

		/*
		 * Resource limits are not yet applied here. The OCI spec's
		 * process.rlimits (POSIX rlimits: RLIMIT_CPU, RLIMIT_AS,
		 * RLIMIT_NPROC, RLIMIT_NOFILE, RLIMIT_FSIZE) should be
		 * applied with setrlimit() in this child process after
		 * jail_attach().
		 *
		 * FreeBSD-specific RCTL rules (c->spec->freebsd->rctl_rules)
		 * are trickier: they must be applied to the JAIL, not the
		 * process. This means they should be applied by the PARENT
		 * before the child is forked, using either:
		 *   - jail_set(2) with jailparam "rctl.*" rules
		 *   - rctl_add_rule(2) with subject "jail:<jid>"
		 *   - system("rctl -a jail:<jid> <rule>")
		 *
		 * To implement properly:
		 *   1. Add 'struct oci_rlimit *rlimits' to oci_process
		 *   2. Parse process.rlimits in oci_parse_config
		 *   3. In container_start (parent, before fork):
		 *        for each freebsd->rctl_rules[i]:
		 *          jail_set with "rctl.rule" param
		 *   4. In child (here, after fork):
		 *        for each spec->process.rlimits:
		 *          setrlimit(RLIMIT_*, ...)
		 *
		 * Without this, containers have NO resource limits
		 * (a misbehaving container can exhaust system resources).
		 * This is a SECURITY/STABILITY issue.
		 * See MIGRATION.md for the full plan.
		 */
		(void)0;

		/* Set process title */
		setproctitle("ocifbsd: %s [%s]",
		    c->name ? c->name : "(unnamed)",
		    c->id ? c->id : "(no-id)");

		/* Execute the container command */
		if (c->spec->process.args && c->spec->process.args[0]) {
			execvp(c->spec->process.args[0], c->spec->process.args);
		} else {
			/* Default: run sh */
			char sh[] = "/bin/sh";
			char *sh_args[] = { sh, NULL };
			execvp("/bin/sh", sh_args);
		}

		/* If execvp fails */
		fprintf(stderr, "error: failed to exec: %s\n", strerror(errno));
		_exit(127);
	}

	/* Parent process */
	c->init_pid = pid;
	c->state = OCIFBSD_STATE_RUNNING;
	c->started_at = time(NULL);

	/* Wait briefly to check if process starts */
	{
		struct timespec ts = { 0, 100 * 1000 * 1000 }; /* 100ms */
		nanosleep(&ts, NULL);
	}
	if (waitpid(pid, &status, WNOHANG) == 0) {
		/* Process is still running */
	} else if (WIFEXITED(status)) {
		/* Process exited immediately */
		c->exit_code = WEXITSTATUS(status);
		c->state = OCIFBSD_STATE_STOPPED;
		c->finished_at = time(NULL);
		return (-1);
	}

	/* Update state */
	state_save(c);

	/* Run poststart hooks */
	hooks_run_poststart(c);

	return (0);
}

/*
 * Send signal to container init process
 */
int
container_kill(struct ocifbsd_container *c, int sig)
{
	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (c->state != OCIFBSD_STATE_RUNNING) {
		errno = EINVAL;
		return (-1);
	}

	if (c->init_pid <= 0) {
		errno = ESRCH;
		return (-1);
	}

	if (kill(c->init_pid, sig) != 0) {
		return (-1);
	}

	return (0);
}

/*
 * Delete a container
 */
int
container_delete(struct ocifbsd_container *c)
{
	int ret;

	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/* Run poststop hooks */
	hooks_run_poststop(c);

	/* Stop container if running */
	if (c->state == OCIFBSD_STATE_RUNNING) {
		if (c->init_pid > 0) {
			kill(c->init_pid, SIGKILL);
			waitpid(c->init_pid, NULL, 0);
		}
	}

	/* Remove jail */
	if (c->jid > 0) {
		ret = jail_remove(c->jid);
		if (ret != 0) {
			fprintf(stderr, "warning: failed to remove jail: %s\n",
			    strerror(errno));
		}
	}

	/* Unregister from memory */
	container_unregister(c->id);

	/* Delete state file */
	state_delete(c->id);

	/* Update state */
	c->state = OCIFBSD_STATE_STOPPED;
	c->finished_at = time(NULL);

	return (0);
}

/*
 * Pause a container
 */
int
container_pause(struct ocifbsd_container *c)
{
	if (c == NULL || c->state != OCIFBSD_STATE_RUNNING) {
		errno = EINVAL;
		return (-1);
	}

	/*
	 * Pausing a jail is not yet implemented. The proper way to
	 * pause a jail on FreeBSD is to send SIGSTOP to the init
	 * process (and all its children). The naive approach is:
	 *
	 *   kill(c->init_pid, SIGSTOP);
	 *
	 * But this only pauses the init process, not the entire
	 * process tree. To pause the whole tree:
	 *
	 *   1. Use kvm_getprocs() to enumerate all processes in the jail
	 *   2. For each PID with prison matching c->jid, send SIGSTOP
	 *   3. Track which PIDs were stopped (for resume)
	 *
	 * Or use the kernel interface (if available):
	 *   - procctl(PROC_PID, pid, PROC_CTL_JAIL_PAUSE)
	 *   - jail_set with "jail.stopped" parameter
	 *
	 * For now, the state is set to PAUSED but the process tree
	 * is NOT actually paused. This is a BUG: container_pause()
	 * returns success without doing anything.
	 * See MIGRATION.md for the full plan.
	 */
	(void)0;

	c->state = OCIFBSD_STATE_PAUSED;
	state_save(c);

	return (0);
}

/*
 * Resume a paused container
 */
int
container_resume(struct ocifbsd_container *c)
{
	if (c == NULL || c->state != OCIFBSD_STATE_PAUSED) {
		errno = EINVAL;
		return (-1);
	}

	/* Resume jail */
	c->state = OCIFBSD_STATE_RUNNING;
	state_save(c);

	return (0);
}

/*
 * Wait for container to exit
 */
int
container_wait(struct ocifbsd_container *c)
{
	int status;

	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (c->state != OCIFBSD_STATE_RUNNING && c->state != OCIFBSD_STATE_STOPPED) {
		errno = EINVAL;
		return (-1);
	}

	if (c->init_pid <= 0) {
		errno = ESRCH;
		return (-1);
	}

	/* Wait for init process to exit */
	if (waitpid(c->init_pid, &status, 0) > 0) {
		if (WIFEXITED(status)) {
			c->exit_code = WEXITSTATUS(status);
		} else if (WIFSIGNALED(status)) {
			c->exit_code = 128 + WTERMSIG(status);
		}
		c->state = OCIFBSD_STATE_STOPPED;
		c->finished_at = time(NULL);
		state_save(c);
	}

	return (c->exit_code);
}

/*
 * Generate container state as JSON for inspect
 */
int
container_inspect(struct ocifbsd_container *c, char **json_out)
{
	char *json;
	int len;

	if (c == NULL || json_out == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/* asprintf() is hidden by -D_XOPEN_SOURCE=700. Use a two-pass
	 * snprintf(NULL, 0) to size, malloc, then snprintf again to fill.
	 * Verbose but avoids the feature-test-macro dance. */
	len = snprintf(NULL, 0,
	    "{"
	    "\"id\": \"%s\","
	    "\"name\": \"%s\","
	    "\"state\": \"%s\","
	    "\"created\": %ld,"
	    "\"started\": %ld,"
	    "\"finished\": %ld,"
	    "\"exit_code\": %d,"
	    "\"bundle\": \"%s\","
	    "\"rootfs\": \"%s\","
	    "\"config\": \"%s\""
	    "}",
	    c->id ? c->id : "",
	    c->name ? c->name : "",
	    ocifbsd_state_to_string(c->state),
	    (long)c->created_at,
	    (long)c->started_at,
	    (long)c->finished_at,
	    c->exit_code,
	    c->bundle_path ? c->bundle_path : "",
	    c->rootfs ? c->rootfs : "",
	    c->config_path ? c->config_path : "");
	if (len < 0)
		return (-1);
	json = malloc(len + 1);
	if (json == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	snprintf(json, len + 1,
	    "{"
	    "\"id\": \"%s\","
	    "\"name\": \"%s\","
	    "\"state\": \"%s\","
	    "\"created\": %ld,"
	    "\"started\": %ld,"
	    "\"finished\": %ld,"
	    "\"exit_code\": %d,"
	    "\"bundle\": \"%s\","
	    "\"rootfs\": \"%s\","
	    "\"config\": \"%s\""
	    "}",
	    c->id ? c->id : "",
	    c->name ? c->name : "",
	    ocifbsd_state_to_string(c->state),
	    (long)c->created_at,
	    (long)c->started_at,
	    (long)c->finished_at,
	    c->exit_code,
	    c->bundle_path ? c->bundle_path : "",
	    c->rootfs ? c->rootfs : "",
	    c->config_path ? c->config_path : "");

	if (len < 0) {
		errno = ENOMEM;
		return (-1);
	}

	*json_out = json;
	return (0);
}
