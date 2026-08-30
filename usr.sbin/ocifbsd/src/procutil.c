/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * $FreeBSD$
 *
 * Process/jail introspection helpers.
 *
 * This file deliberately avoids the strict _POSIX_C_SOURCE feature-test
 * macro used by the rest of the runtime: <sys/user.h> (needed for
 * struct kinfo_proc) pulls in <sys/proc.h>, which references stack_t and
 * only compiles with BSD visibility. The build assigns per-file CFLAGS
 * that drop _POSIX_C_SOURCE for this translation unit.
 */

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <stdbool.h>

bool pid_in_jail(pid_t pid, int jid);

/*
 * Return true if process `pid` currently exists and, when jid > 0, belongs
 * to jail `jid`. Queries the kernel via sysctl(KERN_PROC_PID). On any
 * inconclusive result (no such process, lookup failure) we return false so
 * callers treat the stored PID as no longer ours — this prevents signaling
 * a recycled PID after a container's init has exited and been reaped by
 * init(8).
 */
bool
pid_in_jail(pid_t pid, int jid)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
	struct kinfo_proc kp;
	size_t len = sizeof(kp);

	if (pid <= 0)
		return (false);
	if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0)
		return (false);		/* no such process */
	if (len == 0)
		return (false);
	if (jid > 0 && kp.ki_jid != jid)
		return (false);
	return (true);
}
