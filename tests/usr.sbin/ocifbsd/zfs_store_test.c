/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Live ZFS dataset-operation tests for the image store
 * (usr.sbin/ocifbsd/image/zfs_store.c). These drive real zfs(8), so they need
 * root and a throwaway pool named by OCIFBSD_ZFS_POOL; both absent, they skip.
 * Create such a pool with, e.g.:
 *     truncate -s 256m /tmp/ocitest.img
 *     zpool create ocitest /tmp/ocitest.img
 *     env OCIFBSD_ZFS_POOL=ocitest kyua test zfs_store_test
 */

#include <atf-c.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* zfs_store.c externs these from the product's utils.c; stub for the test. */
int mkdirp(const char *path, mode_t mode);
int copy_file(const char *from, const char *to, int mode);

int
mkdirp(const char *path, mode_t mode)
{
	char buf[1024];
	size_t i, len;

	len = strlcpy(buf, path, sizeof(buf));
	if (len >= sizeof(buf))
		return (-1);
	for (i = 1; i < len; i++) {
		if (buf[i] == '/') {
			buf[i] = '\0';
			if (mkdir(buf, mode) != 0 && errno != EEXIST)
				return (-1);
			buf[i] = '/';
		}
	}
	if (mkdir(buf, mode) != 0 && errno != EEXIST)
		return (-1);
	return (0);
}

int
copy_file(const char *from __unused, const char *to __unused, int mode __unused)
{
	return (0);
}

/*
 * zfs_store.c references these path helpers from image/paths.c, which cannot be
 * co-included here (its static mountpoint_base() clashes with zfs_store.c's
 * mountpoint_base variable). The dataset-op tests below never exercise the
 * layer/volume path code, so trivial stubs satisfy the link.
 */
char *zfs_layer_path(const char *digest);
char *zfs_volume_path(const char *name);

char *
zfs_layer_path(const char *digest __unused)
{
	return (strdup("/var/lib/ocifbsd/layers/stub"));
}

char *
zfs_volume_path(const char *name __unused)
{
	return (strdup("/var/lib/ocifbsd/volumes/stub"));
}

#include "image/zfs_store.c"

/* Skip unless we can actually operate on a real test pool as root. */
static const char *
require_pool(const struct atf_tc *tc)
{
	const char *pool = getenv("OCIFBSD_ZFS_POOL");
	char *out = NULL;
	char *argv[4];

	if (pool == NULL || pool[0] == '\0')
		atf_tc_skip("set OCIFBSD_ZFS_POOL to a throwaway test pool");
	if (geteuid() != 0)
		atf_tc_skip("requires root for zfs(8)");
	/* Confirm the pool exists so we fail as skip, not error. */
	argv[0] = (char *)"zfs"; argv[1] = (char *)"list";
	argv[2] = (char *)pool; argv[3] = NULL;
	if (run_zfs_capture(argv, &out) != 0) {
		free(out);
		atf_tc_skip("pool %s not importable", pool);
	}
	free(out);
	(void)tc;
	return (pool);
}

ATF_TC(dataset_lifecycle);
ATF_TC_HEAD(dataset_lifecycle, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "zfs_store_init creates the base datasets and create/destroy_image "
	    "add and remove an image dataset");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(dataset_lifecycle, tc)
{
	const char *pool = require_pool(tc);
	char ds[PATH_MAX];
	char *argv[5];
	int rc;

	/* Base datasets. */
	ATF_REQUIRE_EQ(0, zfs_store_init());
	ATF_CHECK(zfs_dataset_exists(zfs_get_images_dataset()));
	ATF_CHECK(zfs_dataset_exists(zfs_get_layers_dataset()));

	/* Create an image dataset and confirm it exists. */
	ATF_REQUIRE_EQ(0, zfs_store_create_image("reg.test", "team/app",
	    "v1", "sha256:0123456789abcdef"));
	ATF_REQUIRE_EQ(0, make_image_dataset("reg.test", "team/app", "v1",
	    ds, sizeof(ds)));
	ATF_CHECK_MSG(zfs_dataset_exists(ds),
	    "image dataset %s was not created", ds);

	/* Destroy it and confirm it is gone. */
	ATF_REQUIRE_EQ(0, zfs_store_destroy_image("reg.test", "team/app",
	    "v1"));
	ATF_CHECK_MSG(!zfs_dataset_exists(ds),
	    "image dataset %s survived destroy", ds);

	/* Clean the base datasets we created (best effort). */
	argv[0] = (char *)"zfs"; argv[1] = (char *)"destroy";
	argv[2] = (char *)"-r"; argv[3] = (char *)zfs_get_images_dataset();
	argv[4] = NULL;
	rc = run_zfs_capture(argv, NULL);
	(void)rc; (void)pool;
}

ATF_TC(snapshot_rollback);
ATF_TC_HEAD(snapshot_rollback, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "zfs_store_snapshot then zfs_store_rollback succeed on a dataset");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(snapshot_rollback, tc)
{
	const char *pool = require_pool(tc);
	char ds[PATH_MAX];
	char *argv[5];

	ATF_REQUIRE_EQ(0, zfs_store_init());
	ATF_REQUIRE_EQ(0, zfs_store_create_image("reg.test", "snap/app",
	    "v1", "sha256:abc"));
	ATF_REQUIRE_EQ(0, make_image_dataset("reg.test", "snap/app", "v1",
	    ds, sizeof(ds)));

	ATF_CHECK_EQ(0, zfs_store_snapshot(ds, "snap0"));
	ATF_CHECK_EQ(0, zfs_store_rollback(ds, "snap0"));

	(void)zfs_store_destroy_image("reg.test", "snap/app", "v1");
	argv[0] = (char *)"zfs"; argv[1] = (char *)"destroy";
	argv[2] = (char *)"-r"; argv[3] = (char *)zfs_get_images_dataset();
	argv[4] = NULL;
	(void)run_zfs_capture(argv, NULL);
	(void)pool;
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, dataset_lifecycle);
	ATF_TP_ADD_TC(tp, snapshot_rollback);

	return (atf_no_error());
}
