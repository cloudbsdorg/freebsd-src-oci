/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * $FreeBSD$
 *
 * JSON pretty-printing for human-facing CLI output. `ocifbsd --pretty`
 * renders JSON (inspect/state) indented instead of as a single compact line.
 */

#ifndef _OCIFBSD_JSONFMT_H
#define _OCIFBSD_JSONFMT_H

/*
 * Reformat a JSON document into an indented, human-readable string. Returns a
 * malloc'd NUL-terminated string the caller frees, or NULL if the input is not
 * valid JSON (callers then fall back to printing the original text).
 */
char	*ocifbsd_json_pretty(const char *json);

#endif /* _OCIFBSD_JSONFMT_H */
