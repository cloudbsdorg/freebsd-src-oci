/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * RACCT/rctl(8) resource-usage capture for `ocifbsd stats`. Extracted from the
 * CLI dispatcher so the rctl plumbing lives on its own rather than inside
 * ocifbsd.c.
 */

#include <sys/wait.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../include/ocifbsd.h"

/*
 * Capture `rctl -u jail:<jailname>` output into buf. rctl reports live RACCT
 * resource usage for the jail (cputime, memoryuse, nthr, ...). Returns 0 on
 * success (buf NUL-terminated), -1 otherwise. jailname is ocifbsd-<hexid>, so
 * no shell is involved and the argument is not attacker-controlled.
 */
int
capture_rctl(const char *jailname, char *buf, size_t buflen)
{
	int fds[2], status;
	pid_t pid;
	char rule[80];
	size_t o = 0;

	if (buflen == 0 || pipe(fds) != 0)
		return (-1);
	snprintf(rule, sizeof(rule), "jail:%s", jailname);
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return (-1);
	}
	if (pid == 0) {
		int dn;

		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		dn = open("/dev/null", O_WRONLY);
		if (dn >= 0)
			dup2(dn, STDERR_FILENO);
		/*
		 * Fixed path, not execlp: this runs as root, so resolving
		 * "rctl" through $PATH would let an attacker-controlled PATH
		 * substitute a trojan binary (the mount helpers use /sbin/mount
		 * for the same reason).
		 */
		execl("/usr/bin/rctl", "rctl", "-u", rule, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	for (;;) {
		ssize_t r = read(fds[0], buf + o, buflen - 1 - o);

		if (r <= 0)
			break;
		o += (size_t)r;
		if (o >= buflen - 1)
			break;
	}
	buf[o] = '\0';
	close(fds[0]);
	waitpid(pid, &status, 0);
	return (0);
}

/* Extract the unsigned value of "<key>=" from an rctl -u dump (0 if absent). */
uintmax_t
rctl_field(const char *buf, const char *key)
{
	char pat[48];
	const char *p;

	snprintf(pat, sizeof(pat), "%s=", key);
	for (p = strstr(buf, pat); p != NULL; p = strstr(p + 1, pat)) {
		if (p == buf || p[-1] == '\n')
			return (strtoumax(p + strlen(pat), NULL, 10));
	}
	return (0);
}
