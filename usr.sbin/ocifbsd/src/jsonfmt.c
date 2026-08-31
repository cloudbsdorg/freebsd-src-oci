/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * $FreeBSD$
 *
 * JSON pretty-printing for human-facing CLI output (see jsonfmt.h).
 */

#include <stdlib.h>
#include <string.h>

#include <json.h>

#include "jsonfmt.h"

char *
ocifbsd_json_pretty(const char *json)
{
	struct json_object *o;
	const char *rendered;
	char *out = NULL;

	if (json == NULL)
		return (NULL);
	o = json_tokener_parse(json);
	if (o == NULL)
		return (NULL);
	rendered = json_object_to_json_string_ext(o,
	    JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED |
	    JSON_C_TO_STRING_NOSLASHESCAPE);
	if (rendered != NULL)
		out = strdup(rendered);
	json_object_put(o);
	return (out);
}
