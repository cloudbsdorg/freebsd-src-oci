/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 CloudBSD
 *
 * Unit tests for usr.sbin/ocifbsd/src/hooks.c
 */

#include <sys/param.h>

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ocifbsd.h"
#include "src/hooks.c"

ATF_TC(hooks_null_container);
ATF_TC_HEAD(hooks_null_container, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "hooks_run_* on NULL container are no-ops");
}
ATF_TC_BODY(hooks_null_container, tc)
{
	ATF_CHECK_EQ(hooks_run_prestart(NULL), 0);
	ATF_CHECK_EQ(hooks_run_poststart(NULL), 0);
	ATF_CHECK_EQ(hooks_run_poststop(NULL), 0);
}

ATF_TC(hooks_no_spec);
ATF_TC_HEAD(hooks_no_spec, tc)
{
	atf_tc_set_md_var(tc, "descr", "container without hooks is no-op");
}
ATF_TC_BODY(hooks_no_spec, tc)
{
	struct ocifbsd_container c;

	memset(&c, 0, sizeof(c));
	ATF_CHECK_EQ(hooks_run_prestart(&c), 0);
	ATF_CHECK_EQ(hooks_run_poststart(&c), 0);
	ATF_CHECK_EQ(hooks_run_poststop(&c), 0);
}

ATF_TC(hooks_prestart_runs_true);
ATF_TC_HEAD(hooks_prestart_runs_true, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "prestart hook /usr/bin/true (or /bin/true) succeeds");
}
ATF_TC_BODY(hooks_prestart_runs_true, tc)
{
	struct ocifbsd_container c;
	struct oci_runtime_spec spec;
	struct oci_hooks hooks;
	struct oci_hook hook;
	struct oci_hook *hookp;
	const char *true_path = NULL;

	if (access("/usr/bin/true", X_OK) == 0)
		true_path = "/usr/bin/true";
	else if (access("/bin/true", X_OK) == 0)
		true_path = "/bin/true";
	else
		atf_tc_skip("no true binary");

	memset(&c, 0, sizeof(c));
	memset(&spec, 0, sizeof(spec));
	memset(&hooks, 0, sizeof(hooks));
	memset(&hook, 0, sizeof(hook));

	hook.path = __DECONST(char *, true_path);
	hookp = &hook;
	hooks.prestart = &hookp;
	hooks.n_prestart = 1;
	spec.hooks = &hooks;
	c.spec = &spec;
	c.bundle_path = ".";

	ATF_CHECK_EQ(hooks_run_prestart(&c), 0);
}

ATF_TC(hooks_prestart_missing_path);
ATF_TC_HEAD(hooks_prestart_missing_path, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "missing hook binary yields non-zero (warning path)");
}
ATF_TC_BODY(hooks_prestart_missing_path, tc)
{
	struct ocifbsd_container c;
	struct oci_runtime_spec spec;
	struct oci_hooks hooks;
	struct oci_hook hook;
	struct oci_hook *hookp;

	memset(&c, 0, sizeof(c));
	memset(&spec, 0, sizeof(spec));
	memset(&hooks, 0, sizeof(hooks));
	memset(&hook, 0, sizeof(hook));

	hook.path = __DECONST(char *, "/nonexistent/ocifbsd-hook");
	hookp = &hook;
	hooks.prestart = &hookp;
	hooks.n_prestart = 1;
	spec.hooks = &hooks;
	c.spec = &spec;
	c.bundle_path = ".";

	/* execute_hook returns 127 on exec failure after wait */
	ATF_CHECK(hooks_run_prestart(&c) != 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, hooks_null_container);
	ATF_TP_ADD_TC(tp, hooks_no_spec);
	ATF_TP_ADD_TC(tp, hooks_prestart_runs_true);
	ATF_TP_ADD_TC(tp, hooks_prestart_missing_path);
	return (atf_no_error());
}
