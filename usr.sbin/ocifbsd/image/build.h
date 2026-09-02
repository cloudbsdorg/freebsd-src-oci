/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * ocifbsd build: assemble an image from a Containerfile/Dockerfile.
 */

#ifndef _OCIFBSD_IMAGE_BUILD_H
#define _OCIFBSD_IMAGE_BUILD_H

/*
 * Build an image from `containerfile` using `context` as the build context
 * directory and tag it `tag` (name[:tag]) in the local image store. Executes
 * FROM/RUN/COPY/ADD/ENV/WORKDIR/USER/CMD/ENTRYPOINT/EXPOSE/LABEL instructions.
 * RUN steps execute inside the assembled rootfs (chroot with devfs + the host
 * resolver, so pkg(8) works over the host's network). Returns 0 on success,
 * non-zero on any failed instruction. When verbose, each step is echoed.
 */
int	image_build(const char *containerfile, const char *context,
	    const char *tag, int verbose);

#endif /* _OCIFBSD_IMAGE_BUILD_H */
