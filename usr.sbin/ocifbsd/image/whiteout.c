/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 The FreeBSD Foundation
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Pure OCI/Docker whiteout name helpers (no filesystem).
 */

#include <sys/param.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "unpack.h"

#define WHITEOUT_PREFIX		".wh."
#define WHITEOUT_PREFIX_LEN	4

bool
is_whiteout(const char *filename)
{
	if (filename == NULL)
		return (false);
	return (strncmp(filename, WHITEOUT_PREFIX, WHITEOUT_PREFIX_LEN) == 0);
}

/*
 * .wh.foo -> foo
 * .wh..wh..opq -> NULL (opaque directory marker)
 */
char *
get_whiteout_target(const char *whiteout_name)
{
	char *target;
	size_t len;

	if (whiteout_name == NULL)
		return (NULL);

	if (strcmp(whiteout_name, ".wh..wh..opq") == 0)
		return (NULL);

	if (strncmp(whiteout_name, WHITEOUT_PREFIX, WHITEOUT_PREFIX_LEN) != 0)
		return (NULL);

	len = strlen(whiteout_name) - WHITEOUT_PREFIX_LEN + 1;
	target = malloc(len);
	if (target == NULL)
		return (NULL);

	strlcpy(target, whiteout_name + WHITEOUT_PREFIX_LEN, len);
	return (target);
}
