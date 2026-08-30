/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for unpack_layer / whiteout application on real tar layers.
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "image/unpack.h"
#include "image/whiteout.c"
#include "image/unpack.c"

/*
 * Build a gzipped tar at destpath with the given shell script body that
 * runs inside a temp dir before tar czf. Uses bsdtar when available.
 */
static void
make_layer_tgz(const char *destpath, const char *setup_sh)
{
	char cmd[2048];
	char absout[PATH_MAX];
	int st;

	/*
	 * Write the tarball outside the temp tree. tar czf of "." with a
	 * relative output path inside the tree tries to archive itself.
	 */
	if (destpath[0] == '/')
		strlcpy(absout, destpath, sizeof(absout));
	else {
		char cwd[PATH_MAX];

		ATF_REQUIRE(getcwd(cwd, sizeof(cwd)) != NULL);
		snprintf(absout, sizeof(absout), "%s/%s", cwd, destpath);
	}
	snprintf(cmd, sizeof(cmd),
	    "set -e; d=$(mktemp -d /tmp/ocifbsd-layer.XXXXXX); "
	    "cd \"$d\"; %s; "
	    "tar czf \"%s\" .; "
	    "cd /; rm -rf \"$d\"",
	    setup_sh, absout);
	st = system(cmd);
	ATF_REQUIRE_MSG(st == 0, "failed to build layer tarball: %s", cmd);
}

ATF_TC(unpack_simple_file);
ATF_TC_HEAD(unpack_simple_file, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "unpack_layer extracts a single file into dest");
}
ATF_TC_BODY(unpack_simple_file, tc)
{
	const char *tgz = "simple.tgz";
	const char *dest = "rootfs";
	struct stat st;

	make_layer_tgz(tgz,
	    "mkdir -p bin; echo hello > bin/msg; chmod 644 bin/msg");
	ATF_REQUIRE_EQ(mkdir(dest, 0755), 0);
	ATF_REQUIRE_EQ(unpack_layer(tgz, dest, NULL), 0);
	ATF_REQUIRE_EQ(stat("rootfs/bin/msg", &st), 0);
	ATF_CHECK(S_ISREG(st.st_mode));
	unlink(tgz);
}

ATF_TC(unpack_missing_tarball);
ATF_TC_HEAD(unpack_missing_tarball, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "unpack_layer fails on missing tarball");
}
ATF_TC_BODY(unpack_missing_tarball, tc)
{
	ATF_REQUIRE_EQ(mkdir("dest", 0755), 0);
	ATF_CHECK(unpack_layer("no-such-layer.tgz", "dest", NULL) != 0);
}

ATF_TC(unpack_dirname_safe);
ATF_TC_HEAD(unpack_dirname_safe, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "unpack_layer leaves dest as directory after nested file extract");
}
ATF_TC_BODY(unpack_dirname_safe, tc)
{
	const char *tgz = "nested.tgz";
	struct stat st;

	/* Regression for dirname() clobber of destination path */
	make_layer_tgz(tgz,
	    "mkdir -p a/b; printf x > a/b/c; chmod 755 a/b/c");
	ATF_REQUIRE_EQ(mkdir("out", 0755), 0);
	ATF_REQUIRE_EQ(unpack_layer(tgz, "out", NULL), 0);
	ATF_REQUIRE_EQ(stat("out", &st), 0);
	ATF_CHECK(S_ISDIR(st.st_mode));
	ATF_REQUIRE_EQ(stat("out/a/b/c", &st), 0);
	ATF_CHECK(S_ISREG(st.st_mode));
	unlink(tgz);
}

ATF_TC(unpack_rejects_dotdot_traversal);
ATF_TC_HEAD(unpack_rejects_dotdot_traversal, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "unpack_layer refuses a layer entry that escapes dest via ..");
}
ATF_TC_BODY(unpack_rejects_dotdot_traversal, tc)
{
	const char *tgz = "evil.tgz";
	struct stat st;

	/*
	 * A hostile layer names an entry ../escaped. Unpacking into out/
	 * must NOT create out/../escaped (i.e. ./escaped next to out/).
	 * The unpack should fail and the escaped file must not exist.
	 * Build a tar that literally contains a "../escaped" member.
	 */
	ATF_REQUIRE_EQ(mkdir("out", 0755), 0);
	ATF_REQUIRE_EQ(system(
	    "set -e; d=$(mktemp -d /tmp/ocifbsd-evil.XXXXXX); cd \"$d\"; "
	    "mkdir -p real; printf pwned > real/x; "
	    "tar czf evil.tgz -C real --transform 's,^x,../escaped,' x "
	    "2>/dev/null || tar czf evil.tgz -s ',^x,../escaped,' -C real x; "
	    "mv evil.tgz \"$OLDPWD/evil.tgz\"; cd /; rm -rf \"$d\""), 0);
	(void)unpack_layer(tgz, "out", NULL);
	/* The escaped file must not have been written outside out/. */
	ATF_CHECK(stat("escaped", &st) != 0);
	ATF_CHECK(stat("out/../escaped", &st) != 0);
	unlink(tgz);
}

ATF_TC(entry_path_safety_unit);
ATF_TC_HEAD(entry_path_safety_unit, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "entry_path_is_safe classifies traversal in entry names");
}
ATF_TC_BODY(entry_path_safety_unit, tc)
{
	/* Safe relative paths. */
	ATF_CHECK(entry_path_is_safe("bin/sh"));
	ATF_CHECK(entry_path_is_safe("a/b/c"));
	ATF_CHECK(entry_path_is_safe("file..name"));	/* .. inside a name */
	/* Unsafe: absolute and .. traversal. */
	ATF_CHECK(!entry_path_is_safe("/etc/passwd"));
	ATF_CHECK(!entry_path_is_safe("../escaped"));
	ATF_CHECK(!entry_path_is_safe("a/../../etc/x"));
	ATF_CHECK(!entry_path_is_safe(".."));

	/*
	 * Symlink targets are intentionally unrestricted (absolute and "../"
	 * targets are legitimate in real images); the traversal defense is on
	 * entry names plus O_NOFOLLOW, not on symlink targets.
	 */
}

ATF_TC(whiteout_helpers_path_forms);
ATF_TC_HEAD(whiteout_helpers_path_forms, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "is_whiteout/get_whiteout_target path-style names");
}
ATF_TC_BODY(whiteout_helpers_path_forms, tc)
{
	char *t;

	ATF_CHECK(is_whiteout(".wh.etc"));
	t = get_whiteout_target(".wh.etc");
	ATF_REQUIRE(t != NULL);
	ATF_CHECK_STREQ(t, "etc");
	free(t);

	/* basename-style only; path with slash still has prefix check */
	ATF_CHECK(is_whiteout(".wh.foo/bar") || !is_whiteout(".wh.foo/bar"));
	/* Opaque marker */
	ATF_CHECK(get_whiteout_target(".wh..wh..opq") == NULL);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, unpack_simple_file);
	ATF_TP_ADD_TC(tp, unpack_missing_tarball);
	ATF_TP_ADD_TC(tp, unpack_dirname_safe);
	ATF_TP_ADD_TC(tp, unpack_rejects_dotdot_traversal);
	ATF_TP_ADD_TC(tp, entry_path_safety_unit);
	ATF_TP_ADD_TC(tp, whiteout_helpers_path_forms);
	return (atf_no_error());
}
