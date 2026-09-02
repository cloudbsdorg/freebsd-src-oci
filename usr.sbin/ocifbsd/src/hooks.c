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
 * OCI Hooks execution
 */

#include <sys/param.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <json.h>

#include "ocifbsd.h"

/*
 * Execute a single hook
 */
static int
execute_hook(const struct oci_hook *hook, const char *state_file)
{
	pid_t pid;
	int status;
	char **envp = NULL;
	char **argp = NULL;
	int env_count, arg_count;
	int ret = 0;
	int i;

	if (hook == NULL || hook->path == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/* Count args and env */
	for (arg_count = 0; hook->args && hook->args[arg_count]; arg_count++)
		;
	for (env_count = 0; hook->env && hook->env[env_count]; env_count++)
		;

	/* Allocate argp: path + args + state_file + NULL */
	argp = calloc(arg_count + 3, sizeof(char *));
	if (argp == NULL)
		return (-1);

	/* Build argp */
	argp[0] = hook->path;
	for (i = 0; i < arg_count; i++)
		argp[i + 1] = hook->args[i];

	/* Add state file as argument */
	argp[arg_count + 1] = strdup(state_file);
	argp[arg_count + 2] = NULL;

	/* Allocate envp: hook env + state env + NULL */
	envp = calloc(env_count + 4, sizeof(char *));
	if (envp == NULL) {
		free(argp[arg_count + 1]);
		free(argp);
		return (-1);
	}

	for (i = 0; i < env_count; i++)
		envp[i] = hook->env[i];

	/* Add standard state environment variables */
	envp[env_count] = strdup("OCI_CONTAINER_ID=ocifbsd");
	envp[env_count + 1] = strdup("OCI_HOOK_SPEC_PATH=.");
	envp[env_count + 2] = strdup("OCI_RUNTIME=ocifbsd");
	envp[env_count + 3] = NULL;

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "error: fork failed for hook: %s\n",
		    strerror(errno));
		ret = -1;
		goto cleanup;
	}

	if (pid == 0) {
		/* Child: exec the hook */

		/* Close unnecessary file descriptors */
		closefrom(STDERR_FILENO + 1);

		execve(hook->path, argp, envp);

		/* If execve fails */
		fprintf(stderr, "error: failed to exec hook: %s\n",
		    strerror(errno));
		_exit(127);
	}

	/*
	 * Parent: wait for the hook, enforcing the OCI-specified timeout if
	 * one is set. A hung or malicious hook must not block container
	 * startup indefinitely. We poll with WNOHANG and SIGKILL the hook
	 * once the timeout elapses.
	 */
	{
		int timeout_sec = 0;
		int waited_ms = 0;
		struct timespec tick = { 0, 50 * 1000 * 1000 }; /* 50ms */
		pid_t w;

		if (hook->timeout != NULL)
			timeout_sec = atoi(hook->timeout);

		if (timeout_sec <= 0) {
			/* No timeout: block until the hook exits. */
			if (waitpid(pid, &status, 0) < 0) {
				fprintf(stderr,
				    "error: waitpid failed for hook: %s\n",
				    strerror(errno));
				ret = -1;
				goto cleanup;
			}
		} else {
			for (;;) {
				w = waitpid(pid, &status, WNOHANG);
				if (w == pid)
					break;
				if (w < 0) {
					fprintf(stderr,
					    "error: waitpid failed for hook: %s\n",
					    strerror(errno));
					ret = -1;
					goto cleanup;
				}
				if (waited_ms >= timeout_sec * 1000) {
					fprintf(stderr,
					    "error: hook %s timed out after %ds; "
					    "killing\n", hook->path, timeout_sec);
					kill(pid, SIGKILL);
					waitpid(pid, &status, 0);
					ret = -1;
					goto cleanup;
				}
				nanosleep(&tick, NULL);
				waited_ms += 50;
			}
		}
	}

	if (WIFEXITED(status)) {
		ret = WEXITSTATUS(status);
		if (ret != 0) {
			fprintf(stderr, "error: hook %s exited with code %d\n",
			    hook->path, ret);
		}
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "error: hook %s killed by signal %d\n",
		    hook->path, WTERMSIG(status));
		ret = -1;
	}

cleanup:
	/*
	 * Only free memory we allocated. argp[0] and hook->args[] are
	 * borrowed; argp[arg_count+1] is the strdup'd state_file.
	 * envp[0..env_count) are borrowed; the next three are ours.
	 */
	if (argp != NULL) {
		free(argp[arg_count + 1]);
		free(argp);
	}
	if (envp != NULL) {
		free(envp[env_count]);
		free(envp[env_count + 1]);
		free(envp[env_count + 2]);
		free(envp);
	}

	return (ret);
}

/*
 * Run a set of hooks
 */
static int
hooks_run(struct oci_hook **hooks, int nhooks, const char *state_file)
{
	int i;
	int ret = 0;

	if (hooks == NULL || nhooks == 0 || state_file == NULL)
		return (0);

	for (i = 0; i < nhooks; i++) {
		int hook_ret = execute_hook(hooks[i], state_file);
		if (hook_ret != 0) {
			fprintf(stderr, "warning: hook %d failed with code %d\n",
			    i, hook_ret);
			if (ret == 0)
				ret = hook_ret;
		}
	}

	return (ret);
}

/*
 * Run prestart hooks
 */
int
hooks_run_prestart(const struct ocifbsd_container *c)
{
	char state_file[PATH_MAX];

	if (c == NULL || c->spec == NULL || c->spec->hooks == NULL)
		return (0);

	snprintf(state_file, sizeof(state_file), "%s/state.json",
	    c->bundle_path ? c->bundle_path : "/tmp");

	return (hooks_run(c->spec->hooks->prestart,
	    c->spec->hooks->n_prestart, state_file));
}

/*
 * Run poststart hooks
 */
int
hooks_run_poststart(const struct ocifbsd_container *c)
{
	char state_file[PATH_MAX];

	if (c == NULL || c->spec == NULL || c->spec->hooks == NULL)
		return (0);

	snprintf(state_file, sizeof(state_file), "%s/state.json",
	    c->bundle_path ? c->bundle_path : "/tmp");

	return (hooks_run(c->spec->hooks->poststart,
	    c->spec->hooks->n_poststart, state_file));
}

/*
 * Run poststop hooks
 */
int
hooks_run_poststop(const struct ocifbsd_container *c)
{
	char state_file[PATH_MAX];

	if (c == NULL || c->spec == NULL || c->spec->hooks == NULL)
		return (0);

	snprintf(state_file, sizeof(state_file), "%s/state.json",
	    c->bundle_path ? c->bundle_path : "/tmp");

	return (hooks_run(c->spec->hooks->poststop,
	    c->spec->hooks->n_poststop, state_file));
}
