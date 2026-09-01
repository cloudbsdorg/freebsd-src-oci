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
 * ZFS storage backend for OCI images
 */

#ifndef _OCIFBSD_IMAGE_ZFS_STORE_H
#define _OCIFBSD_IMAGE_ZFS_STORE_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Dataset layout:
 *   ocifbsd/images/<registry>/<image>/<tag>@<digest>  (leaf dataset)
 *   ocifbsd/images/<registry>/<image>/<tag>@saved     (snapshot for caching)
 *   ocifbsd/layers/<digest>                           (shared layer storage)
 *   ocifbsd/volumes/<volume>                          (volume datasets)
 */

#define OCIFBSD_ZFS_POOL	"zroot"
#define OCIFBSD_ZFS_IMAGES	"ocifbsd/images"
#define OCIFBSD_ZFS_LAYERS	"ocifbsd/layers"
#define OCIFBSD_ZFS_VOLUMES	"ocifbsd/volumes"
#define OCIFBSD_ZFS_CONFIGS	"ocifbsd/configs"

/*
 * Image layer descriptor
 */
struct zfs_layer {
	char		*digest;		/* sha256:<hex> */
	char		*dataset;		/* ZFS dataset path */
	char		*mountpoint;		/* where it's mounted */
	uint64_t	size;			/* uncompressed size */
	bool		shared;			/* can be shared between images */
	bool		compressed;		/* uses compression */
};

/*
 * Image descriptor
 */
struct zfs_image {
	char		*registry;
	char		*repository;
	char		*tag;
	char		*digest;		/* manifest digest */
	char		*dataset;	/* ZFS dataset path */
	char		*mountpoint;	/* rootfs mount point */
	struct zfs_layer **layers;
	int		nlayers;
	uint64_t	size;		/* total size on disk */
};

/*
 * Volume descriptor
 */
struct zfs_volume {
	char		*name;
	char		*dataset;
	char		*mountpoint;
	uint64_t	size;
	bool		encrypted;
	bool		readonly;
};

/*
 * ZFS store operations
 */
int	 zfs_store_init(void);
int	 zfs_store_ensure_dataset(const char *dataset, const char *mountpoint);
int	 zfs_store_create_image(const char *registry, const char *repo,
	     const char *tag, const char *digest);
int	 zfs_store_clone_layer(const char *src_digest, const char *dst_digest);
int	 zfs_store_add_layer(const char *image_dataset, const char *layer_digest);
int	 zfs_store_destroy_image(const char *registry, const char *repo,
	     const char *tag);
int	 zfs_store_list_images(struct zfs_image ***images, int *nimages);
int	 zfs_store_get_image(const char *registry, const char *repo,
	     const char *tag, struct zfs_image **image);
int	 zfs_store_snapshot(const char *dataset, const char *snapshot);
int	 zfs_store_rollback(const char *dataset, const char *snapshot);
int	 zfs_store_send(const char *dataset, const char *snapshot, int fd);
int	 zfs_store_recv(const char *dataset, int fd);
int	 zfs_store_get_usage(uint64_t *used, uint64_t *available);
int	 zfs_store_cleanup_unused(void);

/*
 * Layer operations
 */
int	 zfs_layer_create(const char *digest, uint64_t size);
int	 zfs_layer_destroy(const char *digest);
int	 zfs_layer_mount(const char *digest, char **mountpoint);
int	 zfs_layer_umount(const char *digest);
int	 zfs_layer_add_files(const char *digest, const char *srcdir);
struct zfs_layer *zfs_layer_get(const char *digest);
void	 zfs_layer_free(struct zfs_layer *layer);
bool	 zfs_layer_exists(const char *digest);
uint64_t zfs_layer_get_size(const char *digest);
int	 zfs_layer_set_shared(const char *digest, bool shared);

/*
 * Volume operations
 */
int	 zfs_volume_create(const char *name, uint64_t size, bool encrypted);
int	 zfs_volume_destroy(const char *name);
int	 zfs_volume_mount(const char *name, char **mountpoint);
int	 zfs_volume_umount(const char *name);
int	 zfs_volume_snapshot(const char *name, const char *snapname);
int	 zfs_volume_rollback(const char *name, const char *snapname);
struct zfs_volume *zfs_volume_get(const char *name);
void	 zfs_volume_free(struct zfs_volume *vol);
int	 zfs_volume_list(struct zfs_volume ***volumes, int *nvolumes);

/*
 * Utility functions
 */
const char *zfs_get_pool(void);
const char *zfs_get_images_dataset(void);
const char *zfs_get_layers_dataset(void);
const char *zfs_get_volumes_dataset(void);
char *zfs_image_path(const char *registry, const char *repo, const char *tag);
char *zfs_layer_path(const char *digest);
char *zfs_volume_path(const char *name);
int	 zfs_set_property(const char *dataset, const char *property,
	     const char *value);
int	 zfs_get_property(const char *dataset, const char *property,
	     char **value);
int	 zfs_dataset_exists(const char *dataset);
int	 zfs_destroy_dataset(const char *dataset, bool recursive);

#endif /* _OCIFBSD_IMAGE_ZFS_STORE_H */
