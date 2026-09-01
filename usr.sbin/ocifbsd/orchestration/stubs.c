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
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * $FreeBSD$
 *
 * Stub implementations of the ocifbsd_* container lifecycle functions
 * that the orchestration library calls but which live in the main
 * ocifbsd binary. When the orchestration library is linked into the
 * main ocifbsd binary, the real implementations override these stubs.
 * When the library is loaded standalone, these stubs return -1 so
 * callers can detect the lack of a backing container runtime.
 */

#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <syslog.h>

int	ocifbsd_create_container(const char *name, const char *image,
	    const char *command, char **args, uid_t uid,
	    char **container_id);
int	ocifbsd_start_container(const char *container_id);
int	ocifbsd_stop_container(const char *cid, int sig);
int	ocifbsd_delete_container(const char *cid, bool force);
int	ocifbsd_get_container_state(const char *cid, int *state,
	    int *exit_code);
int	ocifbsd_logs(const char *cid, int tail, bool follow);

int
ocifbsd_create_container(const char *name, const char *image,
    const char *command, char **args, uid_t uid, char **container_id)
{
	(void)name;
	(void)image;
	(void)command;
	(void)args;
	(void)uid;
	(void)container_id;
	syslog(LOG_WARNING,
	    "ocifbsd_create_container(%s) stub: no backing runtime", name);
	return (-1);
}

int
ocifbsd_start_container(const char *container_id)
{
	(void)container_id;
	syslog(LOG_WARNING,
	    "ocifbsd_start_container(%s) stub: no backing runtime",
	    container_id);
	return (-1);
}

int
ocifbsd_stop_container(const char *cid, int sig)
{
	(void)cid;
	(void)sig;
	syslog(LOG_WARNING,
	    "ocifbsd_stop_container(%s) stub: no backing runtime", cid);
	return (-1);
}

int
ocifbsd_delete_container(const char *cid, bool force)
{
	(void)cid;
	(void)force;
	syslog(LOG_WARNING,
	    "ocifbsd_delete_container(%s) stub: no backing runtime", cid);
	return (-1);
}

int
ocifbsd_get_container_state(const char *cid, int *state, int *exit_code)
{
	(void)cid;
	if (state != NULL)
		*state = 0;
	if (exit_code != NULL)
		*exit_code = 0;
	syslog(LOG_WARNING,
	    "ocifbsd_get_container_state(%s) stub: no backing runtime", cid);
	return (-1);
}

int
ocifbsd_logs(const char *cid, int tail, bool follow)
{
	(void)cid;
	(void)tail;
	(void)follow;
	syslog(LOG_WARNING,
	    "ocifbsd_logs(%s) stub: no backing runtime", cid);
	return (-1);
}
